// Copyright (c) 2026 Noud. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "CascadeCadence.h"
#include <numeric>

namespace fps {
// Staggered cascade rules as Bryant-21's Fallout 4 Community Shaders describes them
// (iStaggeredCascadeMode), rebuilt here so the two policies can be timed against each other
// in one harness ([SunShadows] CadencePolicy=1). The mechanism stays CascadeCadence's: a
// cascade left out keeps its map, matrix and camera. Only the choice of which cascades wait
// differs:
// - each cascade draws on a fixed frame of its interval; nearest first, it takes the phase
//   whose frames already draw the fewest cascades, so equal intervals take turns;
// - a kept cascade also draws once it is due, or when its kept box no longer holds this
//   frame's receivers (its view-depth slice widened by the cascade blend, within two texels);
// - every cascade draws after a camera jump (over 256 units in one frame), a sun turn over
//   0.25 degrees since a kept draw, a cell change, another light or a new cascade set.
struct StaggeredView {
    Vec3 eye{};
    std::array<Vec3, 3> axes{};  // view camera forward, up and right (NiCamera world rows 0-2)
    float halfWidth{}, halfHeight{}, nearPlane{};  // frustum tangents widened to be symmetric
    std::array<float, 4> splitEnds{};  // view depth at which each cascade ends
    float blend{};  // fBlendSplitDirShadow: the depth over which neighbouring cascades blend
    std::array<Vec3, 4> fitForward{};  // each cascade camera's forward as fitted this frame
};
// A rendered cascade's box as world -> [-1, 1] x [-1, 1] x [0, 1] rows, from its orthographic
// camera, with the clip-space slack of two of its texels.
struct KeptBox {
    std::array<std::array<float, 4>, 3> rows{};
    Vec3 forward{};
    float toleranceX{}, toleranceY{};
    bool valid{};
};
inline float Dot(Vec3 a, Vec3 b) noexcept { return a.x*b.x + a.y*b.y + a.z*b.z; }
// frustum: left, right, top, bottom, near, far; width and height of the shadow-map rectangle in texels.
inline KeptBox MakeKeptBox(Vec3 position, const std::array<Vec3, 3>& axes, const std::array<float, 6>& frustum,
                           std::uint32_t width, std::uint32_t height) noexcept {
    const auto [left, right, top, bottom, nearPlane, farPlane] = frustum;
    if (!(right > left) || !(top > bottom) || !(farPlane > nearPlane) || !width || !height) return {};
    const auto& [forward, up, side] = axes;
    const float sx = 2.0f / (right - left), sy = 2.0f / (top - bottom), sz = 1.0f / (farPlane - nearPlane);
    KeptBox box;
    box.rows[0] = {side.x*sx, side.y*sx, side.z*sx, -sx*Dot(position, side) - (left + right) / (right - left)};
    box.rows[1] = {up.x*sy, up.y*sy, up.z*sy, -sy*Dot(position, up) - (top + bottom) / (top - bottom)};
    box.rows[2] = {forward.x*sz, forward.y*sz, forward.z*sz, -sz*Dot(position, forward) - nearPlane*sz};
    box.forward = forward;
    box.toleranceX = 4.0f / static_cast<float>(width);  // two texels of the 2-unit clip width
    box.toleranceY = 4.0f / static_cast<float>(height);
    box.valid = true;
    for (const auto& row : box.rows) for (const float value : row) box.valid &= std::isfinite(value);
    return box;
}
// Every corner of cascade `cascade`'s receiver slice lies inside the kept box: within the
// tolerance of its sides and short of its far plane (the near plane is up-sun of all receivers).
inline bool KeptBoxCovers(const KeptBox& box, const StaggeredView& view, std::uint32_t cascade) noexcept {
    if (!box.valid || cascade >= view.splitEnds.size()) return false;
    const float nearDepth = cascade == 0 ? view.nearPlane :
        std::max(view.nearPlane, view.splitEnds[cascade - 1] - view.blend);
    const float farDepth = view.splitEnds[cascade] + view.blend;
    if (!std::isfinite(nearDepth) || !std::isfinite(farDepth) || !(farDepth > nearDepth)) return false;
    const auto& [forward, up, side] = view.axes;
    for (const float depth : {nearDepth, farDepth})
        for (const float across : {-view.halfWidth, view.halfWidth})
            for (const float height : {-view.halfHeight, view.halfHeight}) {
                const Vec3 corner{view.eye.x + depth*(forward.x + across*side.x + height*up.x),
                                  view.eye.y + depth*(forward.y + across*side.y + height*up.y),
                                  view.eye.z + depth*(forward.z + across*side.z + height*up.z)};
                const auto clip = [&](std::size_t row) {
                    const auto& m = box.rows[row];
                    return m[0]*corner.x + m[1]*corner.y + m[2]*corner.z + m[3];
                };
                if (!(std::fabs(clip(0)) <= 1.0f + box.toleranceX) || !(std::fabs(clip(1)) <= 1.0f + box.toleranceY) ||
                    !(clip(2) <= 1.0f)) return false;
            }
    return true;
}
// HoldFitFrames: a cascade that renders keeps the camera (box) it was last fitted with for up to N
// frames, while that box still covers what the view needs and the sun has not turned past the
// cadence limit, so its texel grid stands still between refits (less shimmer; the base a static
// shadow cache needs). Cascades already kept by the cadence are not touched.
class FitHold {
    struct Held {
        KeptBox box{};
        std::uint32_t age{};
        std::uintptr_t camera{};
        std::uint64_t slot{};
        Vec3 sun{}, sunPrevious{};
        float progress{1.0f};
        bool valid{};
    };
    std::array<Held, 4> held{};
public:
    void Reset() noexcept { held = {}; }
    // The cascades outside `skip` that keep their held camera this frame.
    std::uint32_t Plan(std::uint32_t frames, const CadenceSettings& s, const CascadeView& base, const StaggeredView& view,
                       std::uint32_t skip) noexcept {
        std::uint32_t mask = 0;
        for (std::uint32_t i = 0; i < base.count && i < held.size(); ++i) {
            auto& h = held[i];
            if (skip >> i & 1) continue;
            if (h.valid && h.age < frames && h.camera == base.cameras[i] && h.slot == base.slots[i] &&
                SunWithin(s, h.sun, h.sunPrevious, h.progress, base) && KeptBoxCovers(h.box, view, i)) {
                ++h.age;
                mask |= 1u << i;
            } else {
                h.valid = false;
            }
        }
        return mask;
    }
    // After Render: a cascade that rendered with a fresh fit starts a new hold with that box.
    void Rendered(std::uint32_t rendered, std::uint32_t heldMask, const CascadeView& base,
                  const std::array<KeptBox, 4>& boxes) noexcept {
        for (std::uint32_t i = 0; i < base.count && i < held.size(); ++i)
            if ((rendered >> i & 1) && !(heldMask >> i & 1))
                held[i] = {boxes[i], 0, base.cameras[i], base.slots[i], base.sun, base.sunPrevious, base.sunBlendProgress, boxes[i].valid};
    }
};
class StaggeredCadence {
    struct Kept {
        std::uint64_t frame{};
        KeptBox box{};
        std::uintptr_t camera{}, accumulator{};
        std::uint64_t slot{};
        bool valid{};
    };
    std::array<Kept, 4> kept{};
    std::array<std::uint32_t, 4> interval{}, phase{};
    std::uint32_t count{};
    std::array<std::uint32_t, 3> scheduled{};  // count, middle and far interval of interval/phase
    std::uint64_t frame{};
    std::uintptr_t light{}, cell{};
    Vec3 eye{};
    bool seen{};
    void Schedule(const CadenceSettings& s) noexcept {
        if (scheduled == std::array<std::uint32_t, 3>{count, s.midInterval, s.farInterval}) return;
        scheduled = {count, s.midInterval, s.farInterval};
        std::uint32_t cycle = 1;
        for (std::uint32_t i = 0; i < count; ++i) {
            interval[i] = i == 0 ? 1 : i + 1 == count ? s.farInterval : i + 2 == count ? s.midInterval : 1;
            interval[i] = std::clamp<std::uint32_t>(interval[i], 1, 8);
            cycle = std::lcm(cycle, interval[i]);
        }
        std::array<std::uint32_t, 840> load{};  // lcm(1..8)
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t best = 0, bestLoad = ~0u;
            for (std::uint32_t p = 0; p < interval[i]; ++p) {
                std::uint32_t worst = 0;
                for (std::uint32_t f = p; f < cycle; f += interval[i]) worst = std::max(worst, load[f]);
                if (worst < bestLoad) { bestLoad = worst; best = p; }
            }
            phase[i] = best;
            for (std::uint32_t f = best; f < cycle; f += interval[i]) ++load[f];
        }
    }
public:
    static constexpr float jumpUnits = 256.0f;
    static constexpr double sunStepCosine = 0.99999048;  // cos(0.25 degrees)
    void Reset() noexcept { kept = {}; count = 0; seen = false; }
    // Called once per sun update, after the engine fitted this frame's cascades and before any
    // kept camera is put back. Returns the cascades that keep last frame's map.
    std::uint32_t Plan(const CadenceSettings& s, const CascadeView& base, const StaggeredView& view) noexcept {
        ++frame;
        if (base.count != count || base.count > kept.size()) {
            Reset();
            count = base.count <= kept.size() ? base.count : 0;
        }
        Schedule(s);
        bool force = !seen || base.light != light || base.cell != cell || !Finite(base.eye) ||
                     !(DistanceSquared(base.eye, eye) <= static_cast<double>(jumpUnits)*jumpUnits);
        for (std::uint32_t i = 0; i < count && !force; ++i) {
            if (!kept[i].valid) continue;
            const auto& a = kept[i].box.forward;
            const auto& b = view.fitForward[i];
            const double scale = std::sqrt(static_cast<double>(Dot(a, a))*Dot(b, b));
            force = !(static_cast<double>(Dot(a, b)) >= sunStepCosine*scale) || !(scale > 0);
        }
        seen = true;light = base.light;cell = base.cell;eye = base.eye;
        if (force) return 0;
        std::uint32_t waiting = 0;
        for (std::uint32_t i = 1; i < count; ++i) {
            const auto& k = kept[i];
            if (interval[i] <= 1 || frame % interval[i] == phase[i]) continue;  // draws on its own frame
            if (!k.valid || k.camera != base.cameras[i] || k.accumulator != base.accumulators[i] || k.slot != base.slots[i]) continue;
            if (frame <= k.frame || frame - k.frame >= interval[i]) continue;  // due
            if (!KeptBoxCovers(k.box, view, i)) continue;
            waiting |= 1u << i;
        }
        return waiting;
    }
    // Called after Render with the cascades that rendered and their boxes as rendered.
    void Rendered(std::uint32_t mask, const CascadeView& base, const std::array<KeptBox, 4>& boxes) noexcept {
        for (std::uint32_t i = 0; i < count && i < kept.size(); ++i) {
            if (!(mask >> i & 1)) continue;
            kept[i] = {frame, boxes[i], base.cameras[i], base.accumulators[i], base.slots[i], boxes[i].valid};
        }
    }
};
} // namespace fps
