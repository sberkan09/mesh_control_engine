#include "mce/net/prediction.h"

#include <cmath>

namespace mce::net::detail {

void write_input_header(ByteWriter& out, u32 input_size, u32 newest_seq, u32 count) {
    out.append(Span<const u8>(kInputMagic, 3));
    out.write_u8(kInputVersion);
    out.write_u8(static_cast<u8>(input_size));
    out.write_u32(newest_seq);
    out.write_u8(static_cast<u8>(count));
}

bool read_input_header(ByteReader& in, u32 input_size, u32& newest_seq, u32& count) {
    const u8 m0 = in.read_u8(), m1 = in.read_u8(), m2 = in.read_u8();
    const u8 version = in.read_u8();
    const u8 size = in.read_u8();
    newest_seq = in.read_u32();
    count = in.read_u8();
    return in.ok() && m0 == kInputMagic[0] && m1 == kInputMagic[1] && m2 == kInputMagic[2] &&
           version == kInputVersion && size == input_size && count >= 1 && count <= kMaxInputsPerPacket &&
           in.remaining() == static_cast<usize>(count) * input_size;
}

bool same_transform(const LocalTransform& a, const LocalTransform& b) {
    return std::memcmp(&a.position, &b.position, sizeof a.position) == 0 &&
           std::memcmp(&a.rotation, &b.rotation, sizeof a.rotation) == 0 &&
           std::memcmp(&a.scale, &b.scale, sizeof a.scale) == 0;
}

void Correction::begin(const LocalTransform& drawn, const LocalTransform& predicted, f32 snap_distance,
                       PredictionStats& stats) {
    ++stats.corrections;
    const math::vec3 offset = drawn.position - predicted.position;
    if (!(math::length(offset) <= snap_distance)) {
        *this = Correction{};
        ++stats.snaps;
        return;
    }
    position = offset;
    // drawn = rotation * predicted, so the drawn orientation holds too.
    rotation = math::normalize(drawn.rotation * math::conjugate(predicted.rotation));
    if (rotation.w < 0.0f) rotation = {-rotation.x, -rotation.y, -rotation.z, -rotation.w};
    active = true;
}

void Correction::decay(f32 rate, f32 dt) {
    if (!active) return;
    const f32 k = std::fmin(1.0f, std::fmax(0.0f, rate * dt));
    position = position * (1.0f - k);
    rotation = math::slerp(rotation, math::quat::identity(), k);
    // Small enough to be invisible: gone, so the drawn transform is the prediction bit for bit.
    if (math::length(position) < 1e-4f && rotation.w > 1.0f - 1e-7f) *this = Correction{};
}

LocalTransform Correction::apply(const LocalTransform& predicted) const {
    if (!active) return predicted;
    LocalTransform t = predicted;
    t.position = predicted.position + position;
    t.rotation = math::normalize(rotation * predicted.rotation);
    return t;
}

f32 Correction::size() const { return active ? math::length(position) : 0.0f; }

}  // namespace mce::net::detail
