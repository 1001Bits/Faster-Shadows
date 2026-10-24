// Copyright (c) 2026 Noud. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ShadowOptimizations.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace fps {
// Staggered sun-cascade updates. BSShadowDirectionalLight::Render(mask) (OG 0x28CA710,
// AE 0x2250340) renders a cascade, and computes its world->shadow matrix
// (BSLight::ComputeProjection inside RenderShadowMap), only for set mask bits. A cascade
// left out keeps both its depth slice and the matrix that produced it, so lighting
// samples a consistent map that is a frame older. Cascade 0 always renders. A cascade
// renders anyway after a long gap, a move, a turn, a sun change, a cell change or a
// different shadow-map slot, projection or owner.
struct CadenceSettings {
    std::uint32_t midInterval{2}, farInterval{2};
    std::uint32_t maxWaiting{1};  // cascades that may keep their map in the same frame
    float maxMove{64.0f};        // game units since the cascade last rendered
    float minTurnCosine{0.99939f};  // cos(2 degrees) of the camera rotation since then
    // cos of the largest sun turn (blended direction) a kept cascade may wait through. 1: any change
    // of the sun target, previous direction or blend progress redraws, bit for bit as before.
    float minSunCosine{1.0f};
    // 0: these rules. 1: the staggered rules of Fallout 4 Community Shaders (StaggeredCadence.h),
    // which use only the two intervals; kept for timing the two against each other.
    std::uint32_t policy{0};
};
struct CascadeView {
    Vec3 eye{};
    std::array<float, 9> rotation{};  // camera world rotation, row-major
    std::array<float, 6> frustum{};  // viewer left/right/top/bottom/near/far
    bool orthographic{};
    std::uintptr_t light{};
    std::array<std::uintptr_t, 4> cameras{}, accumulators{};
    // Native +0x200 is the target, not the interpolated camera direction.
    // The camera also depends on +0x210 and +0x220 while the sun blends.
    Vec3 sun{}, sunPrevious{};
    float sunBlendProgress{1.0f};
    std::uintptr_t cell{};
    std::array<std::uint64_t, 4> slots{};  // depth target << 32 | slice, per cascade
    std::uint32_t count{};
};
inline float TurnCosine(const std::array<float, 9>& a, const std::array<float, 9>& b) noexcept {
    // trace(A^T B) = 1 + 2 cos(angle) for two rotations.
    float trace = 0;
    for (std::size_t i = 0; i < 9; ++i) trace += a[i] * b[i];
    return std::isfinite(trace) ? (trace - 1.0f) * 0.5f : -1.0f;
}
// Cascades whose view-depth range is empty this frame: the end UpdateCamerasI gave them is not past
// the previous cascade's end (Ghidra Combined, 2026-10-23). That happens with 3 splits and
// fDirShadowDistance at or below 3000 (the last cascade), with 4 splits (the engine gives a middle
// cascade the end 1.0), or with 2 splits below 800. The sun shader never selects such a cascade;
// godrays still read its map with its camera, so it is kept like a waiting cascade, not left stale.
inline std::uint32_t EmptyCascades(const std::array<float, 4>& splitEnds, std::uint32_t count) noexcept {
    std::uint32_t empty = 0;
    float reached = std::isfinite(splitEnds[0]) ? splitEnds[0] : std::numeric_limits<float>::infinity();
    for (std::uint32_t i = 1; i < count && i < splitEnds.size(); ++i) {
        if (!std::isfinite(splitEnds[i])) return 0;  // unreadable ends: keep every cascade
        if (!(splitEnds[i] > reached)) empty |= 1u << i;
        reached = std::max(reached, splitEnds[i]);
    }
    return std::isfinite(reached) ? empty : 0;
}
// The kept cascades to redraw because another shadow map drew into their slice of the sun's depth
// array since the last plan. `marked` has one bit per slice of `target`; slots are target << 32 | slice.
inline std::uint32_t RedrawReusedSlices(std::uint32_t kept, const std::array<std::uint64_t, 4>& slots, std::uint32_t count,
                                        std::uint32_t marked, std::uint32_t target) noexcept {
    std::uint32_t redraw = 0;
    for (std::uint32_t i = 0; i < count && i < slots.size(); ++i) {
        const auto slice = slots[i] & 0xFFFFFFFF;
        if ((kept >> i & 1) && (slots[i] >> 32) == target && slice < 32 && (marked >> slice & 1)) redraw |= 1u << i;
    }
    return redraw;
}
// Whether a cascade kept with sun state `kept` may stand for this frame's sun.
inline bool SunWithin(const CadenceSettings& s, Vec3 keptSun, Vec3 keptPrevious, float keptProgress,
                      const CascadeView& view) noexcept {
    if (s.minSunCosine >= 1.0f)
        return Exact(keptSun, view.sun) && Exact(keptPrevious, view.sunPrevious) &&
               std::bit_cast<std::uint32_t>(keptProgress) == std::bit_cast<std::uint32_t>(view.sunBlendProgress);
    const Vec3 a = BlendedSunDirection(keptSun, keptPrevious, keptProgress);
    const Vec3 b = BlendedSunDirection(view.sun, view.sunPrevious, view.sunBlendProgress);
    const double dot = static_cast<double>(a.x)*b.x + static_cast<double>(a.y)*b.y + static_cast<double>(a.z)*b.z;
    const double lengths = std::sqrt((static_cast<double>(a.x)*a.x + static_cast<double>(a.y)*a.y + static_cast<double>(a.z)*a.z) *
                                     (static_cast<double>(b.x)*b.x + static_cast<double>(b.y)*b.y + static_cast<double>(b.z)*b.z));
    return lengths > 0 && dot >= s.minSunCosine * lengths;
}
class CascadeCadence {
    struct History {
        std::uint64_t frame{};
        Vec3 eye{}, sun{}, sunPrevious{};
        float sunBlendProgress{1.0f};
        std::array<float, 9> rotation{};
        std::array<float, 6> frustum{};
        bool orthographic{};
        std::uintptr_t light{}, camera{}, accumulator{};
        std::uintptr_t cell{};
        std::uint64_t slot{};
        bool valid{};
    };
    std::array<History, 4> history{};
    std::uint32_t count{};
    std::uint64_t frame{};
    std::uint32_t Interval(const CadenceSettings& s, std::uint32_t cascade) const noexcept {
        if (cascade == 0) return 1;
        if (cascade + 1 == count) return s.farInterval;
        if (cascade + 2 == count) return s.midInterval;
        return 1;
    }
public:
    void Reset() noexcept { history = {}; count = 0; }
    // Called once per sun update, before the cascades accumulate. Returns the cascades
    // to leave out this frame: at most maxWaiting, so the work spreads evenly over frames.
    std::uint32_t Plan(const CadenceSettings& s, const CascadeView& view) noexcept {
        ++frame;
        if (view.count != count || view.count > history.size()) {
            Reset();
            count = view.count <= history.size() ? view.count : 0;
            return 0;
        }
        if (!Finite(view.sun) || !Finite(view.sunPrevious) || !std::isfinite(view.sunBlendProgress) ||
            view.sunBlendProgress < 0.0f || view.sunBlendProgress > 1.0f) return 0;
        for (const float plane : view.frustum) if (!std::isfinite(plane)) return 0;
        const double moveSquared = static_cast<double>(s.maxMove) * s.maxMove;
        // With more than one cascade allowed to wait, a cascade whose interval ends together
        // with another's may wait one frame more, so the two render on different frames.
        const bool spread = s.maxWaiting > 1;
        std::uint32_t waiting = 0, due = 0;
        for (std::uint32_t i = 1; i < count; ++i) {
            const auto interval = Interval(s, i);
            const auto& h = history[i];
            if (interval <= 1 || !h.valid) continue;
            const auto age = frame - h.frame;
            if (age > interval || (age == interval && !spread)) continue;
            if (h.slot != view.slots[i] || h.cell != view.cell ||
                !SunWithin(s, h.sun, h.sunPrevious, h.sunBlendProgress, view)) continue;
            if (h.frustum != view.frustum || h.orthographic != view.orthographic || h.light != view.light ||
                h.camera != view.cameras[i] || h.accumulator != view.accumulators[i]) continue;
            if (!Finite(view.eye) || DistanceSquared(h.eye, view.eye) > moveSquared) continue;
            if (TurnCosine(h.rotation, view.rotation) < s.minTurnCosine) continue;
            (age == interval ? due : waiting) |= 1u << i;
        }
        // Of the cascades due together, the nearest renders and the others wait their extra frame.
        waiting |= due & (due - 1);
        // Keep the cascades rendered most recently; on a tie, the farther one.
        while (static_cast<std::uint32_t>(std::popcount(waiting)) > s.maxWaiting) {
            std::uint32_t oldest = 0;
            for (std::uint32_t i = 1; i < count; ++i)
                if ((waiting >> i & 1) && (!oldest || history[i].frame < history[oldest].frame)) oldest = i;
            waiting &= ~(1u << oldest);
        }
        return waiting;
    }
    // Called after Render with the cascades that actually rendered.
    void Rendered(std::uint32_t mask, const CascadeView& view) noexcept {
        for (std::uint32_t i = 0; i < count && i < history.size(); ++i) {
            if (!(mask >> i & 1)) continue;
            history[i] = {frame, view.eye, view.sun, view.sunPrevious, view.sunBlendProgress,
                          view.rotation, view.frustum, view.orthographic, view.light, view.cameras[i],
                          view.accumulators[i], view.cell, view.slots[i], true};
        }
    }
};
// A map and its saved camera are one cache entry. Loads and renderer resets
// invalidate both, even if the native engine reuses all addresses and slot IDs.
template<class CameraState, std::size_t N>
void ResetCascadeHistory(CascadeCadence& cadence, std::array<CameraState, N>& cameras) noexcept {
    cadence.Reset();
    for (auto& camera : cameras) camera.valid = false;
}
} // namespace fps
