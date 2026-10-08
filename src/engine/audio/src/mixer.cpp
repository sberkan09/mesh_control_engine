#include "mixer.h"

#include <cmath>
#include <cstring>
#include <initializer_list>

#include "mce/math/math.h"

namespace mce::audio::detail {

namespace {

// Channel gains for a voice: mono sources pan with constant power; stereo sources balance.
void pan_gains(f32 gain, f32 pan, bool stereo, f32& l, f32& r) {
    pan = math::clamp(pan, -1.f, 1.f);
    if (stereo) {
        l = gain * (pan > 0.f ? 1.f - pan : 1.f);
        r = gain * (pan < 0.f ? 1.f + pan : 1.f);
    } else {
        const f32 theta = (pan + 1.f) * (math::kPi * 0.25f);
        l = gain * std::cos(theta);
        r = gain * std::sin(theta);
    }
}

}  // namespace

Mixer::Mixer(RingBuffer<AudioCommand>& commands, RingBuffer<AudioReply>& replies, StreamRing* const* streams)
    : commands_(commands), replies_(replies), streams_(streams) {
    std::memset(bus_buf_, 0, sizeof(bus_buf_));
}

// ---------------------------------------------------------------------------- commands

void Mixer::process_commands() {
    AudioCommand c;
    for (u32 n = 0; n < kMaxCommandsPerCallback && commands_.pop(c); ++n) {
        switch (c.type) {
            case CmdType::Play: cmd_play(c); break;
            case CmdType::Stop: cmd_stop(c); break;
            case CmdType::VoiceGain:
            case CmdType::VoicePan:
                if (VoiceRT* v = find_voice(c.index, c.voice.generation); v && !v->ending) {
                    if (c.type == CmdType::VoiceGain) v->gain = c.voice.gain;
                    else v->pan = c.voice.pan;
                    set_targets(*v, kRampFrames);
                }
                break;
            case CmdType::BusGain:
                if (c.index < kBusCount) {
                    BusRT& b = bus_[c.index];
                    b.target = c.voice.gain;
                    b.ramp_left = kRampFrames;
                    b.inc = (b.target - b.cur) / static_cast<f32>(kRampFrames);
                }
                break;
            case CmdType::RegisterClip: {
                ClipRT& k = clips_[c.index];
                k.frames = c.clip.frames;
                k.frame_count = c.clip.frame_count;
                k.loop_start = c.clip.loop_start;
                k.channels = c.bus;
                k.stream = static_cast<i8>(static_cast<i32>(c.priority) - 1);
                break;
            }
            case CmdType::ReleaseClip: {
                // The clip's memory goes away after our reply: cut every voice reading it now.
                for (VoiceRT* set : {real_, tails_})
                    for (u32 i = 0; i < kMaxRealVoices; ++i)
                        if (set[i].active && set[i].clip == c.index) end_voice(set[i]);
                for (VoiceRT& v : virtual_)
                    if (v.active && v.clip == c.index) end_voice(v);
                clips_[c.index] = ClipRT{};
                AudioReply r;
                r.type = ReplyType::ClipReleased;
                r.index = c.index;
                replies_.push(r);
                break;
            }
            case CmdType::StopAll:
                for (VoiceRT& v : real_)
                    if (v.active && !v.ending) {
                        v.ending = true;
                        set_targets(v, kRampFrames);
                    }
                for (VoiceRT& v : virtual_)
                    if (v.active) end_voice(v);
                break;
        }
    }
}

VoiceRT* Mixer::find_voice(u32 index, u32 gen) {
    for (VoiceRT& v : real_)
        if (v.active && v.handle_index == index && v.handle_gen == gen) return &v;
    for (VoiceRT& v : virtual_)
        if (v.active && v.handle_index == index && v.handle_gen == gen) return &v;
    return nullptr;
}

void Mixer::end_voice(VoiceRT& v) {
    if (!v.active) return;
    v.active = false;
    AudioReply r;
    r.type = ReplyType::VoiceEnded;
    r.index = v.handle_index;
    r.generation = v.handle_gen;
    // Cannot fail: the reply ring holds more entries than handles + clips can ever have pending.
    const bool pushed = replies_.push(r);
    MCE_ASSERT_MSG(pushed, "audio reply ring overflow");
    (void)pushed;
}

void Mixer::set_targets(VoiceRT& v, u32 ramp_frames) {
    f32 tl = 0.f, tr = 0.f;
    if (!v.ending) pan_gains(v.gain, v.pan, clips_[v.clip].channels == 2 || clips_[v.clip].stream >= 0, tl, tr);
    if (ramp_frames == 0) {
        v.cur_l = tl;
        v.cur_r = tr;
        v.ramp_left = 0;
        return;
    }
    v.ramp_left = ramp_frames;
    v.inc_l = (tl - v.cur_l) / static_cast<f32>(ramp_frames);
    v.inc_r = (tr - v.cur_r) / static_cast<f32>(ramp_frames);
}

void Mixer::cmd_play(const AudioCommand& c) {
    VoiceRT nv;
    nv.handle_index = c.index;
    nv.handle_gen = c.voice.generation;
    nv.clip = c.voice.clip;
    nv.gain = c.voice.gain;
    nv.pan = c.voice.pan;
    nv.priority = c.priority;
    nv.bus = c.bus < kBusCount ? c.bus : u8(Bus_SFX);
    nv.loop = (c.flags & CmdFlag::Loop) != 0;
    nv.active = true;
    nv.order = next_order_++;

    if (nv.clip >= kMaxClips || clips_[nv.clip].channels == 0 || clips_[nv.clip].frame_count == 0) {
        end_voice(nv);  // stale clip: the handle still has to come back
        return;
    }
    set_targets(nv, 0);  // one-shots start at full level: no softened transients

    // 1) a free real voice
    for (VoiceRT& v : real_)
        if (!v.active) {
            v = nv;
            return;
        }

    // 2) steal: voices already fading out first, then by priority (less important first),
    //    quieter first, older first. Never steal a more important voice; protected voices
    //    (music) only yield to other protected voices.
    VoiceRT* victim = nullptr;
    for (VoiceRT& v : real_) {
        if (v.priority < nv.priority && !v.ending) continue;
        if (v.priority < kProtectedPriority && nv.priority >= kProtectedPriority && !v.ending) continue;
        if (!victim) {
            victim = &v;
            continue;
        }
        if (v.ending != victim->ending) {
            if (v.ending) victim = &v;
            continue;
        }
        if (v.priority != victim->priority) {
            if (v.priority > victim->priority) victim = &v;
            continue;
        }
        if (v.gain != victim->gain) {
            if (v.gain < victim->gain) victim = &v;
            continue;
        }
        if (v.order < victim->order) victim = &v;
    }
    if (victim) {
        VoiceRT& tail = tails_[victim - real_];
        if (tail.active) end_voice(tail);  // a previous tail still fading: let it go
        tail = *victim;
        if (!tail.ending) {
            ++steals_;
            tail.ending = true;
        }
        set_targets(tail, kStealFadeFrames);
        *victim = nv;
        return;
    }

    // 3) virtual: tracked and advanced, not mixed; promoted when a real voice frees
    for (VoiceRT& v : virtual_)
        if (!v.active) {
            v = nv;
            return;
        }
    ++dropped_;
    end_voice(nv);
}

void Mixer::cmd_stop(const AudioCommand& c) {
    VoiceRT* v = find_voice(c.index, c.voice.generation);
    if (!v) return;
    if (v >= virtual_ && v < virtual_ + kMaxVirtualVoices) {
        end_voice(*v);
        return;
    }
    v->ending = true;
    const u32 fade = static_cast<u32>(c.voice.pan);
    set_targets(*v, fade > kMinStopFadeFrames ? fade : kMinStopFadeFrames);
}

// ---------------------------------------------------------------------------- mixing

void Mixer::advance_virtual(VoiceRT& v, u32 frames) {
    const ClipRT& clip = clips_[v.clip];
    v.pos += frames;
    if (clip.stream >= 0) {
        u64 f;
        if (!StreamRing::map_frame(streams_[clip.stream]->source, v.loop, v.pos, f)) {
            end_voice(v);
            return;
        }
        streams_[clip.stream]->consumed.store(v.pos / kStreamPageFrames, std::memory_order_release);
        return;
    }
    if (v.pos < clip.frame_count) return;
    if (!v.loop || clip.loop_start >= clip.frame_count) {
        end_voice(v);
        return;
    }
    const u64 body = clip.frame_count - clip.loop_start;
    v.pos = clip.loop_start + (v.pos - clip.frame_count) % body;
}

void Mixer::mix_voice(VoiceRT& v, f32* dst, u32 frames) {
    const ClipRT& clip = clips_[v.clip];
    StreamRing* ring = clip.stream >= 0 ? streams_[clip.stream] : nullptr;
    u64 filled = ring ? ring->filled.load(std::memory_order_acquire) : 0;
    for (u32 i = 0; i < frames; ++i) {
        f32 sl, sr;
        if (ring) {
            const u64 page = v.pos / kStreamPageFrames;
            if (page >= filled) filled = ring->filled.load(std::memory_order_acquire);
            if (page < filled) {
                const f32* p = ring->pages[page % kStreamPages] + (v.pos % kStreamPageFrames) * 2;
                sl = p[0];
                sr = p[1];
            } else {
                sl = sr = 0.f;  // underrun: silence, keep the timeline
                if (v.pos % kStreamPageFrames == 0 || i == 0) ++underruns_;
            }
        } else if (clip.channels == 2) {
            sl = clip.frames[v.pos * 2];
            sr = clip.frames[v.pos * 2 + 1];
        } else {
            sl = sr = clip.frames[v.pos];
        }
        dst[i * 2] += sl * v.cur_l;
        dst[i * 2 + 1] += sr * v.cur_r;

        if (v.ramp_left) {
            v.cur_l += v.inc_l;
            v.cur_r += v.inc_r;
            if (--v.ramp_left == 0) {
                set_targets(v, 0);  // land exactly on the target
                if (v.ending) {
                    end_voice(v);
                    return;
                }
            }
        }

        ++v.pos;
        if (ring) {
            if (v.pos % kStreamPageFrames == 0) ring->consumed.store(v.pos / kStreamPageFrames, std::memory_order_release);
            u64 f;
            if (!StreamRing::map_frame(ring->source, v.loop, v.pos, f)) {
                end_voice(v);
                return;
            }
        } else if (v.pos >= clip.frame_count) {
            if (!v.loop || clip.loop_start >= clip.frame_count) {
                end_voice(v);
                return;
            }
            v.pos = clip.loop_start;
        }
    }
}

void Mixer::promote_virtual() {
    for (VoiceRT& slot : real_) {
        if (slot.active) continue;
        VoiceRT* best = nullptr;
        for (VoiceRT& v : virtual_)
            if (v.active && v.gain > 0.f && (!best || v.priority < best->priority ||
                                             (v.priority == best->priority && v.order < best->order)))
                best = &v;
        if (!best) return;
        slot = *best;
        best->active = false;
        slot.cur_l = slot.cur_r = 0.f;  // fade in from where the timeline is: no click
        set_targets(slot, kRampFrames);
    }
}

void Mixer::render(f32* out, u32 frames) {
    mem::NoHeapScope rt_guard;  // FR-AUD-010: any heap traffic in here asserts
    process_commands();
    f32 peak = 0.f;
    u32 done = 0;
    while (done < frames) {
        const u32 n = frames - done < kBlockFrames ? frames - done : kBlockFrames;
        for (f32* b : bus_buf_) std::memset(b, 0, sizeof(f32) * n * kChannels);

        for (VoiceRT& v : real_)
            if (v.active) mix_voice(v, bus_buf_[v.bus], n);
        for (VoiceRT& v : tails_)
            if (v.active) mix_voice(v, bus_buf_[v.bus], n);
        for (VoiceRT& v : virtual_)
            if (v.active) advance_virtual(v, n);
        promote_virtual();

        // Buses → master (ramped gains), master gain, hard clip (the v0.5 limiter goes here).
        f32* dst = out + done * kChannels;
        for (u32 i = 0; i < n; ++i) {
            f32 l = bus_buf_[Bus_Master][i * 2], r = bus_buf_[Bus_Master][i * 2 + 1];
            for (u32 b = 1; b < kBusCount; ++b) {
                l += bus_buf_[b][i * 2] * bus_[b].cur;
                r += bus_buf_[b][i * 2 + 1] * bus_[b].cur;
            }
            l *= bus_[Bus_Master].cur;
            r *= bus_[Bus_Master].cur;
            for (BusRT& b : bus_)
                if (b.ramp_left) {
                    b.cur += b.inc;
                    if (--b.ramp_left == 0) b.cur = b.target;
                }
            l = math::clamp(l, -1.f, 1.f);
            r = math::clamp(r, -1.f, 1.f);
            dst[i * 2] = l;
            dst[i * 2 + 1] = r;
            const f32 m = std::fabs(l) > std::fabs(r) ? std::fabs(l) : std::fabs(r);
            if (m > peak) peak = m;
        }
        done += n;
        mix_sample_ += n;
    }
    publish_stats(peak);
}

void Mixer::publish_stats(f32 peak) {
    u32 real = 0, virt = 0;
    for (const VoiceRT& v : real_) real += v.active ? 1 : 0;
    for (const VoiceRT& v : virtual_) virt += v.active ? 1 : 0;
    stats_.voices_real.store(real, std::memory_order_relaxed);
    stats_.voices_virtual.store(virt, std::memory_order_relaxed);
    stats_.steals.store(steals_, std::memory_order_relaxed);
    stats_.dropped.store(dropped_, std::memory_order_relaxed);
    stats_.underruns.store(underruns_, std::memory_order_relaxed);
    stats_.master_peak.store(peak, std::memory_order_relaxed);
    stats_.mix_sample.store(mix_sample_, std::memory_order_release);
}

}  // namespace mce::audio::detail
