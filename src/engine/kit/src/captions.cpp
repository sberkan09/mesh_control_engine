#include "mce/kit/captions.h"

#include "mce/audio/components.h"
#include "mce/ecs/world.h"
#include "mce/kit/effects.h"
#include "mce/kit/timeline.h"

namespace mce::kit {
namespace {

void add_line(Captions& c, const CaptionLine& line) {
    if (c.lines.size() >= kMaxCaptions) c.lines.erase(0);
    c.lines.push_back(line);
}

CaptionLine* line_from(Captions& c, Entity source) {
    for (CaptionLine& l : c.lines)
        if (l.source == source) return &l;
    return nullptr;
}

void sound_line(Captions& c, Entity e, const SoundCaption& s, f32 seconds) {
    if (s.text.empty()) return;
    if (CaptionLine* l = line_from(c, e)) {  // one line per source: a repeat keeps it up
        l->left = seconds;
        return;
    }
    CaptionLine l;
    l.text.assign(s.text.view());
    l.kind = CaptionKind::Sound;
    l.left = seconds;
    l.source = e;
    add_line(c, l);
}

void kit_captions(Frame& f) {
    World& w = f.world();
    Captions* c = w.try_resource<Captions>();
    if (!c) return;
    const f32 dt = f.dt;
    for (usize i = c->lines.size(); i-- > 0;) {
        c->lines[i].left -= dt;
        if (c->lines[i].left <= 0.0f) c->lines.erase(i);
    }

    for (const ShowCaption& ev : f.events().read<ShowCaption>()) {
        CaptionLine l;
        l.speaker = ev.speaker;
        l.text = ev.text;
        l.kind = ev.kind;
        l.left = ev.seconds > 0.0f ? ev.seconds : kDefaultCaptionSeconds;
        add_line(*c, l);
    }

    f.query<const SoundCaption>().each([&](Entity e, const SoundCaption& s) {
        Captions::Seen* seen = nullptr;
        for (Captions::Seen& x : c->seen)
            if (x.entity == e) seen = &x;
        if (!seen) {  // SfxTrigger::fired is transient: every play since the scene loaded is news
            Captions::Seen x;
            x.entity = e;
            c->seen.push_back(x);
            seen = &c->seen.back();
        }
        if (const SfxTrigger* t = w.try_get<SfxTrigger>(e)) {
            if (t->fired > seen->fired) sound_line(*c, e, s, s.seconds);
            seen->fired = t->fired;
        }
        if (const audio::AudioSource* a = w.try_get<audio::AudioSource>(e)) {
            // A loop keeps its caption up while it plays; a one-shot shows it for `seconds`.
            if (a->started && (a->loop || !seen->playing)) sound_line(*c, e, s, a->loop ? dt * 2.0f : s.seconds);
            seen->playing = a->started;
        }
    });
    for (usize i = c->seen.size(); i-- > 0;)  // entities gone or untagged
        if (!w.try_get<SoundCaption>(c->seen[i].entity)) c->seen.erase_swap(i);
}

}  // namespace

void install_captions(World& world) {
    if (world.try_resource<Captions>()) return;
    world.register_components<SoundCaption>();
    world.register_event<ShowCaption>(Phase::FixedUpdate_Pre);
    world.set_resource(Captions{});
    world.add_system(Phase::FixedUpdate_Post, "kit_captions", &kit_captions);
}

void caption_lines(World& world, Array<CaptionLine>& out) {
    out.clear();
    if (world.find_component(kTypeHash<TimelinePlayer>) != ecs::kInvalidComponent) {
        world.query<const TimelinePlayer>().each([&](Entity e, const TimelinePlayer& p) {
            if (p.subtitle < 0) return;
            StringView speaker, text;
            if (!timeline_subtitle(world, e, speaker, text)) return;
            CaptionLine l;
            l.speaker.assign(speaker);
            l.text.assign(text);
            l.kind = CaptionKind::Speech;
            l.source = e;
            out.push_back(l);
        });
    }
    if (const Captions* c = world.try_resource<Captions>())
        for (const CaptionLine& l : c->lines) out.push_back(l);
}

}  // namespace mce::kit
