// Main-thread math for the v0.5 mixer features: RBJ biquad coefficients (§3.7) and 3D
// spatialization (§3.8). Nothing here runs on the audio thread.

#include <cmath>
#include <complex>

#include "mce/audio/audio.h"

namespace mce::audio {

BiquadCoeffs biquad_coeffs(const BiquadDesc& d, f32 sample_rate) {
    using T = BiquadDesc::Type;
    if (d.type == T::Off) return {};
    const f64 fs = sample_rate;
    f64 f = d.freq_hz;
    if (f < 1.0) f = 1.0;
    if (f > fs * 0.499) f = fs * 0.499;
    const f64 q = d.q > 0.01f ? d.q : 0.01;
    const f64 w0 = 2.0 * 3.14159265358979323846 * f / fs;
    const f64 cw = std::cos(w0), sw = std::sin(w0);
    const f64 alpha = sw / (2.0 * q);
    const f64 A = std::pow(10.0, d.gain_db / 40.0);
    const f64 sa = 2.0 * std::sqrt(A) * alpha;

    f64 b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    switch (d.type) {
        case T::LowPass:
            b0 = (1 - cw) / 2, b1 = 1 - cw, b2 = (1 - cw) / 2;
            a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha;
            break;
        case T::HighPass:
            b0 = (1 + cw) / 2, b1 = -(1 + cw), b2 = (1 + cw) / 2;
            a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha;
            break;
        case T::Peaking:
            b0 = 1 + alpha * A, b1 = -2 * cw, b2 = 1 - alpha * A;
            a0 = 1 + alpha / A, a1 = -2 * cw, a2 = 1 - alpha / A;
            break;
        case T::LowShelf:
            b0 = A * ((A + 1) - (A - 1) * cw + sa);
            b1 = 2 * A * ((A - 1) - (A + 1) * cw);
            b2 = A * ((A + 1) - (A - 1) * cw - sa);
            a0 = (A + 1) + (A - 1) * cw + sa;
            a1 = -2 * ((A - 1) + (A + 1) * cw);
            a2 = (A + 1) + (A - 1) * cw - sa;
            break;
        case T::HighShelf:
            b0 = A * ((A + 1) + (A - 1) * cw + sa);
            b1 = -2 * A * ((A - 1) + (A + 1) * cw);
            b2 = A * ((A + 1) + (A - 1) * cw - sa);
            a0 = (A + 1) - (A - 1) * cw + sa;
            a1 = 2 * ((A - 1) - (A + 1) * cw);
            a2 = (A + 1) - (A - 1) * cw - sa;
            break;
        case T::Off: break;
    }
    BiquadCoeffs c;
    c.b0 = static_cast<f32>(b0 / a0);
    c.b1 = static_cast<f32>(b1 / a0);
    c.b2 = static_cast<f32>(b2 / a0);
    c.a1 = static_cast<f32>(a1 / a0);
    c.a2 = static_cast<f32>(a2 / a0);
    return c;
}

f32 biquad_magnitude(const BiquadCoeffs& c, f32 freq_hz, f32 sample_rate) {
    const f64 w = 2.0 * 3.14159265358979323846 * freq_hz / sample_rate;
    const std::complex<f64> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2.0 * w);
    const std::complex<f64> num = static_cast<f64>(c.b0) + static_cast<f64>(c.b1) * z1 + static_cast<f64>(c.b2) * z2;
    const std::complex<f64> den = 1.0 + static_cast<f64>(c.a1) * z1 + static_cast<f64>(c.a2) * z2;
    return static_cast<f32>(std::abs(num / den));
}

f32 attenuation_gain(const AttenuationCurve& c, f32 distance) {
    const f32 lo = c.min_distance > 1e-4f ? c.min_distance : 1e-4f;
    const f32 hi = c.max_distance > lo ? c.max_distance : lo;
    const f32 d = math::clamp(distance, lo, hi);
    switch (c.model) {
        case AttenuationCurve::Model::Inverse: return lo / (lo + c.rolloff * (d - lo));
        case AttenuationCurve::Model::Linear: {
            if (hi <= lo) return 1.f;
            const f32 g = 1.f - c.rolloff * (d - lo) / (hi - lo);
            return g > 0.f ? g : 0.f;
        }
        case AttenuationCurve::Model::Exponential: return std::pow(d / lo, -c.rolloff);
    }
    return 1.f;
}

Spatial spatialize(const Listener& l, const AttenuationCurve& c, math::vec3 source) {
    const math::vec3 to = source - l.position;
    const f32 dist = math::length(to);
    Spatial s;
    s.gain = attenuation_gain(c, dist) * db_to_linear(l.gain_db);
    const math::vec3 right = math::normalize(math::cross(l.forward, l.up));
    if (dist > 1e-6f) {
        const f32 side = math::dot(to, right) / dist;  // sin(azimuth)
        const f32 near = c.min_distance > 1e-4f ? math::clamp(dist / c.min_distance, 0.f, 1.f) : 1.f;
        s.pan = math::clamp(side * near, -1.f, 1.f);
    }
    return s;
}

Spatial spatialize(Span<const Listener> listeners, ListenerMix mix, const AttenuationCurve& c, math::vec3 source) {
    if (listeners.empty()) return {};
    if (mix == ListenerMix::First || listeners.size() == 1) return spatialize(listeners[0], c, source);
    if (mix == ListenerMix::Nearest) {
        usize best = 0;
        f32 best_d = math::dot(source - listeners[0].position, source - listeners[0].position);
        for (usize i = 1; i < listeners.size(); ++i) {
            const f32 d = math::dot(source - listeners[i].position, source - listeners[i].position);
            if (d < best_d) {
                best_d = d;
                best = i;
            }
        }
        return spatialize(listeners[best], c, source);
    }
    Spatial out;
    out.gain = 0.f;
    f32 weight = 0.f, pan = 0.f;
    for (const Listener& l : listeners) {
        const Spatial s = spatialize(l, c, source);
        out.gain = s.gain > out.gain ? s.gain : out.gain;
        weight += s.gain;
        pan += s.gain * s.pan;
    }
    out.pan = weight > 1e-6f ? math::clamp(pan / weight, -1.f, 1.f) : 0.f;
    return out;
}

}  // namespace mce::audio
