#pragma once
#include "ShadowPolicy.h"
#include "ShadowOptimizations.h"
#include "CascadeCadence.h"
#include "VrCadence.h"
namespace fps {
// What the Mod Configuration Menu switches while the game runs (MenuSettings.h), as at startup.
struct MenuSwitches {
    bool enabled{true}, cadence{true}, skipCull{true}, worker{true}, vrRedraw{true};
};
struct Settings {
    bool enabled{true};
    bool dryRun{false};
    bool diagnostics{false};
    bool pointInteriors{false};
    // SPS 1.0.52 thresholds. The sun rule filters at the shadow-map registration
    // table, which all Fallout cascade casters (previs, precombines) pass through.
    // The caster rules are off by default: on top of the cascade cadence they gained nothing measurable.
    Rule sun{false, 1500.0f, 150.0f, 0.035f, 1};
    Rule point{false, 1000.0f, 150.0f, 0.035f, 0};
    float sunMinElevation{10.0f};
    // Staggered middle/far cascade updates (CascadeCadence.h). On unless the INI turns it off.
    bool cascadeCadence{true};
    CadenceSettings cadence{};
    // A cascade that keeps its previous map also skips its caster cull (previs) or registration.
    bool cadenceSkipCull{true};
    // Frames a rendered cascade keeps its fitted camera while it still covers the view (0: refit every frame).
    std::uint32_t holdFitFrames{0};
    // Gather the sun's cascade casters on a worker thread while the main thread draws the G-buffer.
    bool sunCullWorker{true};
    // Read the sun-glare occlusion query without flushing the command buffer mid-frame.
    bool sunGlareNoFlush{false};
    // Fallout 4 VR: stale cascades join the game's own staggered refits (VrCadence.h).
    VrCadenceSettings vr{};
    bool spotLights{true};
    bool actorShadows{false};
    float actorMinDistance{3500.0f};
    bool actorPointLights{true};
    bool rainOcclusion{false};
    float rainMinRadius{128.0f};
    std::uint32_t toggleKey{0x79}; // Windows VK_F10, not a DirectInput scan code.
    MenuSwitches live{};
    std::uint32_t reportSeconds{60};
    // Volume analysis and memoization start off pending live tests.
    std::uint32_t optimizations{SquaredDistance | LightSnapshot | BatchCounters};
};
Settings LoadSettings();
}
