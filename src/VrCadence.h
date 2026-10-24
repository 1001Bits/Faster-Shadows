// Copyright (c) 2026 Noud. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "CascadeCadence.h"
#include <array>
#include <cmath>
#include <cstdint>

namespace fps {
// Fallout 4 VR 1.2.72 staggers its sun cascades natively (Ghidra Combined, 2026-10-24): each frame
// the DrawWorld render computes a cascade mask (cascade 0 every frame, 1 every second, 2 and 3 every
// fourth), and a cascade is refit only on "update" frames (every iShadowUpdateFrameDelay frames) when
// its bit is set, then rebuilt and published on the swap frame at the end of that period. A far
// cascade can therefore keep its fit for up to twelve frames however far the head moved. Fallout 4
// VR Shadow Boost may rewrite the mask writer (a two-cascade lock, a safe mode, a full rotation).
//
// This plan only ever adds bits to the mask the game (and Shadow Boost) produced: a cascade whose
// fit is stale (the camera moved or turned, the sun turned, or the cell changed since its last fit)
// joins the next update window and stays in the mask for that whole window, so it is refit, rebuilt
// and published together. It never sets a bit the native writer has not emitted in the last eight
// frames (Shadow Boost's safe mode keeps cascades whose arrays are not ready out) or for a cascade
// the caller found incomplete, and it adds nothing while Shadow Boost's fit wrapper is live.
struct VrCadenceSettings {
    bool enabled{true};
    float maxMove{128.0f};          // game units the camera may move from a cascade's fit
    float minTurnCosine{0.98481f};  // cos(10 degrees): head motion makes desktop's 2 degrees constant
    float minSunCosine{0.99996f};   // cos(0.5 degrees), the engine's own sun step
    float teleport{4096.0f};        // a jump this large (or a new cell) forces an update this frame
};
struct VrFrame {
    std::uint32_t native{};    // the mask the writer produced this frame
    std::uint32_t allowed{};   // cascades that exist with cameras, both accumulators and a non-empty range
    std::int32_t frame{}, nextUpdate{}, delay{1};  // g_frameCounter, the next update frame, the period
    Vec3 eye{}, sun{};
    std::array<float, 9> rotation{};
    std::uintptr_t cell{};
    bool wrapperLive{};        // Shadow Boost's fit wrapper is installed and on its path
};
struct VrPlan {
    std::uint32_t mask{};      // native | added
    std::uint32_t added{};
    bool forceUpdate{};        // the caller makes this frame an update frame ([nextUpdate] = 0)
};
class VrCadence {
public:
    VrPlan Plan(const VrCadenceSettings& settings, const VrFrame& in) noexcept {
        history_[cursor_++ % history_.size()] = in.native;
        std::uint32_t recent = 0;
        for (const auto mask : history_) recent |= mask;
        VrPlan plan{in.native, 0, false};
        if (!settings.enabled || in.wrapperLive) return plan;
        const std::uint32_t eligible = in.allowed & recent & 0xF;
        bool update = in.frame >= in.nextUpdate;
        std::uint32_t stale = 0, urgent = 0;
        for (std::uint32_t i = 1; i < 4; ++i) {  // cascade 0 is refit on every update frame
            if (!(eligible >> i & 1)) continue;
            const auto& fit = fits_[i];
            if (!fit.valid) { stale |= 1u << i; continue; }
            const float moved = Distance(fit.eye, in.eye);
            if (fit.cell != in.cell || !(moved < settings.teleport)) urgent |= 1u << i;
            if (fit.cell != in.cell || !(moved <= settings.maxMove) ||
                TurnCosine(fit.rotation, in.rotation) < settings.minTurnCosine ||
                SunCosine(fit.sun, in.sun) < settings.minSunCosine) stale |= 1u << i;
        }
        if (urgent && !update && in.frame - lastForced_ >= in.delay) {
            plan.forceUpdate = true;
            lastForced_ = in.frame;
            update = true;
        }
        if (update) {
            // A new window: the stale cascades join it for the whole period, so their refit
            // (this frame), static-cache rebuild and swap (its last frame) happen together.
            window_ = stale;
            windowEnd_ = in.frame + std::max(in.delay, 1) - 1;
            const auto fitted = (in.native | window_) & eligible;
            for (std::uint32_t i = 1; i < 4; ++i)
                if (fitted >> i & 1) fits_[i] = {in.eye, in.sun, in.rotation, in.cell, true};
        } else if (in.frame > windowEnd_) {
            window_ = 0;
        }
        plan.added = window_ & eligible & ~in.native;
        plan.mask = in.native | plan.added;
        return plan;
    }
    void Reset() noexcept { *this = VrCadence{}; }

private:
    struct Fit { Vec3 eye{}, sun{}; std::array<float, 9> rotation{}; std::uintptr_t cell{}; bool valid{}; };
    static float SunCosine(Vec3 a, Vec3 b) noexcept {
        const float la = std::sqrt(a.x*a.x + a.y*a.y + a.z*a.z), lb = std::sqrt(b.x*b.x + b.y*b.y + b.z*b.z);
        const float c = (a.x*b.x + a.y*b.y + a.z*b.z) / (la*lb);
        return std::isfinite(c) ? c : -1.0f;
    }
    std::array<Fit, 4> fits_{};
    std::array<std::uint32_t, 8> history_{};
    std::uint32_t cursor_{};
    std::uint32_t window_{};
    std::int32_t windowEnd_{-1};
    std::int32_t lastForced_{-1000000};
};
}
