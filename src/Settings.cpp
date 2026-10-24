#include "Settings.h"
#include "MenuSettings.h"
#include <charconv>
#include <cstdlib>
#include <cerrno>
#include <cwctype>
namespace fps {
namespace {
constexpr wchar_t ini[] = L".\\Data\\F4SE\\Plugins\\FasterShadows.ini";
float Number(const wchar_t* section, const wchar_t* key, float fallback, float low, float high) {
    wchar_t value[96]{};
    if (!GetPrivateProfileStringW(section, key, L"", value, 96, ini)) return fallback;
    wchar_t* end = nullptr;
    errno = 0;
    const float parsed = std::wcstof(value, &end);
    while (end && std::iswspace(*end)) ++end;
    if (end == value || !end || *end || errno == ERANGE || !std::isfinite(parsed)) {
        logger::warn("Invalid numeric setting; using default {}", fallback);
        return fallback;
    }
    return std::clamp(parsed, low, high);
}
bool Flag(const wchar_t* section, const wchar_t* key, bool fallback) {
    return Number(section, key, fallback ? 1.0f : 0.0f, 0.0f, 1.0f) >= 1.0f;
}
Rule ReadRule(const wchar_t* section, Rule r) {
    r.enabled = Flag(section, L"Enabled", r.enabled);
    r.minDistance = Number(section, L"MinDistance", r.minDistance, 0, 100000);
    r.maxRadius = Number(section, L"MaxRadius", r.maxRadius, 1, 1000);
    r.minAngularSize = Number(section, L"MinAngularSize", r.minAngularSize, 0.00001f, 0.1f);
    return r;
}
}
Settings LoadSettings() {
    Settings s;
    s.enabled = Flag(L"General", L"Enabled", s.enabled);
    s.dryRun = Flag(L"General", L"DryRun", s.dryRun);
    s.diagnostics = Flag(L"General", L"Diagnostics", s.diagnostics);
    s.toggleKey = static_cast<std::uint32_t>(Number(L"General", L"ToggleKey", 0x79, 0, 255));
    s.reportSeconds = static_cast<std::uint32_t>(Number(L"General", L"ReportSeconds", 60, 5, 600));
    s.sun = ReadRule(L"SunShadows", s.sun);
    s.sun.minCascade = static_cast<std::uint32_t>(Number(L"SunShadows", L"MinCascade", 1, 0, 3));
    s.sunMinElevation = Number(L"SunShadows", L"MinSunElevation", s.sunMinElevation, 0, 90);
    s.cascadeCadence = Flag(L"SunShadows", L"CascadeCadence", s.cascadeCadence);
    s.cadenceSkipCull = Flag(L"SunShadows", L"CadenceSkipCull", s.cadenceSkipCull);
    s.sunCullWorker = Flag(L"SunShadows", L"CullOnWorkerThread", s.sunCullWorker);
    s.sunGlareNoFlush = Flag(L"Optimizations", L"SunGlareNoFlush", s.sunGlareNoFlush);
    s.cadence.midInterval = static_cast<std::uint32_t>(Number(L"SunShadows", L"MidCascadeInterval", 2, 1, 4));
    s.cadence.farInterval = static_cast<std::uint32_t>(Number(L"SunShadows", L"FarCascadeInterval", 4, 1, 4));
    s.cadence.maxWaiting = static_cast<std::uint32_t>(Number(L"SunShadows", L"CadenceMaxWaiting", 2, 1, 3));
    s.cadence.maxMove = Number(L"SunShadows", L"CadenceMaxMove", s.cadence.maxMove, 0, 10000);
    s.cadence.minTurnCosine = std::cos(Number(L"SunShadows", L"CadenceMaxTurn", 2, 0, 90) * 0.0174532925f);
    s.cadence.policy = static_cast<std::uint32_t>(Number(L"SunShadows", L"CadencePolicy", 0, 0, 1));
    s.holdFitFrames = static_cast<std::uint32_t>(Number(L"SunShadows", L"HoldFitFrames", 0, 0, 60));
    const float sunTurn = Number(L"SunShadows", L"CadenceMaxSunTurn", 0, 0, 5);
    s.cadence.minSunCosine = sunTurn > 0 ? std::cos(sunTurn * 0.0174532925f) : 1.0f;
    s.vr.enabled = Flag(L"VR", L"CascadeRedraw", s.vr.enabled);
    s.vr.maxMove = Number(L"VR", L"CascadeMaxMove", s.vr.maxMove, 0, 10000);
    s.vr.minTurnCosine = std::cos(Number(L"VR", L"CascadeMaxTurn", 10, 0, 90) * 0.0174532925f);
    s.vr.minSunCosine = std::cos(Number(L"VR", L"CascadeMaxSunTurn", 0.5f, 0, 5) * 0.0174532925f);
    s.point = ReadRule(L"PointShadows", s.point);
    s.pointInteriors = Flag(L"PointShadows", L"Interiors", false);
    s.spotLights = Flag(L"PointShadows", L"SpotLights", s.spotLights);
    s.actorShadows = Flag(L"ActorShadows", L"Enabled", s.actorShadows);
    // As in SPS, values below ~30 m are raised to the minimum.
    s.actorMinDistance = Number(L"ActorShadows", L"MinDistance", s.actorMinDistance, kMinActorShadowDistance, 100000);
    s.actorPointLights = Flag(L"ActorShadows", L"PointLights", s.actorPointLights);
    s.rainOcclusion = Flag(L"RainOcclusion", L"Enabled", s.rainOcclusion);
    s.rainMinRadius = Number(L"RainOcclusion", L"MinRadius", s.rainMinRadius, 0, 1000);
    s.optimizations = 0;
    if (Flag(L"Optimizations", L"ShadowVolume", false)) s.optimizations |= ShadowVolume;
    if (Flag(L"Optimizations", L"SquaredDistance", true)) s.optimizations |= SquaredDistance;
    if (Flag(L"Optimizations", L"LightSnapshot", true)) s.optimizations |= LightSnapshot;
    if (Flag(L"Optimizations", L"BatchCounters", true)) s.optimizations |= BatchCounters;
    if (Flag(L"Optimizations", L"DecisionMemo", false)) s.optimizations |= DecisionMemo;
    if (s.cadence.minSunCosine < 1.0f) s.optimizations |= SunTurnTolerance;
    if (Flag(L"SunShadows", L"SkipEmptyCascades", false)) s.optimizations |= SkipEmptyCascades;
    if (s.holdFitFrames) s.optimizations |= HoldFit;
    if (Flag(L"SunShadows", L"StaticShadowCache", false)) s.optimizations |= ShadowCache;
    // The Mod Configuration Menu's saved choices win over the lines above. The shadow rules apply at
    // startup only; the other switches also change while the game runs, so their hooks install when
    // either the INI or the menu wants them.
    s.sun.enabled = menu::Saved(L"SunShadows", L"bEnabled").value_or(s.sun.enabled);
    s.actorShadows = menu::Saved(L"ActorShadows", L"bEnabled").value_or(s.actorShadows);
    s.point.enabled = menu::Saved(L"PointShadows", L"bEnabled").value_or(s.point.enabled);
    s.rainOcclusion = menu::Saved(L"RainOcclusion", L"bEnabled").value_or(s.rainOcclusion);
    const auto live = [](const wchar_t* section, const wchar_t* key, bool& installed) {
        const bool on = menu::Saved(section, key).value_or(installed);
        installed = installed || on;
        return on;
    };
    s.live.enabled = menu::Saved(L"General", L"bEnabled").value_or(true);
    s.live.cadence = live(L"SunShadows", L"bCascadeCadence", s.cascadeCadence);
    s.live.skipCull = live(L"SunShadows", L"bCadenceSkipCull", s.cadenceSkipCull);
    s.live.worker = live(L"SunShadows", L"bCullOnWorkerThread", s.sunCullWorker);
    s.live.vrRedraw = live(L"VR", L"bCascadeRedraw", s.vr.enabled);
    return s;
}
}
