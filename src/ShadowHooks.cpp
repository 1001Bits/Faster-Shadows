// Fallout-specific integration of dnai92/SPS shadow policies.
// Copyright (c) 2026 Noud. SPDX-License-Identifier: GPL-3.0-or-later
#include "ShadowHooks.h"
#include "EngineProfiles.h"
#include "HookGate.h"
#include "HookTransaction.h"
#include "MenuSettings.h"
#include "ShadowCache.h"
#include "StaggeredCadence.h"
#include <d3d11.h>
#include "SunWorkerSite.h"
#include "VrCadence.h"
#if defined(FPS_ENABLE_PRIVATE_BENCHMARK)
#include "BenchmarkCadence.h"
#endif

namespace fps {
#if defined(FPS_ENABLE_PRIVATE_BENCHMARK)
namespace lightcost { void Install(std::uintptr_t base); void Report(bool reset); std::uint64_t Now() noexcept; }
namespace framecost { void Install(std::uintptr_t base); void Report(bool reset); void SunRendered(std::uint64_t ticks) noexcept;
                      bool Ready() noexcept;
                      void RequestCullingBatch(bool on) noexcept; }
#endif
namespace {
static_assert(offsetof(RE::NiAVObject, parent) == 0x28);
static_assert(offsetof(RE::NiAVObject, world) == 0x70);
static_assert(offsetof(RE::NiTransform, translate) == 0x30);
static_assert(offsetof(RE::NiAVObject, worldBound) == 0xB0);
static_assert(offsetof(RE::NiAVObject, userData) == 0x110);
static_assert(offsetof(RE::NiBound, fRadius) == 0x0C);
static_assert(offsetof(RE::BSGeometry, skinInstance) == 0x140);
static_assert(offsetof(RE::NiCamera, worldToCam) == 0x120);
static_assert(offsetof(RE::NiCamera, viewFrustum) == 0x160);
static_assert(offsetof(RE::NiFrustum, ortho) == 0x18);
static_assert(offsetof(RE::TESObjectREFR, parentCell) == 0xB8);
static_assert(offsetof(RE::TESObjectCELL, cellFlags) == 0x40);
// Verified in both executables (Ghidra Combined; tools/verify_port.py). No Skyrim layout or slot is reused.
constexpr std::size_t descriptorCount = 0x190, descriptorArray = 0x198;
constexpr std::size_t descriptorStride = 0xF0, descriptorCamera = 0x40, descriptorAccumulator = 0x48;
// Shadow-map slot of a cascade: depth target (QShadowMapArrayDepthStencil) and array slice.
constexpr std::size_t descriptorTarget = 0x50, descriptorSlice = 0x54;
// ShadowMapData::kShadowMapRect (left, right, top = bottom + height, bottom), in texels, and the
// view depth each cascade ends at, which UpdateCamerasI writes (light +0x250, one float per
// cascade). Both as Fallout 4 Community Shaders reads them on 1.10.163 and 1.11.240; used only by
// the staggered comparison rules (CadencePolicy=1).
constexpr std::size_t descriptorRect = 0xD0, cascadeSplitEnds = 0x250;
// UpdateCamerasI holds the target at +0x200, previous direction at +0x210 and
// blend progress at +0x220. Its interpolated direction stays in a local register.
constexpr std::size_t targetSunDirection = 0x200, previousSunDirection = 0x210, sunBlendPhase = 0x220;
constexpr std::size_t cullerCamera = 0x18;
constexpr std::size_t lampCenter = 0x1C0, lampReach = 0x1CC;  // BSParabolicCullingProcess::SetCamera
constexpr std::size_t accumulatorCamera = 0x10;  // set before every shadow-map registration
// A spot map's camera is the lamp; BSShadowFrustumLight::UpdateCamerasI sets its far plane to the reach.
constexpr std::size_t cameraFrustumFar = 0x174, cameraFrustumOrtho = 0x178;
constexpr std::size_t propertyFlags = 0x30;
constexpr std::uint64_t propertySkinned = 1ULL << 1;  // the native rain-map handler skips it too
constexpr std::size_t maxCascades = 4;
// BSShaderAccumulator render modes whose pRegisterObjectFunc entries are replaced.
constexpr std::uint32_t occlusionMode = 14, spotMode = 15, sunMode = 16;
constexpr float radians = 0.0174532925199433f;
// What differs on Fallout 4 VR 1.2.72 (Ghidra Combined and the VR executable, 2026-10-24): VR's
// NiAVObject is 0x40 bytes larger, a shadow-map descriptor is 0x110 bytes with two accumulators (a
// static-caster layer and a dynamic one), the sun light's blend state sits 0x10 lower, and a camera
// keeps its frustums in an array with a pointer to the active one.
struct Layout {
    std::size_t descriptorStride, descriptorAccumulator, dynamicAccumulator;
    std::size_t sunTarget, sunPrevious, sunBlend, skinInstance;
    bool frustumPointer;
};
constexpr Layout desktopLayout{descriptorStride, descriptorAccumulator, 0, targetSunDirection, previousSunDirection,
                               sunBlendPhase, 0x140, false};
constexpr Layout vrLayout{0x110, 0x50, 0x58, 0x1F0, 0x200, 0x210, 0x180, true};
const Layout* layout = &desktopLayout;
constexpr std::size_t vrActiveFrustum = 0x200, frustumFar = 0x14, frustumOrtho = 0x18;
using UpdateSun = void (*)(void*, const RE::NiCamera*);
using BeginPoint = void (*)(void*, const RE::NiCamera*, RE::NiAVObject*, void*);
using Append = void (*)(void*, RE::BSGeometry*);
// VR's handlers take a fourth argument, the pass sub-list RegisterObject passes in r9d; the desktop
// handlers ignore r9, so one signature forwards it everywhere.
using RegisterHandler = bool (*)(void*, RE::NiAVObject*, void*, std::uint32_t);
using RenderSun = void (*)(void*, std::uint32_t);
UpdateSun originalSun{};
RenderSun originalRender{};
BeginPoint originalBeginPoint{};
Append originalPointAppend{};
RegisterHandler nativeShadow{}, nativeOcclusion{};
using WorldCamera = RE::NiCamera* (*)();
WorldCamera worldCamera{};
std::uintptr_t playerSlot{}, imageBase{};
const Profile* activeProfile{};
// The runtimes with profiles. Version numbers alone do not separate them safely (Next-Gen 1.10.984
// shares OG's 1.10), so every per-runtime table is chosen by this.
enum class Runtime { OG, NG, AE, VR, Unknown };
Runtime RuntimeOf(const Profile* profile) noexcept {
    if (!profile) return Runtime::Unknown;
    const auto& v = profile->version;
    if (v[0] == 1 && v[1] == 10 && v[2] == 163) return Runtime::OG;
    if (v[0] == 1 && v[1] == 10 && v[2] == 984) return Runtime::NG;
    if (v[0] == 1 && v[1] == 11 && v[2] == 240) return Runtime::AE;
    if (v[0] == 1 && v[1] == 2 && v[2] == 72) return Runtime::VR;
    return Runtime::Unknown;
}
bool OriginalRuntime() noexcept { return RuntimeOf(activeProfile) == Runtime::OG; }  // OG-only private globals
Settings settings;
// Read on every registration: keep them off cache lines that other data writes.
alignas(64) std::atomic<bool> enabled{false};
// The Mod Configuration Menu's runtime switches (MenuSettings.h). Each is read once where a frame is
// planned, so a frame never mixes the old and the new choice.
std::atomic<bool> liveCadence{true}, liveSkipCull{true}, liveWorker{true}, liveVrRedraw{true};
alignas(64) std::atomic<bool> hooksReady{false};
bool installed = false;
// CBRO (Confidence-Based Runtime Occlusion; it installs on 1.10.163 only) trims the sun's shadow casters and
// suspends previs around the cascade cull. Next to it the sun state comes from the cull itself (cbropublish),
// so the light's update slot, which CBRO checks before it trims, stays the engine's.
bool cbro = false;
struct SunFrame {
    Vec3 eye, direction;
    PreparedSunRule sunRule;
    std::array<const void*, maxCascades> accumulators{};
    std::array<const void*, maxCascades> dynamicAccumulators{};  // VR's second layer; null on desktop
    std::array<const RE::NiCamera*, maxCascades> cameras{};
    std::uint32_t count{};
    bool actorsAllowed{};
    const void* light{};
    CascadeView view{};
    std::uint32_t skip{};  // cascades that keep last frame's map; nothing registers into them
    std::uint32_t held{};  // cascades that render with their held camera (HoldFitFrames)
    bool cadence{};
    bool skipCull{};  // the menu's cull-skip switch as this frame was planned
    std::uint64_t generation{};
};
std::atomic<std::shared_ptr<const SunFrame>> sunFrame;
CascadeCadence cadenceState;
StaggeredCadence staggeredState;  // CadencePolicy=1; guarded by cadenceLock like cadenceState
FitHold fitHold;  // HoldFitFrames; guarded by cadenceLock
// Camera state that passes reading a cascade use besides its matrix: the sun light shader
// takes each cascade camera's frustum extents (BSDFLightShader::SetupGeometry, OG
// 0x28C0780), and godrays read the first two cascades. A cascade left out gets back the
// camera it rendered with, so map, matrix and camera constants all match.
static_assert(offsetof(RE::NiCamera, worldToCam) == 0x120 && offsetof(RE::NiCamera, port) == 0x184);
constexpr std::size_t cameraTransforms = 0x30, cameraTransformBytes = 0x80;   // NiAVObject local and world
constexpr std::size_t cameraProjection = 0x120, cameraProjectionBytes = 0x74;  // worldToCam, frustum, port
struct CameraState {
    std::array<std::byte, cameraTransformBytes> transforms{};
    std::array<std::byte, cameraProjectionBytes> projection{};
    bool valid{};
};
std::array<CameraState, maxCascades> cameraStates{};  // guarded by cadenceLock
std::mutex cadenceLock;
std::uint64_t cadenceGeneration{};  // guarded by cadenceLock
namespace shadowcache { void Reset() noexcept; }  // the static shadow cache, defined below
namespace slotguard {  // the lamp slice guard, defined below
inline constexpr std::uint32_t sunDepthTarget = 6, keepSlice = 3;  // SetDirtyRenderTargets: mode 3 binds without a clear
extern bool installed, insideSun;
std::uint32_t Take() noexcept;
}
std::uint64_t VrAdded() noexcept;   // VR cascade redraw counters, defined with vrmask below
std::uint64_t VrForced() noexcept;
void ResetCadence() {
    ResetCascadeHistory(cadenceState, cameraStates);
    staggeredState.Reset();
    fitHold.Reset();
    shadowcache::Reset();
    ++cadenceGeneration;
}
alignas(64) std::atomic<std::uint64_t> frameEpoch{0};
struct PointContext {
    const void* culler{};
    const RE::NiCamera* lightCamera{};
    Vec3 eye{}, light{};
    ReceiverFrustum view{};
    std::uint64_t traversal{}, tested{}, candidates{}, removed{}, volumeCandidates{}, memoHits{}, memoMisses{}, actors{};
    std::uint32_t mask{};
    bool valid{false};
};
thread_local PointContext* pointContext{};
thread_local DecisionCache decisionCache;
PreparedPointRule preparedPoint;
alignas(64) std::atomic<std::uint32_t> optimizationMask{0};
// The caster rules' gate: a dry run without diagnostics evaluates nothing, and the lab's A/B
// windows may suspend every rule while the cadence keeps running.
bool RulesActive() noexcept {
    return ShouldEvaluateCasterPolicies(settings.dryRun, settings.diagnostics) &&
           !(optimizationMask.load(std::memory_order_relaxed) & SuspendCasterRules);
}
enum Counter : std::size_t {
    volumeCandidates, memoHits, memoMisses, pointRemoved,
    sunTested, sunCandidates, pointTested, pointCandidates, otherCalls,
    spotTested, spotCandidates, actorCandidates, rainTested, rainCandidates, removedTotal,
    cadenceMid, cadenceFar, cadenceDropped, cadenceLeaks, slotConflicts, cadenceSunBlends, cadenceCullsSkipped,
    cacheBuilt, cacheReused, cacheRefused, cadenceLampRedraws, cadenceEmpty, counterCount
};
// Never destroyed: game threads may still register casters while the DLL shuts down.
auto& diagnostics = *new ShardedCounters<counterCount>;
std::atomic<std::uint64_t> lastPoll{0};
std::uint64_t lastReport = 0;
bool keyDown = false;
std::mutex pollMutex;

template<class T> T Read(const void* object, std::size_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object)+offset, sizeof(T));
    return value;
}
Vec3 Position(const RE::NiPoint3& point) noexcept { return {point.x, point.y, point.z}; }
Sphere Bound(const RE::NiAVObject& object) noexcept { return {Position(object.worldBound.center), object.worldBound.fRadius}; }
// NiFrustum left, right, top, bottom, near, far (Windows headers define `near` and `far` away).
std::array<float, 6> Planes(const RE::NiFrustum& frustum) noexcept {
    std::array<float, 6> planes;
    std::memcpy(planes.data(), &frustum, sizeof(planes));
    return planes;
}
// A lamp camera's far plane (its reach) and orthographic flag.
std::pair<float, bool> FarAndOrtho(const RE::NiCamera* camera) noexcept {
    if (!layout->frustumPointer) return {Read<float>(camera, cameraFrustumFar), Read<bool>(camera, cameraFrustumOrtho)};
    const auto* frustum = Read<const std::byte*>(camera, vrActiveFrustum);
    if (!frustum) return {std::numeric_limits<float>::quiet_NaN(), true};  // no frustum: not a lamp
    return {Read<float>(frustum, frustumFar), Read<bool>(frustum, frustumOrtho)};
}
std::array<Vec3, 3> Rows(const RE::NiMatrix3& rotate) noexcept {
    const auto& m = rotate.entry;
    return {{{m[0].x, m[0].y, m[0].z}, {m[1].x, m[1].y, m[1].z}, {m[2].x, m[2].y, m[2].z}}};
}
// fBlendSplitDirShadow:Display, the depth over which neighbouring sun cascades blend; NaN if the
// game has no such setting, so no kept map covers and the staggered rules reuse nothing.
float CascadeBlend() noexcept {
    static const RE::Setting* const setting = [] {
        const auto* found = RE::GetINISetting("fBlendSplitDirShadow:Display");
        if (!found || found->GetType() != RE::Setting::SETTING_TYPE::kFloat) {
            logger::warn("fBlendSplitDirShadow:Display not found: the staggered cascade rules keep no map");
            return static_cast<const RE::Setting*>(nullptr);
        }
        return found;
    }();
    return setting ? setting->GetFloat() : std::numeric_limits<float>::quiet_NaN();
}
// The staggered rules' inputs, from the view camera and this frame's fresh cascade fit.
StaggeredView StaggeredInputs(const void* light, const RE::NiCamera& camera, const SunFrame& frame) noexcept {
    StaggeredView view;
    view.eye = frame.eye;
    view.axes = Rows(camera.world.rotate);
    const auto [left, right, top, bottom, nearPlane, farPlane] = Planes(camera.viewFrustum);
    view.halfWidth = std::max(std::fabs(left), std::fabs(right));
    view.halfHeight = std::max(std::fabs(top), std::fabs(bottom));
    view.nearPlane = camera.viewFrustum.ortho ? std::numeric_limits<float>::quiet_NaN() : nearPlane;
    view.splitEnds = Read<std::array<float, 4>>(light, cascadeSplitEnds);
    view.blend = CascadeBlend();
    for (std::uint32_t i = 0; i < frame.count; ++i)
        if (frame.cameras[i]) view.fitForward[i] = Rows(frame.cameras[i]->world.rotate)[0];
    return view;
}
bool Skinned(const void* property) noexcept {
    return property && (Read<std::uint64_t>(property, propertyFlags) & propertySkinned);
}
// SPS ActorShadowCulling: wind-animated plants are skinned as well, so the first
// ancestor that carries its own reference decides. Never recurses into GetUserData.
const RE::NiAVObject* ActorRoot(const RE::NiAVObject* object) noexcept {
    for (int depth = 0; object && depth < 32; ++depth, object = object->parent) {
        if (const auto reference = reinterpret_cast<const RE::TESForm*>(object->userData))
            return reference->GetFormType() == RE::ENUM_FORM_ID::kACHR ? object : nullptr;
    }
    return nullptr;
}
// Per-actor hysteresis for the actor distance rule (ActorShadowCulled), shared by the sun and
// lamp passes, which judge the same distance from the camera. Lock-free: a slot holds the root
// node of an actor whose shadow is off; a collision only makes one actor use the plain limit once.
namespace actorrule {
constexpr std::size_t slots = 512;
std::array<std::atomic<std::uintptr_t>, slots> off{};
std::size_t Slot(std::uintptr_t key) noexcept { return static_cast<std::size_t>((key >> 4) * 0x9E3779B97F4A7C15ull >> 55); }
void Clear() noexcept { for (auto& slot : off) slot.store(0, std::memory_order_relaxed); }
// Body parts sit within a few hundred units of their actor; anything this far inside the
// return distance keeps its shadow without walking up to the actor.
constexpr float clearlyNear = 1000.0f;
bool Culled(const RE::NiAVObject* object, Sphere bound, Vec3 eye) noexcept {
    const float limit = std::max(settings.actorMinDistance, kMinActorShadowDistance);
    if (Finite(bound.center) && Finite(eye) && Distance(bound.center, eye) + bound.radius < kActorShadowReturn * limit - clearlyNear)
        return false;
    const auto* root = ActorRoot(object);
    if (!root) return false;
    const auto key = reinterpret_cast<std::uintptr_t>(root);
    auto& slot = off[Slot(key)];
    const bool culled = ActorShadowCulled(slot.load(std::memory_order_relaxed) == key, settings.actorMinDistance,
                                          Position(root->world.translate), eye);
    if (culled) slot.store(key, std::memory_order_relaxed);
    else {
        auto expected = key;
        slot.compare_exchange_strong(expected, 0, std::memory_order_relaxed);
    }
    return culled;
}
}
const RE::TESObjectCELL* PlayerCell() noexcept {
    const auto* player = Read<RE::PlayerCharacter*>(reinterpret_cast<void*>(playerSlot), 0);
    return player ? player->GetParentCell() : nullptr;
}
bool PointCellAllowed() noexcept {
    const auto* cell = PlayerCell();
    return LampCellAllowed(cell != nullptr, cell && cell->IsInterior(), settings.pointInteriors);
}
void Count(Counter counter) {
    if (settings.diagnostics) diagnostics.Add(counter);
}

// pRegisterObjectFunc entries. They are filled when the renderer starts and
// rewritten by BSShaderManager::Reinitialize, so installation is retried.
bool SunRegister(void* accumulator, RE::NiAVObject* object, void* property, std::uint32_t sub);
bool SpotRegister(void* accumulator, RE::NiAVObject* object, void* property, std::uint32_t sub);
bool OcclusionRegister(void* accumulator, RE::NiAVObject* object, void* property, std::uint32_t sub);
struct RegisterEntry {
    const char* name;
    std::uint32_t mode;
    RegisterHandler replacement;
    bool occlusion;
    bool wanted{};
    std::atomic<int> state{0};  // 0 waiting for the renderer, 1 installed, -1 refused
};
std::array<RegisterEntry, 3> registerEntries{{
    {"spot", spotMode, &SpotRegister, false}, {"sun", sunMode, &SunRegister, false},
    {"rain occlusion", occlusionMode, &OcclusionRegister, true}}};
void* volatile* RegisterSlot(const RegisterEntry& entry) noexcept {
    return reinterpret_cast<void* volatile*>(imageBase+activeProfile->registerTable+entry.mode*8);
}
void TryInstallRegisterHooks() {
    if (!hooksReady.load(std::memory_order_acquire) || !activeProfile) return;
    for (auto& entry : registerEntries) {
        if (!entry.wanted || entry.state.load() < 0) continue;
        void* const native = entry.occlusion ? reinterpret_cast<void*>(nativeOcclusion) : reinterpret_cast<void*>(nativeShadow);
        void* const ours = reinterpret_cast<void*>(entry.replacement);
        auto* slot = RegisterSlot(entry);
        void* const current = *slot;
        if (current == ours) continue;
        const bool reinstalled = entry.state.exchange(0) == 1;
        if (reinstalled) ResetShadows();  // renderer recreated its tables/resources; old maps are invalid
        if (!current) continue;  // renderer not started yet
        if (current != native) {
            entry.state.store(-1);
            logger::warn("Shadow-map {} registration entry differs from the engine handler; that rule stays off", entry.name);
            continue;
        }
        if (InterlockedCompareExchangePointer(slot, ours, native) == native) {
            entry.state.store(1);
            logger::info("Shadow-map {} registration filter {}", entry.name, reinstalled ? "re-installed after renderer reinitialisation" : "installed");
        }
    }
}

// A switch of the Mod Configuration Menu. `unavailable` says why it cannot change in this game (empty
// when its hooks are installed); `saved` is its last choice read from the menu's settings file.
struct MenuSwitch {
    const wchar_t* section;
    const wchar_t* key;
    std::atomic<bool>* value;
    const char* name;
    const char* unavailable;
    std::optional<bool> saved;
};
std::array<MenuSwitch, 5> menuSwitches{};
// A new save in the menu: a switch whose choice changed applies from the next frame. The F10 state
// stays unless the menu's own Faster Shadows switch changed.
void ApplyMenu() {
    for (auto& entry : menuSwitches) {
        const auto saved = menu::Saved(entry.section, entry.key);
        if (!entry.value || !saved || saved == entry.saved) continue;
        entry.saved = saved;
        if (*entry.unavailable) {
            logger::info("Menu: {} {} {}", entry.name, *saved ? "on" : "off", entry.unavailable);
            continue;
        }
        entry.value->store(*saved, std::memory_order_relaxed);
        logger::info("Menu: {} {}", entry.name, *saved ? "ON" : "OFF");
    }
}

void Poll() {
    if (!hooksReady.load(std::memory_order_acquire)) return;
    const auto now = GetTickCount64();
    auto previous = lastPoll.load(std::memory_order_relaxed);
    if (now-previous < 50 || !lastPoll.compare_exchange_strong(previous, now)) return;
    std::scoped_lock lock(pollMutex);
    TryInstallRegisterHooks();
    DWORD foregroundPID = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPID);
    const bool pressed = settings.toggleKey && foregroundPID == GetCurrentProcessId() &&
                         (GetAsyncKeyState(static_cast<int>(settings.toggleKey)) & 0x8000);
    if (pressed && !keyDown) {
        const bool active = !enabled.load(std::memory_order_relaxed);
        enabled.store(active, std::memory_order_relaxed);
        logger::info("Faster Shadows {} (DryRun={})", active ? "ON" : "OFF", settings.dryRun);
    }
    keyDown = pressed;
    if (menu::Changed()) ApplyMenu();
    if (settings.diagnostics && now-lastReport >= settings.reportSeconds*1000ULL) {
        lastReport = now;
        logger::info("Shadows: enabled={} dryRun={} sun tested={} candidates={} point tested={} candidates={} spot tested={} candidates={} actor candidates={} rain tested={} candidates={} removed={} other={}",
            enabled.load(), settings.dryRun, diagnostics.Sample(sunTested, true), diagnostics.Sample(sunCandidates, true),
            diagnostics.Sample(pointTested, true), diagnostics.Sample(pointCandidates, true),
            diagnostics.Sample(spotTested, true), diagnostics.Sample(spotCandidates, true),
            diagnostics.Sample(actorCandidates, true), diagnostics.Sample(rainTested, true),
            diagnostics.Sample(rainCandidates, true), diagnostics.Sample(removedTotal, true), diagnostics.Sample(otherCalls, true));
        if (RuntimeOf(activeProfile) == Runtime::VR)
            logger::info("VR cascades: redraw bits added={} forced refits={}", VrAdded(), VrForced());
    }
}

const SunFrame* CurrentSun() {
    thread_local std::uint64_t cachedEpoch = ~std::uint64_t{0};
    thread_local std::shared_ptr<const SunFrame> cached;
    const auto epoch = frameEpoch.load(std::memory_order_acquire);
    if (cachedEpoch != epoch) {
        cached = sunFrame.load(std::memory_order_acquire);
        cachedEpoch = epoch;
    }
    return cached.get();
}


// Staggered cascades need the sun registration filter: it keeps a skipped cascade's
// accumulator empty, so nothing registered this frame is left behind in it.
bool CadenceReady() noexcept {
    return settings.cascadeCadence && liveCadence.load(std::memory_order_relaxed) &&
           registerEntries[1].state.load(std::memory_order_relaxed) == 1 &&
           *RegisterSlot(registerEntries[1]) == reinterpret_cast<void*>(&SunRegister);
}

// Publishes this frame's sun state before the cascades accumulate, on both the batched and the
// unbatched path: from the desktop sun update, or on VR from the cascade gather (vrsun below).
void PublishSun(void* light, const RE::NiCamera* camera) {
    if (!hooksReady.load(std::memory_order_acquire)) return;
    Poll();
    const bool cadence = CadenceReady();
    const bool evaluate = RulesActive();
    if (!enabled.load(std::memory_order_relaxed) || !((evaluate && (settings.sun.enabled || settings.actorShadows)) || cadence)) {
        // Render and registration read the current frame: never leave an old skip mask behind.
        if (sunFrame.load(std::memory_order_acquire)) ResetShadows();
        return;
    }
    auto frame = std::make_shared<SunFrame>();
    frame->light = light;
    // Serialize planning/publication with load and renderer invalidation. An old
    // render may finish afterward, but its generation cannot refill this cache.
    std::scoped_lock lock(cadenceLock);
    frame->generation = cadenceGeneration;
    if (light && camera) {
        frame->eye = Position(camera->world.translate);
        const auto target = Read<Vec3>(light, layout->sunTarget);
        const auto previous = Read<Vec3>(light, layout->sunPrevious);
        const auto blend = Read<float>(light, layout->sunBlend);
        frame->direction = BlendedSunDirection(target, previous, blend);
        frame->sunRule = PreparedSunRule(settings.sun, frame->direction, settings.sunMinElevation);
        frame->actorsAllowed = frame->sunRule.elevationAllowed;
        const auto count = Read<std::uint32_t>(light, descriptorCount);
        const auto* descriptors = Read<const std::byte*>(light, descriptorArray);
        if (Finite(frame->eye) && frame->direction.z != 0.0f && descriptors && count <= maxCascades) {
            frame->count = count;
            for (std::uint32_t i = 0; i < count; ++i) {
                const auto* descriptor = descriptors + i*layout->descriptorStride;
                frame->cameras[i] = Read<const RE::NiCamera*>(descriptor, descriptorCamera);
                frame->accumulators[i] = Read<const void*>(descriptor, layout->descriptorAccumulator);
                if (layout->dynamicAccumulator)
                    frame->dynamicAccumulators[i] = Read<const void*>(descriptor, layout->dynamicAccumulator);
            }
            if (cadence) {
                for (std::uint32_t i = 0; i < count; ++i) {  // desktop layout only: cadence never runs on VR
                    frame->view.slots[i] = std::uint64_t{Read<std::uint32_t>(descriptors, i*descriptorStride+descriptorTarget)} << 32 |
                                           Read<std::uint32_t>(descriptors, i*descriptorStride+descriptorSlice);
                    frame->view.cameras[i] = reinterpret_cast<std::uintptr_t>(frame->cameras[i]);
                    frame->view.accumulators[i] = reinterpret_cast<std::uintptr_t>(frame->accumulators[i]);
                }
                const auto& rotation = camera->world.rotate.entry;
                frame->view.eye = frame->eye;
                frame->view.sun = target;
                frame->view.sunPrevious = previous;
                frame->view.sunBlendProgress = blend;
                if (frame->view.sunBlendProgress >= 0.0f && frame->view.sunBlendProgress < 1.0f)
                    Count(cadenceSunBlends);
                frame->view.count = count;
                frame->view.cell = reinterpret_cast<std::uintptr_t>(PlayerCell());
                frame->view.light = reinterpret_cast<std::uintptr_t>(light);
                static_assert(offsetof(RE::NiFrustum, ortho) == 6*sizeof(float));
                std::memcpy(frame->view.frustum.data(), &camera->viewFrustum, sizeof(frame->view.frustum));
                frame->view.orthographic = camera->viewFrustum.ortho;
                frame->view.rotation = {rotation[0].x, rotation[0].y, rotation[0].z, rotation[1].x, rotation[1].y,
                                        rotation[1].z, rotation[2].x, rotation[2].y, rotation[2].z};
                frame->cadence = true;
                frame->skipCull = liveSkipCull.load(std::memory_order_relaxed);
                auto rules = settings.cadence;
                // The private benchmark toggles the sun tolerance per window; without its bit the sun compares exactly.
                if (!(optimizationMask.load(std::memory_order_relaxed) & SunTurnTolerance)) rules.minSunCosine = 1.0f;
                frame->skip = rules.policy == 1 ?
                    staggeredState.Plan(rules, frame->view, StaggeredInputs(light, *camera, *frame)) :
                    cadenceState.Plan(rules, frame->view);
                // An empty cascade keeps its last map and camera too (each restored below only if saved).
                if (optimizationMask.load(std::memory_order_relaxed) & SkipEmptyCascades) {
                    const auto empty = EmptyCascades(Read<std::array<float, 4>>(light, cascadeSplitEnds), count);
                    frame->skip |= empty;
                    for (auto bits = empty; bits; bits &= bits - 1) Count(cadenceEmpty);
                }
                // A kept cascade on a slice a lamp map reused since the last plan renders instead. Without
                // the guard only slice 2 is kept: no lamp ever uses it.
                const auto reused = slotguard::installed ? slotguard::Take() : ~0u & ~(1u << 2);
                const auto redraw = RedrawReusedSlices(frame->skip, frame->view.slots, count, reused, slotguard::sunDepthTarget);
                frame->skip &= ~redraw;
                for (auto bits = redraw; bits; bits &= bits - 1) Count(cadenceLampRedraws);
                if ((optimizationMask.load(std::memory_order_relaxed) & HoldFit) && settings.holdFitFrames)
                    frame->held = fitHold.Plan(settings.holdFitFrames, rules, frame->view, StaggeredInputs(light, *camera, *frame), frame->skip);
                for (std::uint32_t i = 0; i < count; ++i) {
                    if (!((frame->skip | frame->held) >> i & 1)) continue;
                    auto* bytes = reinterpret_cast<std::byte*>(const_cast<RE::NiCamera*>(frame->cameras[i]));
                    if (!bytes || !cameraStates[i].valid) {
                        frame->skip &= ~(1u << i);  // without its camera, the cascade renders with a fresh fit
                        frame->held &= ~(1u << i);
                        continue;
                    }
                    std::memcpy(bytes+cameraTransforms, cameraStates[i].transforms.data(), cameraTransformBytes);
                    std::memcpy(bytes+cameraProjection, cameraStates[i].projection.data(), cameraProjectionBytes);
                }
            }
        }
    }
    if (settings.cascadeCadence && !frame->cadence) ResetCadence();
    sunFrame.store(std::move(frame), std::memory_order_release);
    frameEpoch.fetch_add(1, std::memory_order_release);
}
void SunUpdate(void* light, const RE::NiCamera* camera) {
    originalSun(light, camera);
    PublishSun(light, camera);
}

// Called by DeferredLightsImpl with all cascades (7). A cascade planned out this frame
// is left out of the mask; its slice and its world->shadow matrix then stay as last
// rendered, because RenderShadowMap computes that matrix only after drawing.
// Renders the masked cascades. With the static shadow cache on, the middle and far cascades go
// through it (the nearest refits too often when the camera moves to profit from a held copy).
namespace shadowcache {  // defined after the call-site hooks below
extern bool installed;
bool Render(RenderSun render, void* light, std::uint32_t cascade, std::uint64_t slot, void* accumulator, bool held);
}
void RenderCascades(void* light, std::uint32_t mask, const SunFrame* frame) {
    std::uint32_t ordinary = mask;
    constexpr std::uint32_t cacheBits = ShadowCache | HoldFit;
    if (frame && shadowcache::installed && settings.holdFitFrames &&
        (optimizationMask.load(std::memory_order_relaxed) & cacheBits) == cacheBits) {
        for (std::uint32_t i = 1; i < frame->count; ++i)
            if ((mask >> i & 1) && shadowcache::Render(originalRender, light, i, frame->view.slots[i],
                    const_cast<void*>(frame->accumulators[i]), frame->held >> i & 1)) ordinary &= ~(1u << i);
    }
    if (ordinary) originalRender(light, ordinary);
}
void SunRender(void* light, std::uint32_t mask) {
    const auto* frame = hooksReady.load(std::memory_order_acquire) ? CurrentSun() : nullptr;
    const bool planned = frame && frame->cadence && frame->light == light;
    const std::uint32_t skip = planned ? frame->skip : 0;
    // The native call waits for the culling batch before rendering. Inspect a skipped
    // accumulator only after that wait: its worker jobs may still be registering
    // passes on entry, and an early empty read could miss a later registration.
#if defined(FPS_ENABLE_PRIVATE_BENCHMARK)
    const auto renderBegin = lightcost::Now();
    slotguard::insideSun = true;
    RenderCascades(light, mask & ~skip, planned ? frame : nullptr);
    slotguard::insideSun = false;
    framecost::SunRendered(lightcost::Now()-renderBegin);
#else
    slotguard::insideSun = true;
    RenderCascades(light, mask & ~skip, planned ? frame : nullptr);
    slotguard::insideSun = false;
#endif
    if (planned) {
        std::scoped_lock lock(cadenceLock);
        // Reset/reinstallation invalidates native camera and accumulator ownership.
        // Reject an old render before inspecting either kind of native resource.
        if (frame->generation != cadenceGeneration) return;
        if (skip) {
            for (std::uint32_t i = 0; i < frame->count; ++i)
                if (skip >> i & 1) Count(i + 1 == frame->count ? cadenceFar : cadenceMid);
#if defined(FPS_ENABLE_PRIVATE_BENCHMARK)
            // OG BSShaderAccumulator: +0x3B0 is 0xFFFF when no batch pass is queued
            // (FinishAccumulating_ShadowMapOrMask). Anything queued would be drawn later.
            if (settings.diagnostics && OriginalRuntime()) {
                for (std::uint32_t i = 0; i < frame->count; ++i)
                    if ((skip >> i & 1) && frame->accumulators[i] && Read<std::uint32_t>(frame->accumulators[i], 0x3B0) != 0xFFFF)
                        Count(cadenceLeaks);
            }
#endif
        }
        const auto rendered = mask & ~skip;
        cadenceState.Rendered(rendered, frame->view);
        const bool holding = settings.holdFitFrames && (optimizationMask.load(std::memory_order_relaxed) & HoldFit);
        if (settings.cadence.policy == 1 || holding) {
            std::array<KeptBox, 4> boxes{};
            const auto* descriptors = Read<const std::byte*>(light, descriptorArray);
            for (std::uint32_t i = 0; i < frame->count && descriptors; ++i) {
                const auto* camera = frame->cameras[i];
                if (!(rendered >> i & 1) || !camera || !camera->viewFrustum.ortho) continue;
                const auto rect = Read<std::array<std::uint32_t, 4>>(descriptors, i*descriptorStride+descriptorRect);
                boxes[i] = MakeKeptBox(Position(camera->world.translate), Rows(camera->world.rotate),
                    Planes(camera->viewFrustum), rect[1] > rect[0] ? rect[1] - rect[0] : 0, rect[2] > rect[3] ? rect[2] - rect[3] : 0);
            }
            if (settings.cadence.policy == 1) staggeredState.Rendered(rendered, frame->view, boxes);
            if (holding) fitHold.Rendered(rendered, frame->held, frame->view, boxes);
        }
        for (std::uint32_t i = 0; i < frame->count; ++i) {
            const auto* bytes = reinterpret_cast<const std::byte*>(frame->cameras[i]);
            if (!(rendered >> i & 1) || !bytes) continue;
            std::memcpy(cameraStates[i].transforms.data(), bytes+cameraTransforms, cameraTransformBytes);
            std::memcpy(cameraStates[i].projection.data(), bytes+cameraProjection, cameraProjectionBytes);
            cameraStates[i].valid = true;
        }
    }
}

// Cascade cadence keeps a reused cascade's map, so this frame's casters for it are wasted
// work. In BSShadowDirectionalLight::AccumulateFromLists (OG 0x28CA7D0, AE 0x2250400, Ghidra
// Combined) each cascade iteration opens with BSShaderAccumulator::ClearDebugStats(accumulator),
// then BSCullingGroup::Process(group) and AccumulatePasses(group, accumulator). Both builds
// have the same loops and the same +0x168 handling in Process.
// With previs, every Process first clears the group's culled results (+0x168 clear), so one
// cascade's cull never feeds the next: a reused cascade skips both calls. Without previs the
// group keeps state across cascades (+0x168 set), so only the registration pass is skipped.
// Either way the reused accumulator stays empty, exactly as SunRegister already ensures.
namespace cullskip {
using ClearStats = void (*)(void*);
using ProcessGroup = void (*)(void*, std::uint64_t);
using AccumulateGroup = void (*)(void*, void*);
struct Site { std::uint32_t call, target; };
struct Sites { Site previsOpening, previsProcess, previsAccumulate, plainAccumulate; };
constexpr Sites og{{0x28CA9B6, 0x282E790}, {0x28CAA5D, 0x1CCCE70}, {0x28CAA73, 0x1CCD520}, {0x28CAC73, 0x1CCD520}};
constexpr Sites ae{{0x2250696, 0x21D64B0}, {0x225073E, 0x17E0270}, {0x2250754, 0x17E08C0}, {0x22509B1, 0x17E08C0}};
// Next-Gen 1.10.984 has AE's code at its own addresses (Ghidra Combined, 2026-10-23).
constexpr Sites ng{{0x20F3586, 0x2079310}, {0x20F362E, 0x16C5010}, {0x20F3644, 0x16C5660}, {0x20F38A1, 0x16C5660}};
// The verified sites for a runtime, or nullptr where none are mapped (the cull is then never skipped).
const Sites* For(Runtime runtime) noexcept {
    switch (runtime) {
    case Runtime::OG: return &og;
    case Runtime::AE: return &ae;
    case Runtime::NG: return &ng;
    default: return nullptr;
    }
}
constexpr std::size_t hookCount = 4;
ClearStats originalClear{};
ProcessGroup originalProcess{};
AccumulateGroup originalAccumulate{};
thread_local bool reusedCascade = false;
bool IsCallTo(std::uintptr_t base, Site site) noexcept {
    const auto* code = reinterpret_cast<const std::uint8_t*>(base+site.call);
    std::int32_t relative{};
    std::memcpy(&relative, code+1, sizeof(relative));
    return code[0] == 0xE8 && base+site.call+5+relative == base+site.target;
}
bool Reused(const void* accumulator) noexcept {
    const auto* frame = hooksReady.load(std::memory_order_acquire) ? CurrentSun() : nullptr;
    if (!frame || !frame->cadence || !frame->skipCull || !frame->skip) return false;
    for (std::uint32_t i = 0; i < frame->count; ++i)
        if (frame->accumulators[i] == accumulator) return frame->skip >> i & 1;
    return false;
}
void Opening(void* accumulator) {
    originalClear(accumulator);
    reusedCascade = Reused(accumulator);
}
void Process(void* group, std::uint64_t flag) {
    if (!reusedCascade) originalProcess(group, flag);
}
void Accumulate(void* group, void* accumulator) {
    reusedCascade = false;
    if (Reused(accumulator)) {
        Count(cadenceCullsSkipped);
        return;
    }
    originalAccumulate(group, accumulator);
}
void Install(std::uintptr_t base, const Sites& sites) {
    for (const auto site : {sites.previsOpening, sites.previsProcess, sites.previsAccumulate, sites.plainAccumulate}) {
        if (!IsCallTo(base, site)) {
            logger::warn("Cascade cull skipping: call sites differ from the verified engine; reused cascades are still culled");
            return;
        }
    }
    // Render threads may already run these loops: each original must be valid before its call
    // site points at us (SPS 1.0.64 crashed on the reverse order). IsCallTo verified the targets.
    originalClear = reinterpret_cast<ClearStats>(base+sites.previsOpening.target);
    originalProcess = reinterpret_cast<ProcessGroup>(base+sites.previsProcess.target);
    originalAccumulate = reinterpret_cast<AccumulateGroup>(base+sites.previsAccumulate.target);
    auto& trampoline = F4SE::GetTrampoline();
    const Site rewritten[] = {sites.previsOpening, sites.previsProcess, sites.previsAccumulate, sites.plainAccumulate};
    const std::uintptr_t replacements[] = {reinterpret_cast<std::uintptr_t>(&Opening), reinterpret_cast<std::uintptr_t>(&Process),
                                           reinterpret_cast<std::uintptr_t>(&Accumulate), reinterpret_cast<std::uintptr_t>(&Accumulate)};
    for (std::size_t i = 0; i < std::size(rewritten); ++i) {
        if (trampoline.write_call<5>(base+rewritten[i].call, replacements[i]) != base+rewritten[i].target)
            logger::error("Cascade cull skipping: a rewritten call did not target the verified function");
    }
    logger::info("Cascade cull skipping installed: a reused cascade is neither culled (previs) nor registered");
}
}

// The sun-glare occlusion poll (BSShaderAccumulator::EvaluateSunOcclusionResults, every frame from
// DeferredPrePass) calls BSGraphics::Renderer::GetOcclusionQueryResults(query, wait=false, flush=true).
// GetData without D3D11_ASYNC_GETDATA_DONOTFLUSH flushes the command buffer mid-frame although the
// poll never waits; without it the answer arrives after the frame's own submission, at most a frame
// later. Community Shaders removed the same flush from Skyrim's sun-glare poll.
namespace sunglare {
using Results = std::uint32_t (*)(void*, void*, bool, bool);
constexpr cullskip::Site og{0x282F27A, 0x1D11CD0}, ae{0x21D70CA, 0x181EE70}, ng{0x2079F2A, 0x1703BA0};
const cullskip::Site* For(Runtime runtime) noexcept {
    switch (runtime) {
    case Runtime::OG: return &og;
    case Runtime::AE: return &ae;
    case Runtime::NG: return &ng;
    default: return nullptr;
    }
}
Results original{};
std::uint32_t Poll(void* renderer, void* query, bool wait, bool flush) {
    // A waiting read still flushes; the runtime toggle (ToggleKey) restores the engine's poll.
    const bool keep = wait || !enabled.load(std::memory_order_relaxed);
    return original(renderer, query, wait, keep && flush);
}
void Install(std::uintptr_t base, const cullskip::Site& site) {
    if (!cullskip::IsCallTo(base, site)) {
        logger::warn("Sun-glare occlusion poll differs from the verified engine; it keeps its flush");
        return;
    }
    original = reinterpret_cast<Results>(base+site.target);  // valid before the call points at us
    F4SE::GetTrampoline().write_call<5>(base+site.call, &Poll);
    logger::info("Sun-glare occlusion poll reads its query without a mid-frame flush");
}
}

// Sun cascade casters on a worker thread (Bryant-21's FO4CS bSunShadowWorker idea, reimplemented;
// sites mapped in Ghidra Combined for OG and AE). Outdoors with previs and without batched culling,
// FO4 fits the sun's cascades and gathers their casters (AccumulateFromLists) in DeferredLightsImpl on
// the main thread, after the G-buffer. The engine's own batched path fits them earlier, in
// MainRenderSetup (DrawWorld::AccumulateSunShadowLight). We answer that path's two
// CullingJob::QBatchEnabled queries so the fit moves there, run AccumulateFromLists on a worker while
// the main thread draws the G-buffer, and answer DeferredLightsImpl's query "batched" after joining,
// so the casters are gathered once. BSMTAManager is one global collector: any other accumulation
// waits for the worker first. Every rule and cull skip inside AccumulateFromLists runs unchanged.
// Lamps share the sun's depth array (target 6, one slice per cascade). A shadowed point lamp draws
// its second map into slice 1, the sun's middle cascade, after the sun's pass, and nothing restores
// it (Ghidra Combined 2026-10-23, scratchpad port/lamp-slots.md). A cascade the cadence keeps would
// light the next frame with the lamp's map. Every shadow map binds its slice through
// RenderShadowMap's SetCurrentDepthStencilTarget call: outside the sun's own render, a bind of target
// 6 marks its slice, and the next plan redraws a cascade on a marked slice instead of keeping it.
// The static shadow cache uses the same call to keep a slice it filled (mode 3, no clear).
namespace slotguard {
struct Site { std::uint32_t call, target, modeSetup; };  // the call, SetCurrentDepthStencilTarget, xor r8d, r8d
constexpr Site og{0x28C98CA, 0x1D31940, 0x28C98BF}, ae{0x224F5E2, 0x1839C80, 0x224F5D4}, ng{0x20F24D2, 0x171E960, 0x20F24C4};
const Site* For(Runtime runtime) noexcept {
    switch (runtime) {
    case Runtime::OG: return &og;
    case Runtime::AE: return &ae;
    case Runtime::NG: return &ng;
    default: return nullptr;
    }
}
using Bind = void (*)(void*, std::uint32_t, std::uint32_t, std::uint32_t, bool);
Bind original{};
bool installed = false;
std::atomic<std::uint32_t> marked{0};  // sun-array slices other shadow maps drew into since the last plan
bool insideSun = false;  // render thread: set around the sun's own cascade renders
bool keepDepth = false;  // render thread: the static shadow cache keeps the slice of the next sun bind
void BindDepth(void* manager, std::uint32_t target, std::uint32_t mode, std::uint32_t slice, bool flag) {
    if (target == sunDepthTarget && !insideSun && slice < 32) {
        marked.fetch_or(1u << slice, std::memory_order_relaxed);
        if (slice) Count(slotConflicts);  // slice 0 is the nearest cascade's, which always renders
    }
    original(manager, target, keepDepth ? keepSlice : mode, slice, flag);
}
std::uint32_t Take() noexcept { return marked.exchange(0, std::memory_order_relaxed); }
void Install(std::uintptr_t base, const Site& site) {
    const auto* setup = reinterpret_cast<const std::uint8_t*>(base+site.modeSetup);
    if (!cullskip::IsCallTo(base, {site.call, site.target}) || setup[0] != 0x45 || setup[1] != 0x33 || setup[2] != 0xC0) {
        logger::warn("Shadow slice guard: call site differs from the verified engine; only the far cascade is kept");
        return;
    }
    original = reinterpret_cast<Bind>(base+site.target);  // valid before the call site points at us
    F4SE::GetTrampoline().write_call<5>(base+site.call, &BindDepth);
    installed = true;
    logger::info("Shadow slice guard installed: a cascade whose slice a lamp map reused is redrawn, not kept");
}
}
// Static shadow cache (StaticShadowCache; Ghidra Combined 2026-10-23, 1.10.163 for now). While a
// cascade keeps its held camera (HoldFitFrames), its unmoving casters draw into the depth slice once
// and are copied into a spare array of the same format; on later held frames that copy is put back
// and only the moving casters (MovingCaster) draw on top, with the slice bound but not cleared.
// The first frame of a hold draws the cascade twice: unmoving casters, copy, then moving ones. The
// accumulator's auto-clear flag keeps its pass lists for the second draw; ClearActivePasses empties
// them afterwards. A copied-back slice is also immune to a lamp map that reused the slice.
namespace shadowcache {
struct Sites {
    // BSBatchRenderer::Draw(BSRenderPass*) from RenderPassImmediately_Standard/_Skinned/_Custom.
    std::uint32_t drawStandard, drawSkinned, drawCustom, draw;
    std::uint32_t clearActivePasses;  // BSShaderAccumulator::ClearActivePasses(accumulator, bool)
    std::uint32_t currentAccumulator, rendererData;  // BSShaderManager::pCurrentShaderAccumulator, RendererData*
};
constexpr Sites og{0x287FCE5, 0x287FD86, 0x287FDF7, 0x287EDE0, 0x282F080, 0x6721AB0, 0x609BF80};
constexpr Sites ae{0x22200B5, 0x2220156, 0x22201C7, 0x221F080, 0x21D6DE0, 0x3E5DEE0, 0x38E13A0};
constexpr Sites ng{0x20C2F15, 0x20C2FB6, 0x20C3027, 0x20C1EE0, 0x2079C40, 0x3CA2630, 0x3624CA0};
const Sites* For(Runtime runtime) noexcept {
    switch (runtime) {
    case Runtime::OG: return &og;
    case Runtime::AE: return &ae;
    case Runtime::NG: return &ng;
    default: return nullptr;
    }
}
constexpr std::size_t hookCount = 3;  // the depth bind is the slice guard's hook
constexpr std::size_t autoClear = 0x418;  // accumulator byte: RenderBatches consumes the pass lists when set
constexpr std::size_t rendererDevice = 0x48, rendererContext = 0x50;
using slotguard::sunDepthTarget;
using Draw = void (*)(void*);
using ClearPasses = void (*)(void*, bool);
Draw originalDraw{};
ClearPasses clearActivePasses{};
void* const* currentAccumulator{};
std::byte* const* rendererData{};
bool installed = false;
// Render thread only: set around the cascade draws below.
enum class Filter { None, Unmoving, Moving };
Filter filter = Filter::None;
const void* filterAccumulator{};
ID3D11Texture2D* cache{};
ID3D11Texture2D* cachedFrom{};
D3D11_TEXTURE2D_DESC cachedDesc{};
std::array<bool, 4> built{};
bool creationFailed = false;
// The sun shadow array as seen bound during a cascade draw (one reference held), and the slice the
// render being captured should bind. The engine maps the descriptor's target id through its render
// target manager, so the renderer's own target table cannot be indexed with it.
ID3D11Texture2D* live{};
std::uint32_t captureSlice = ~0u;
void Reset() noexcept { built = {}; }
std::byte* Renderer() noexcept { return rendererData ? *rendererData : nullptr; }
ID3D11DeviceContext* Context() noexcept {
    auto* data = Renderer();
    return data ? Read<ID3D11DeviceContext*>(data, rendererContext) : nullptr;
}
// After a draw of the captured render: the bound depth view must be slice `captureSlice` of an array.
void CaptureBound() {
    auto* context = Context();
    if (!context) return;
    ID3D11DepthStencilView* view{};
    context->OMGetRenderTargets(0, nullptr, &view);
    if (!view) return;
    D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
    view->GetDesc(&desc);
    ID3D11Resource* resource{};
    if (desc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY && desc.Texture2DArray.FirstArraySlice == captureSlice &&
        desc.Texture2DArray.ArraySize == 1) view->GetResource(&resource);
    view->Release();
    if (!resource) return;
    ID3D11Texture2D* texture{};
    resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
    resource->Release();
    if (!texture) return;
    if (texture == live) { texture->Release(); return; }
    if (live) live->Release();
    live = texture;  // keeps the reference QueryInterface added
    built = {};
}
void DrawPass(void* pass) {
    if (filter != Filter::None && *currentAccumulator == filterAccumulator &&
        MovingCaster(static_cast<const std::byte*>(pass), propertySkinned) != (filter == Filter::Moving)) return;
    originalDraw(pass);
    if (captureSlice != ~0u && filter != Filter::None && *currentAccumulator == filterAccumulator) {
        CaptureBound();
        captureSlice = ~0u;  // once per captured render
    }
}
// The spare array, (re)created to match the shadow array (resolution or renderer changes).
bool EnsureCache(ID3D11Texture2D* source) {
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (cache && cachedFrom == source && std::memcmp(&desc, &cachedDesc, sizeof(desc)) == 0) return true;
    if (cache) { cache->Release(); cache = nullptr; }
    built = {};
    if (creationFailed) return false;
    auto* device = Read<ID3D11Device*>(Renderer(), rendererDevice);
    if (!device || FAILED(device->CreateTexture2D(&desc, nullptr, &cache)) || !cache) {
        cache = nullptr;
        creationFailed = true;
        logger::warn("Static shadow cache: could not create its {}x{}x{} copy of the sun shadow array; cascades draw as usual",
            desc.Width, desc.Height, desc.ArraySize);
        return false;
    }
    cachedFrom = source;
    cachedDesc = desc;
    logger::info("Static shadow cache: {}x{}x{} copy of the sun shadow array created", desc.Width, desc.Height, desc.ArraySize);
    return true;
}
void CopySlice(ID3D11Texture2D* to, ID3D11Texture2D* from, std::uint32_t slice) {
    auto* context = Context();
    const auto subresource = D3D11CalcSubresource(0, slice, cachedDesc.MipLevels);
    context->CopySubresourceRegion(to, subresource, 0, 0, 0, from, subresource, nullptr);
}
// Draws cascade `cascade` through the cache; false leaves it to the caller's ordinary Render.
bool Render(RenderSun render, void* light, std::uint32_t cascade, std::uint64_t slot, void* accumulator, bool held) {
    const auto slice = static_cast<std::uint32_t>(slot & 0xFFFFFFFF);
    auto* data = Renderer();
    if ((slot >> 32) != sunDepthTarget || !accumulator || !data || !Context() || !Read<ID3D11Device*>(data, rendererDevice) ||
        slice >= 4) {
        Count(cacheRefused);
        return false;
    }
    // A refit renders as usual: building on that frame would draw it twice although, with Fallout's
    // tight cascade boxes, a turning view rarely keeps the new box for a second frame (2026-10-23
    // smoke run: 10210 builds, 13 reuses while panning). Build on the first frame the hold succeeds.
    if (!held) {
        built[cascade] = false;
        return false;
    }
    const auto bit = 1u << cascade;
    filterAccumulator = accumulator;
    if (built[cascade] && live && EnsureCache(live) && slice < cachedDesc.ArraySize) {
        CopySlice(live, cache, slice);
        filter = Filter::Moving;
        slotguard::keepDepth = true;
        render(light, bit);
        slotguard::keepDepth = false;
        filter = Filter::None;
        Count(cacheReused);
        return true;
    }
    auto& keepLists = *reinterpret_cast<std::uint8_t*>(static_cast<std::byte*>(accumulator) + autoClear);
    const auto saved = keepLists;
    keepLists = 0;
    filter = Filter::Unmoving;
    captureSlice = slice;  // the first unmoving draw shows which texture and slice the engine binds
    render(light, bit);
    captureSlice = ~0u;
    const bool copied = live && EnsureCache(live) && slice < cachedDesc.ArraySize;
    if (copied) CopySlice(cache, live, slice);
    filter = Filter::Moving;
    slotguard::keepDepth = true;
    render(light, bit);
    slotguard::keepDepth = false;
    filter = Filter::None;
    keepLists = saved;
    clearActivePasses(accumulator, true);
    built[cascade] = copied;  // without a captured slice the cascade still drew in full, uncached
    Count(copied ? cacheBuilt : cacheRefused);
    return true;
}
void Install(std::uintptr_t base, const Sites& sites) {
    const bool verified = slotguard::installed && cullskip::IsCallTo(base, {sites.drawStandard, sites.draw}) &&
                          cullskip::IsCallTo(base, {sites.drawSkinned, sites.draw}) && cullskip::IsCallTo(base, {sites.drawCustom, sites.draw});
    if (!verified) {
        logger::warn("Static shadow cache: call sites differ from the verified engine; cascades draw as usual");
        return;
    }
    originalDraw = reinterpret_cast<Draw>(base+sites.draw);
    clearActivePasses = reinterpret_cast<ClearPasses>(base+sites.clearActivePasses);
    currentAccumulator = reinterpret_cast<void* const*>(base+sites.currentAccumulator);
    rendererData = reinterpret_cast<std::byte* const*>(base+sites.rendererData);
    auto& trampoline = F4SE::GetTrampoline();
    for (const auto call : {sites.drawStandard, sites.drawSkinned, sites.drawCustom})
        trampoline.write_call<5>(base+call, &DrawPass);
    installed = true;
    logger::info("Static shadow cache installed: held cascades copy their unmoving casters back and draw only moving ones");
}
}
namespace sunworker {
struct Sites {
    std::uint32_t cameras, accumulate, deferred, begin;  // calls to rewrite
    std::uint32_t batchEnabled, beginAccumulation, accumulateFromLists, preCullEnabled, setContext, setThreadEventFlag;
    // Engine calls that prove the three functions we call directly: AccumulateFromLists'
    // BSPreCulledObjects::QEnabled, a job-list serving thread's Renderer::SetContext and
    // BSJobs::JobThread's BSSystemFileStreamer::SetThreadEventFlag (Ghidra Combined).
    std::uint32_t preCullCall, setContextCall, setThreadEventFlagCall;
};
constexpr Sites og{0x2850384, 0x28503B1, 0x28537D6, 0x1CCD54C, 0x1CCB850, 0x28608F0, 0x28CA7D0, 0x2809E30, 0x1D14CB0, 0x1B48680,
                   0x28CA88D, 0xCC0751, 0x1B53F31};
constexpr Sites ae{0x21F3218, 0x21F3245, 0x21F5CE5, 0x17E08EA, 0x17DE920, 0x2202500, 0x2250400, 0x21AE520, 0x1821EB0, 0x1681A90,
                   0x22504B9, 0xBD7461, 0x1687D11};
constexpr Sites ng{0x2096078, 0x20960A5, 0x2098B45, 0x16C568A, 0x16C36C0, 0x20A5360, 0x20F32F0, 0x2051380, 0x1706BE0, 0x1567360,
                   0x20F33A9, 0xB57B11, 0x156D5E1};
const Sites* For(Runtime runtime) noexcept {
    switch (runtime) {
    case Runtime::OG: return &og;
    case Runtime::AE: return &ae;
    case Runtime::NG: return &ng;
    default: return nullptr;
    }
}
constexpr std::size_t hookCount = 4;
constexpr std::size_t sunLight = 0x208;  // ShadowSceneNode: the sun's BSShadowDirectionalLight
using Query = bool (*)();
using Accumulate = void (*)(void*, void*);
constexpr std::uint32_t ogVisCamera = 0x6723240;  // DrawWorld::pVisCamera, the camera the sun's fit receives
using Begin = std::uintptr_t (*)(void*);
Query batchEnabled{}, preCullEnabled{};
Accumulate accumulate{};
Begin beginAccumulation{};
void (*setContext)(void*, void*){};
void* (*setThreadEventFlag)(void*){};
Accumulate cull{};  // this frame's cascade cull: the engine's, or CBRO's wrapper of it (ChooseCull)
const bool* shadows{};
void* const* sceneNode{};
void* cullingGroup{};
HANDLE kick{}, done{};
DWORD workerId{};
std::atomic<DWORD> frameThread{};  // the render thread that owns the frame (set by CamerasQuery)
void* light{};
std::atomic_bool inFlight{false};
bool engaged{false};  // frame thread only: from the cameras query to DeferredLightsImpl's

DWORD WINAPI Worker(void*) {
    WaitForSingleObject(kick, INFINITE);
    // Set up as the engine's job threads are, once the renderer exists: pass building records
    // command buffers through the thread's render context, and the file streamer needs an event.
    setContext(nullptr, nullptr);
    static HANDLE streamerEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    setThreadEventFlag(&streamerEvent);
    for (;; WaitForSingleObject(kick, INFINITE)) {
        cull(light, cullingGroup);  // the engine's entry (or CBRO's wrapper), so every hook on it still runs
        SetEvent(done);
    }
}
// Frame thread only. Only the thread that kicks the worker clears inFlight: a job thread that
// cleared it after a late wait could race the next frame's kick, and DeferredLightsImpl would
// then render cascades the worker is still gathering.
void Join() {
    if (inFlight.load(std::memory_order_acquire)) {
        WaitForSingleObject(done, INFINITE);
        inFlight.store(false, std::memory_order_release);
    }
}
// The engine's own cascade cull call in DeferredLightsImpl (OG 1108521+0xE49), the one CBRO wraps.
constexpr std::uint32_t ogInlineCull = 0x28537F9;
// Where a call's target finally lands: through a trampoline's absolute or relative jump to its function.
std::uintptr_t Landing(std::uintptr_t call) noexcept {
    std::int32_t relative{};
    std::memcpy(&relative, reinterpret_cast<const void*>(call+1), sizeof(relative));
    auto target = call+5+relative;
    for (int hop = 0; hop < 2; ++hop) {
        const auto* code = reinterpret_cast<const std::uint8_t*>(target);
        if (code[0] == 0xFF && code[1] == 0x25 && !code[2] && !code[3] && !code[4] && !code[5])
            target = *reinterpret_cast<const std::uintptr_t*>(target+6);
        else if (code[0] == 0xE9) {
            std::memcpy(&relative, code+1, sizeof(relative));
            target += 5+relative;
        } else break;
    }
    return target;
}
// The cascade cull the worker calls. With CBRO it is CBRO's wrapper at the engine's own call site, so the cull
// sees CBRO's previs suspension and its trimmed casters exactly as on the main thread (CBRO's main-cull window
// has closed before MainRenderSetup kicks the worker). Any other wrapper there keeps the cull on the main thread.
bool ChooseCull() {
    cull = accumulate;
    if (!cbro) return true;
    const auto call = imageBase+ogInlineCull;
    std::int32_t relative{};
    std::memcpy(&relative, reinterpret_cast<const void*>(call+1), sizeof(relative));
    const auto target = call+5+relative;
    if (*reinterpret_cast<const std::uint8_t*>(call) != 0xE8) return false;
    if (target == reinterpret_cast<std::uintptr_t>(accumulate)) return true;
    HMODULE owner{};
    const bool cbroOwned = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                              reinterpret_cast<LPCWSTR>(Landing(call)), &owner) && owner == GetModuleHandleW(L"CBRO.dll");
    static bool announced = false;
    if (!announced) {
        announced = true;
        if (cbroOwned) logger::info("Sun cull worker gathers through CBRO's cascade cull");
        else logger::warn("Another plugin wraps the sun cascade cull; the main thread keeps gathering sun casters");
    }
    if (!cbroOwned) return false;
    cull = reinterpret_cast<Accumulate>(target);
    return true;
}
bool CamerasQuery() {
    frameThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    const bool batch = batchEnabled();
    Join();  // a frame that never reached DeferredLightsImpl
    // The runtime toggle (ToggleKey) returns the gather to the main thread from the next frame.
    engaged = enabled.load(std::memory_order_relaxed) && liveWorker.load(std::memory_order_relaxed) && !batch && *shadows && preCullEnabled() &&
              ChooseCull();
    return batch || engaged;
}
bool AccumulateQuery() {
    if (!engaged) return batchEnabled();
    const auto* node = static_cast<const std::byte*>(*sceneNode);
    light = node ? *reinterpret_cast<void* const*>(node+sunLight) : nullptr;
    if (!light) {
        engaged = false;
        return false;  // no sun light: DeferredLightsImpl's own path decides as usual
    }
    // With CBRO the light's fit just ran (AccumulateSunShadowLight) without our update hook: publish here, on the
    // frame thread, before the worker registers anything.
    if (cbro) PublishSun(light, Read<const RE::NiCamera*>(reinterpret_cast<void*>(imageBase+ogVisCamera), 0));
    ResetEvent(done);
    inFlight.store(true, std::memory_order_release);
    SetEvent(kick);
    static bool announced = false;
    if (!announced) {
        announced = true;
        logger::info("Sun cull worker engaged: sun casters now gather beside the G-buffer");
    }
    return false;  // the worker gathers; MainRenderSetup does not
}
bool DeferredQuery() {
    if (!engaged) return batchEnabled();
    engaged = false;
    Join();
    return true;  // gathered already: skip DeferredLightsImpl's inline fit and gather
}
std::uintptr_t BeginGuard(void* accumulator) {
    const auto self = GetCurrentThreadId();
    if (self != workerId && inFlight.load(std::memory_order_acquire)) {
        if (self == frameThread.load(std::memory_order_relaxed)) Join();
        else WaitForSingleObject(done, INFINITE);  // other threads only wait; the frame thread clears
    }
    return beginAccumulation(accumulator);
}
void Install(std::uintptr_t base, const Sites& sites) {
    const auto operands = DecodeSunAccumulateSite(reinterpret_cast<const std::uint8_t*>(base+sites.accumulate), base+sites.accumulate);
    const bool calls = cullskip::IsCallTo(base, {sites.cameras, sites.batchEnabled}) &&
                       cullskip::IsCallTo(base, {sites.accumulate, sites.batchEnabled}) &&
                       cullskip::IsCallTo(base, {sites.deferred, sites.batchEnabled}) &&
                       cullskip::IsCallTo(base, {sites.begin, sites.beginAccumulation}) &&
                       cullskip::IsCallTo(base, {sites.preCullCall, sites.preCullEnabled}) &&
                       cullskip::IsCallTo(base, {sites.setContextCall, sites.setContext}) &&
                       cullskip::IsCallTo(base, {sites.setThreadEventFlagCall, sites.setThreadEventFlag});
    if (!operands || !calls) {
        logger::warn("Sun cull worker: call sites differ from the verified engine; the main thread keeps gathering sun casters");
        return;
    }
    batchEnabled = reinterpret_cast<Query>(base+sites.batchEnabled);
    preCullEnabled = reinterpret_cast<Query>(base+sites.preCullEnabled);
    accumulate = reinterpret_cast<Accumulate>(base+sites.accumulateFromLists);
    beginAccumulation = reinterpret_cast<Begin>(base+sites.beginAccumulation);
    setContext = reinterpret_cast<void (*)(void*, void*)>(base+sites.setContext);
    setThreadEventFlag = reinterpret_cast<void* (*)(void*)>(base+sites.setThreadEventFlag);
    shadows = reinterpret_cast<const bool*>(operands->shadows);
    sceneNode = reinterpret_cast<void* const*>(operands->sceneNode);
    cullingGroup = reinterpret_cast<void*>(operands->cullingGroup);
    kick = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    done = CreateEventW(nullptr, TRUE, TRUE, nullptr);  // manual reset: every joiner sees it
    const HANDLE thread = kick && done ? CreateThread(nullptr, 0, &Worker, nullptr, 0, &workerId) : nullptr;
    if (!thread) {
        logger::warn("Sun cull worker: could not start its thread; the main thread keeps gathering sun casters");
        return;
    }
    SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);  // the main thread waits on it
    SetThreadDescription(thread, L"Faster Shadows sun cull");
    CloseHandle(thread);
    auto& trampoline = F4SE::GetTrampoline();
    trampoline.write_call<5>(base+sites.cameras, &CamerasQuery);
    trampoline.write_call<5>(base+sites.accumulate, &AccumulateQuery);
    trampoline.write_call<5>(base+sites.deferred, &DeferredQuery);
    trampoline.write_call<5>(base+sites.begin, &BeginGuard);
    logger::info("Sun cull worker installed: outdoors with previs, sun casters are gathered beside the G-buffer");
}
}

// Next to CBRO (OG only) the sun state is published from BSShadowDirectionalLight::AccumulateFromLists' first call,
// BSGeometryListCullingProcess::ClearCollectedGeometry (Ghidra Combined, 2026-10-24). The batched and the inline
// path both reach it right after the light's fit, before any caster is culled or registered; the light is in rdi
// there (mov rdi, rcx at +0x23) and a stub passes it on. The worker's culls were published before the kick.
namespace cbropublish {
constexpr std::uint32_t call = 0x28CA800, target = 0x2977E80, checked = 0x28CA7F3;
// mov rdi, rcx; mov rcx, [rcx+0x2C8]; mov r14, rdx
constexpr std::array<std::uint8_t, 13> prologue{0x48, 0x8B, 0xF9, 0x48, 0x8B, 0x89, 0xC8, 0x02, 0x00, 0x00, 0x4C, 0x8B, 0xF2};
constexpr std::size_t stubBytes = 15;
using Clear = void (*)(void*);
Clear original{};
bool installed = false;
void Run(void* process, void* light) {
    if (GetCurrentThreadId() != sunworker::workerId)
        PublishSun(light, Read<const RE::NiCamera*>(reinterpret_cast<void*>(imageBase+sunworker::ogVisCamera), 0));
    original(process);
}
bool Install(std::uintptr_t base) {
    if (std::memcmp(reinterpret_cast<const void*>(base+checked), prologue.data(), prologue.size()) ||
        !cullskip::IsCallTo(base, {call, target})) {
        logger::warn("CBRO: the sun cascade cull differs from the verified engine; the cascades are not staggered");
        return false;
    }
    original = reinterpret_cast<Clear>(base+target);  // valid before the call site points at us
    // mov rdx, rdi; mov rax, Run; jmp rax
    std::array<std::uint8_t, stubBytes> code{0x48, 0x8B, 0xD7, 0x48, 0xB8};
    const auto run = reinterpret_cast<std::uintptr_t>(&Run);
    std::memcpy(code.data()+5, &run, sizeof(run));
    code[13] = 0xFF;
    code[14] = 0xE0;
    auto& trampoline = F4SE::GetTrampoline();
    auto* stub = trampoline.allocate(code.size());
    std::memcpy(stub, code.data(), code.size());
    trampoline.write_call<5>(base+call, reinterpret_cast<std::uintptr_t>(stub));
    installed = true;
    return true;
}
}

// Fallout 4 VR 1.2.72 (Ghidra Combined and the VR executable, 2026-10-24). VR's sun light has no
// virtual camera update; its cascade gather 0x290D640(light, group, mask, mode) runs once per frame,
// after the fit, on the unbatched (DeferredLightsImpl) and the batched path. The frame's sun state is
// published there, before the casters register.
namespace vrsun {
constexpr std::array<cullskip::Site, 2> calls{{{0x2847921, 0x290D640}, {0x2844154, 0x290D640}}};
constexpr std::uint32_t visibleCamera = 0x6885C88;  // the camera the fit receives
using Gather = std::uintptr_t (*)(void*, void*, std::uintptr_t, std::uintptr_t);
Gather original{};
std::uintptr_t Run(void* light, void* group, std::uintptr_t mask, std::uintptr_t mode) {
    PublishSun(light, Read<const RE::NiCamera*>(reinterpret_cast<void*>(imageBase+visibleCamera), 0));
    return original(light, group, mask, mode);
}
bool Install(std::uintptr_t base) {
    for (const auto& site : calls)
        if (!cullskip::IsCallTo(base, site)) {
            logger::warn("VR sun gather call sites differ from the verified engine; the sun rules stay off");
            return false;
        }
    original = reinterpret_cast<Gather>(base+calls[0].target);
    auto& trampoline = F4SE::GetTrampoline();
    for (const auto& site : calls) trampoline.write_call<5>(base+site.call, &Run);
    return true;
}
}

// VR staggers its sun cascades itself (VrCadence.h): this adds stale cascades to the mask its
// DrawWorld render (0x284E9E0) computed, right after the last mask write (0x284EA67), at the call
// to 0x27AE800 that follows it. Everything that reads the mask (the pre-cull jobs, the fit, the
// gather, the draw) comes later. Fallout 4 VR Shadow Boost rewrites the writer's table and
// threshold above that call; only the 15 bytes from 0x284EA63 are checked here.
namespace vrmask {
constexpr std::uint32_t call = 0x284EA6D, target = 0x27AE800, checked = 0x284EA63;
constexpr std::array<std::uint8_t, 15> expected{0x8b, 0x44, 0x84, 0x60, 0x89, 0x05, 0x57, 0x72, 0x03, 0x04,
                                                0xe8, 0x8e, 0xfd, 0xf5, 0xff};
constexpr std::uint32_t mask = 0x6885CC4, nextUpdate = 0x6885CCC, frameCounter = 0x65A2B48, updateDelay = 0x391FE08;
constexpr std::uint32_t sceneNode = 0x6885D40;
constexpr std::size_t nodeSunLight = 0x248, splitEnds = 0x240;
// Shadow Boost's preloader can replace the batched fit call (a jmp at 0x28440F5); that path runs
// only while the batched-render byte is set. Its wrapper refits every cascade, so nothing is added then.
constexpr std::uint32_t wrapperSite = 0x28440F5, batchedPath = 0x388A808;
using Query = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);
Query original{};
VrCadence planner;  // render thread only
std::atomic<bool> resetRequested{false};
std::atomic<std::uint64_t> added{0}, forced{0};
template<class T> T Global(std::uint32_t rva) noexcept { return Read<T>(reinterpret_cast<void*>(imageBase+rva), 0); }
// Cascades the game can refit and draw this frame: present, with a camera, both accumulators and a
// view range that is not empty.
std::uint32_t Complete(const void* light) noexcept {
    const auto count = std::min<std::uint32_t>(Read<std::uint32_t>(light, descriptorCount), maxCascades);
    const auto* descriptors = Read<const std::byte*>(light, descriptorArray);
    if (!descriptors) return 0;
    const auto ends = Read<std::array<float, 4>>(light, splitEnds);
    std::uint32_t complete = 0;
    float reached = 0.0f;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto* descriptor = descriptors + i*vrLayout.descriptorStride;
        const bool parts = Read<const void*>(descriptor, descriptorCamera) && Read<const void*>(descriptor, vrLayout.descriptorAccumulator) &&
                           Read<const void*>(descriptor, vrLayout.dynamicAccumulator);
        if (parts && std::isfinite(ends[i]) && ends[i] > reached) complete |= 1u << i;
        if (std::isfinite(ends[i])) reached = std::max(reached, ends[i]);
    }
    return complete;
}
void Adjust() {
    if (resetRequested.exchange(false, std::memory_order_acq_rel)) planner.Reset();
    VrFrame in;
    in.native = Global<std::uint32_t>(mask) & 0xF;
    in.frame = Global<std::int32_t>(frameCounter);
    in.nextUpdate = Global<std::int32_t>(nextUpdate);
    in.delay = std::max(1, Global<std::int32_t>(updateDelay));
    in.wrapperLive = Global<std::uint8_t>(wrapperSite) == 0xE9 && Global<std::uint8_t>(batchedPath) != 0;
    const auto* node = Global<const std::byte*>(sceneNode);
    const auto* light = node ? Read<const void*>(node, nodeSunLight) : nullptr;
    const auto* camera = worldCamera ? worldCamera() : nullptr;
    if (light && camera && enabled.load(std::memory_order_relaxed) && liveVrRedraw.load(std::memory_order_relaxed)) {
        in.eye = Position(camera->world.translate);
        const auto& m = camera->world.rotate.entry;
        in.rotation = {m[0].x, m[0].y, m[0].z, m[1].x, m[1].y, m[1].z, m[2].x, m[2].y, m[2].z};
        in.sun = Read<Vec3>(light, vrLayout.sunTarget);
        in.cell = reinterpret_cast<std::uintptr_t>(PlayerCell());
        if (Finite(in.eye)) in.allowed = Complete(light);
    }
    const auto plan = planner.Plan(settings.vr, in);
    if (plan.forceUpdate) {
        *reinterpret_cast<volatile std::int32_t*>(imageBase+nextUpdate) = 0;  // as the engine does on light creation
        forced.fetch_add(1, std::memory_order_relaxed);
    }
    if (plan.added) {
        *reinterpret_cast<volatile std::uint32_t*>(imageBase+mask) = Global<std::uint32_t>(mask) | plan.added;
        added.fetch_add(std::popcount(plan.added), std::memory_order_relaxed);
    }
}
std::uintptr_t Run(std::uintptr_t a, std::uintptr_t b, std::uintptr_t c, std::uintptr_t d) {
    const auto result = original(a, b, c, d);
    if (hooksReady.load(std::memory_order_acquire)) {
        Poll();
        Adjust();
    }
    return result;
}
bool Install(std::uintptr_t base) {
    if (std::memcmp(reinterpret_cast<const void*>(base+checked), expected.data(), expected.size()) ||
        !cullskip::IsCallTo(base, {call, target})) {
        logger::warn("VR cascade mask site differs from the verified engine; the game's own cascade schedule stays as it is");
        return false;
    }
    original = reinterpret_cast<Query>(base+target);
    F4SE::GetTrampoline().write_call<5>(base+call, &Run);
    return true;
}
}
std::uint64_t VrAdded() noexcept { return vrmask::added.load(std::memory_order_relaxed); }
std::uint64_t VrForced() noexcept { return vrmask::forced.load(std::memory_order_relaxed); }


// Cascade casters arrive here from the culling groups (previs feed, precombines,
// group 0), possibly on BSMTAManager jobs: identify the pass by accumulator only.
bool SunRegister(void* accumulator, RE::NiAVObject* object, void* property, std::uint32_t sub) {
    if (!settings.cascadeCadence && !ShouldEvaluateCasterPolicies(settings.dryRun, settings.diagnostics))
        return nativeShadow(accumulator, object, property, sub);
    if (object && hooksReady.load(std::memory_order_acquire)) {
        if (const auto* frame = CurrentSun()) {
            // VR registers a cascade's static casters into one accumulator and its moving ones into a
            // second; a caster reaches exactly one of them, so dropping it there drops its shadow.
            std::uint32_t cascade = 0;
            while (cascade < frame->count && frame->accumulators[cascade] != accumulator &&
                   frame->dynamicAccumulators[cascade] != accumulator) ++cascade;
            // This cascade keeps last frame's map: register nothing, so its accumulator
            // stays empty while Render leaves it out.
            if (cascade < frame->count && (frame->skip >> cascade & 1)) {
                Count(cadenceDropped);
                return true;
            }
            if (!enabled.load(std::memory_order_relaxed) || !RulesActive())
                return nativeShadow(accumulator, object, property, sub);
            if (cascade < frame->count) {
                const auto bound = Bound(*object);
                bool cull = false;
                if (!Skinned(property)) {
                    if (settings.sun.enabled) {
                        Count(sunTested);
                        cull = CullSunPrepared(frame->sunRule, bound, frame->eye, cascade);
                        if (cull) Count(sunCandidates);
                    }
                } else if (settings.actorShadows && frame->actorsAllowed && actorrule::Culled(object, bound, frame->eye)) {
                    cull = true;
                    Count(actorCandidates);
                }
                if (cull && !settings.dryRun) {
                    Count(removedTotal);
                    return true;
                }
            } else {
                Count(otherCalls);
            }
        }
    }
    return nativeShadow(accumulator, object, property, sub);
}

bool SpotRegister(void* accumulator, RE::NiAVObject* object, void* property, std::uint32_t sub) {
    if (object && accumulator && hooksReady.load(std::memory_order_acquire) && enabled.load(std::memory_order_relaxed) &&
        RulesActive() && PointCellAllowed()) {
        const auto* camera = Read<const RE::NiCamera*>(accumulator, accumulatorCamera);
        // An orthographic frustum light (focus shadow) has no lamp position.
        const auto [reach, ortho] = camera ? FarAndOrtho(camera) : std::pair{0.0f, true};
        if (camera && !ortho) {
            const bool skinned = Skinned(property);
            const bool rule = !skinned && settings.point.enabled;
            const bool actorRule = skinned && settings.actorShadows && settings.actorPointLights;
            const auto* viewer = (rule || actorRule) ? worldCamera() : nullptr;
            if (viewer) {
                const auto eye = Position(viewer->world.translate);
                const auto bound = Bound(*object);
                bool cull = false;
                if (rule) {
                    Count(spotTested);
                    // Night scenes can register thousands of casters per spot map: use the
                    // squared predicate, which never removes a reference-kept caster.
                    const auto light = Position(camera->world.translate);
                    cull = (optimizationMask.load(std::memory_order_relaxed) & SquaredDistance) ?
                        CullPointSquared(preparedPoint, bound, eye, light, reach) : CullPoint(settings.point, bound, eye, light, reach);
                    if (cull) Count(spotCandidates);
                } else if (actorrule::Culled(object, bound, eye)) {
                    cull = true;
                    Count(actorCandidates);
                }
                if (cull && !settings.dryRun) {
                    Count(removedTotal);
                    return true;
                }
            }
        }
    }
    return nativeShadow(accumulator, object, property, sub);
}

bool OcclusionRegister(void* accumulator, RE::NiAVObject* object, void* property, std::uint32_t sub) {
    if (object && settings.rainOcclusion && hooksReady.load(std::memory_order_acquire) && enabled.load(std::memory_order_relaxed) &&
        RulesActive()) {
        Count(rainTested);
        if (CullRainOccluder(true, settings.rainMinRadius, object->worldBound.fRadius)) {
            Count(rainCandidates);
            if (!settings.dryRun) {
                Count(removedTotal);
                return true;
            }
        }
    }
    return nativeOcclusion(accumulator, object, property, sub);
}

bool PointWanted() noexcept { return settings.point.enabled || (settings.actorShadows && settings.actorPointLights); }

void PointBegin(void* culler, const RE::NiCamera* camera, RE::NiAVObject* object, void* array) {
    if (!hooksReady.load(std::memory_order_acquire)) {
        originalBeginPoint(culler, camera, object, array);
        return;
    }
    Poll();
    if (!PointWanted() || !enabled.load(std::memory_order_relaxed) || !RulesActive()) {
        // Suppress an outer context during a disabled nested traversal.
        const auto previous = pointContext;
        pointContext = nullptr;
        originalBeginPoint(culler, camera, object, array);
        pointContext = previous;
        return;
    }
    PointContext local{};
    struct RestoreContext {
        PointContext* previous;
        PointContext& current;
        ~RestoreContext() {
            if (current.mask & BatchCounters) {
                diagnostics.Add(pointTested,current.tested);
                diagnostics.Add(pointCandidates,current.candidates);
            }
            diagnostics.Add(volumeCandidates,current.volumeCandidates);
            diagnostics.Add(memoHits,current.memoHits);
            diagnostics.Add(memoMisses,current.memoMisses);
            diagnostics.Add(actorCandidates,current.actors);
            diagnostics.Add(pointRemoved,current.removed);
            diagnostics.Add(removedTotal,current.removed);
            pointContext = previous;
        }
    } restore{pointContext,local};
    pointContext = &local;
    local.mask=optimizationMask.load(std::memory_order_relaxed);
    // The point camera is the lamp, not the player. Query the current world camera,
    // and the actual current cell, so a door cannot reuse an exterior snapshot.
    if (camera && PointCellAllowed()) {
        if (const auto* viewing = worldCamera()) {
            const auto eye = Position(viewing->world.translate);
            if (Finite(eye)) {
                local.culler=culler;local.eye=eye;local.valid=true;
                // SetCamera stored the lamp sphere in the culler before this traversal.
                local.lightCamera=camera;local.light=Read<Vec3>(culler,lampCenter);
                if ((local.mask & ShadowVolume) && !viewing->viewFrustum.ortho)
                    local.view=WideReceiverFrustum(viewing->worldToCam);
                if (local.mask & DecisionMemo) local.traversal=decisionCache.Begin();
            }
        }
    }
    originalBeginPoint(culler, camera, object, array);
}
void PointAppend(void* culler, RE::BSGeometry* geometry) {
    auto* context=pointContext;
    if (hooksReady.load(std::memory_order_acquire) && context && context->valid && context->culler == culler &&
        ((context->mask & LightSnapshot) || enabled.load(std::memory_order_relaxed)) && geometry) {
        if (const auto* camera = Read<const RE::NiCamera*>(culler, cullerCamera)) {
            const auto bound=Bound(*geometry);
            std::uint8_t decision=kKeepCaster;
            if (Read<const void*>(geometry, layout->skinInstance)) {
                if (settings.actorShadows && settings.actorPointLights && actorrule::Culled(geometry, bound, context->eye)) {
                    decision=kFarActor;
                    if (settings.diagnostics) ++context->actors;
                }
            } else if (settings.point.enabled) {
                if (settings.diagnostics) {
                    if (context->mask & BatchCounters) ++context->tested;
                    else diagnostics.Add(pointTested);
                }
                // A different native camera always falls back to the culler's live lamp sphere.
                // The synchronous Begin traversal owns the original snapshot.
                const auto light=PointLightPosition(context->mask,context->lightCamera,camera,context->light,
                    [culler](const RE::NiCamera*) { return Read<Vec3>(culler,lampCenter); });
                const float reach=Read<float>(culler,lampReach);
                const DecisionKey key{reinterpret_cast<std::uintptr_t>(geometry),bound,light};
                const bool cached=(context->mask & DecisionMemo) && decisionCache.Find(context->traversal,key,decision);
                if (cached) { if (settings.diagnostics) ++context->memoHits; }
                else {
                    if (settings.diagnostics && (context->mask & DecisionMemo)) ++context->memoMisses;
                    if ((context->mask & SquaredDistance) ? CullPointSquared(preparedPoint,bound,context->eye,light,reach) :
                        CullPoint(settings.point,bound,context->eye,light,reach)) decision=kSmallCaster;
                    else if ((context->mask & ShadowVolume) && CullPointShadowVolume(context->view,bound,light)) decision=kVolumeOnly;
                    if (context->mask & DecisionMemo) decisionCache.Put(context->traversal,key,decision);
                }
                if (settings.diagnostics) {
                    if (decision==kSmallCaster) {
                        if (context->mask & BatchCounters) ++context->candidates;
                        else diagnostics.Add(pointCandidates);
                    } else if (decision==kVolumeOnly) ++context->volumeCandidates;
                }
            }
            if (RemovePointDecision(decision,settings.dryRun)) {
                if (settings.diagnostics) ++context->removed;
                return;
            }
        }
    }
    originalPointAppend(culler, geometry);
}

const Profile* RuntimeProfile(const REL::Version& version) {
    for (const auto& profile : profiles) {
        if (version == REL::Version(profile.version[0], profile.version[1], profile.version[2], profile.version[3])) return &profile;
    }
    return nullptr;
}
bool HandlerMatches(const HandlerSite& site) noexcept {
    return std::memcmp(reinterpret_cast<const void*>(imageBase+site.target), site.prefix.data(), site.prefix.size()) == 0;
}
}
bool SupportedRuntime(const REL::Version& version) { return RuntimeProfile(version) != nullptr; }

bool InstallShadowHooks(const Settings& configuration) {
    if (installed) return hooksReady.load(std::memory_order_acquire);
    settings = configuration;
    activeProfile = RuntimeProfile(REL::Module::get().version());
    if (!activeProfile) return false;
    const auto runtime = RuntimeOf(activeProfile);
    // CBRO leaves out the sun's shadow casters it can prove unseen and trims the far cascade; Faster Shadows then
    // staggers the cascades and gathers on its worker what CBRO left. Its own caster rules stay off: CBRO culls the
    // same lamp passes. CBRO installs nothing on other runtimes, and there nothing changes here.
    const bool cbroLoaded = GetModuleHandleW(L"CBRO.dll") != nullptr;
    cbro = cbroLoaded && runtime == Runtime::OG;
    if (cbro) {
        settings.sun.enabled = settings.point.enabled = settings.actorShadows = settings.rainOcclusion = false;
        logger::info("CBRO is loaded: it trims the sun's shadow casters, and Faster Shadows staggers the cascades it leaves; the caster rules stay off");
    } else if (cbroLoaded) {
        logger::info("CBRO is loaded but works on Fallout 4 1.10.163 only; Faster Shadows runs as usual");
    }
    if (runtime == Runtime::VR) {
        // VR staggers and caches its cascades itself (vrmask adds to that) and its renderer context is
        // process-wide: the desktop cascade features and the worker thread stay off, and so does the
        // shadow-volume analysis (VR's world camera holds both eyes).
        layout = &vrLayout;
        settings.cascadeCadence = false;
        settings.sunCullWorker = false;
        settings.sunGlareNoFlush = false;
        settings.holdFitFrames = 0;
        settings.optimizations &= ~(ShadowVolume | SunTurnTolerance | SkipEmptyCascades | HoldFit | ShadowCache);
    }
    preparedPoint=PreparedPointRule(settings.point);
    optimizationMask.store(settings.optimizations,std::memory_order_relaxed);
    const bool cadenceWanted = settings.cascadeCadence;
    const bool sunWanted = settings.sun.enabled || settings.actorShadows || cadenceWanted;
    // The sun cull worker and the sun-glare poll are call-site changes; they follow the runtime
    // toggle, which the per-frame sun update polls.
    const bool engineWanted = settings.sunCullWorker || settings.sunGlareNoFlush;
    // Keep an existing, verified per-frame control callback for rain-only
    // configurations too. Polling must not depend on a sun caster rule being on.
    const bool updateWanted = sunWanted || settings.rainOcclusion || engineWanted;
    const bool pointWanted = PointWanted();
    const bool spotWanted = settings.spotLights && pointWanted;
    const bool vrMaskWanted = runtime == Runtime::VR && settings.vr.enabled;
    if (!settings.enabled || (!sunWanted && !pointWanted && !settings.rainOcclusion && !engineWanted && !vrMaskWanted)) {
        logger::info("No shadow rules enabled; no hooks installed");
        return true;
    }
    const auto* profile = activeProfile;
    const auto base = imageBase = REL::Module::get().base();
    if ((pointWanted || spotWanted) && std::memcmp(reinterpret_cast<void*>(base+profile->camera), profile->cameraPrefix.data(), profile->cameraPrefix.size())) {
        logger::error("World camera getter differs from verified engine; installing nothing");
        return false;
    }
    if (((sunWanted || spotWanted) && !HandlerMatches(profile->shadowHandler)) ||
        (settings.rainOcclusion && !HandlerMatches(profile->occlusionHandler))) {
        logger::error("Shadow-map registration handler differs from verified engine; installing nothing");
        return false;
    }
    nativeShadow = reinterpret_cast<RegisterHandler>(base+profile->shadowHandler.target);
    nativeOcclusion = reinterpret_cast<RegisterHandler>(base+profile->occlusionHandler.target);
    const std::array<std::uintptr_t, 4> replacements{
        reinterpret_cast<std::uintptr_t>(&SunUpdate), reinterpret_cast<std::uintptr_t>(&PointBegin),
        reinterpret_cast<std::uintptr_t>(&PointAppend), reinterpret_cast<std::uintptr_t>(&SunRender)};
    std::array<std::uintptr_t, 4> originals{};
    std::array<std::size_t, 4> selected{};
    std::size_t selectedCount = 0;
    for (std::size_t i = 0; i < profile->hooks.size(); ++i) {
        // VR's sun light has no camera-update slot: vrsun publishes the frame instead.
        // Next to CBRO the sun update slot stays the engine's: cbropublish publishes instead.
        if (i == 0 ? updateWanted && runtime != Runtime::VR && !cbro : i == 3 ? cadenceWanted : pointWanted) selected[selectedCount++] = i;
    }
    const std::span<const std::size_t> activeSites(selected.data(), selectedCount);
    // Check every required site before any write. Disabled features touch no
    // vtable pages and do not conflict with another mod at their unused sites.
    for (const auto i : activeSites) {
        const auto& site = profile->hooks[i];
        originals[i] = Read<std::uintptr_t>(reinterpret_cast<void*>(base+site.table), site.slot*8);
        if (!NativeHookSite(originals[i], base+site.target,
                {reinterpret_cast<const std::uint8_t*>(base+site.target), site.prefix.size()}, site.prefix)) {
            logger::error("Hook {} differs from verified engine; installing nothing", i);
            return false;
        }
    }
    originalSun = reinterpret_cast<UpdateSun>(originals[0]);
    originalBeginPoint = reinterpret_cast<BeginPoint>(originals[1]);
    originalPointAppend = reinterpret_cast<Append>(originals[2]);
    originalRender = reinterpret_cast<RenderSun>(originals[3]);
    worldCamera = reinterpret_cast<WorldCamera>(base+profile->camera);
    playerSlot = base+profile->playerSlot;
    // Keep original page protections across rollback attempts. Restoring one
    // page must not prevent rolling its pointer writes back if another fails.
    struct PatchBackend {
        struct Page { void* address{}; DWORD protection{}; bool writable{}; };
        std::uintptr_t base;
        const Profile& profile;
        std::span<const std::size_t> selected;
        const std::array<std::uintptr_t,4>& originals;
        const std::array<std::uintptr_t,4>& replacements;
        std::array<Page,4> pages{};
        std::size_t pageCount{};
        DWORD pageSize{};
        void* volatile* Address(std::size_t index) const {
            const auto& site=profile.hooks[selected[index]];
            return reinterpret_cast<void* volatile*>(base+site.table+site.slot*8);
        }
        Page& PageFor(std::size_t index) {
            const auto address=reinterpret_cast<std::uintptr_t>(Address(index));
            auto* page=reinterpret_cast<void*>(address-address%pageSize);
            for (std::size_t i=0;i<pageCount;++i) if (pages[i].address==page) return pages[i];
            pages[pageCount].address=page;
            return pages[pageCount++];
        }
        bool Prepare() {
            for (std::size_t i=0;i<selected.size();++i) {
                auto& page=PageFor(i);
                if (page.writable) continue;
                if (!VirtualProtect(page.address,pageSize,PAGE_READWRITE,&page.protection)) {
                    logger::error("Could not prepare vtable page ({})",GetLastError());
                    return false;
                }
                page.writable=true;
            }
            return true;
        }
        bool Restore() {
            bool ok=true;
            for (std::size_t i=0;i<pageCount;++i) {
                auto& page=pages[i];
                if (!page.writable) continue;
                DWORD ignored{};
                if (VirtualProtect(page.address,pageSize,page.protection,&ignored)) page.writable=false;
                else {
                    logger::error("Could not restore vtable page protection ({})",GetLastError());
                    ok=false;
                }
            }
            return ok;
        }
        bool Replace(std::size_t index) {
            const auto i=selected[index];
            return InterlockedCompareExchangePointer(Address(index),reinterpret_cast<void*>(replacements[i]),
                reinterpret_cast<void*>(originals[i]))==reinterpret_cast<void*>(originals[i]);
        }
        void Rollback(std::size_t index) {
            auto& page=PageFor(index);
            if (!page.writable) {
                DWORD ignored{};
                if (!VirtualProtect(page.address,pageSize,PAGE_READWRITE,&ignored)) {
                    logger::error("Could not reopen vtable page for rollback ({})",GetLastError());
                    return;
                }
                page.writable=true;
            }
            const auto i=selected[index];
            InterlockedCompareExchangePointer(Address(index),reinterpret_cast<void*>(originals[i]),
                reinterpret_cast<void*>(replacements[i])); // preserve another plugin's replacement
        }
        bool Owned(std::size_t index) const {
            return *Address(index)==reinterpret_cast<void*>(replacements[selected[index]]);
        }
    };
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    PatchBackend backend{base,*profile,activeSites,originals,replacements,{},0,system.dwPageSize};
    installed=true; // No automatic retry of a partial installation.
    const auto outcome=InstallTransaction(selectedCount,backend);
    if (!outcome.active) {
        logger::error("Hook transaction failed: rollbackComplete={} protectionsRestored={}; callbacks remain inert",
            outcome.rollbackComplete,outcome.protectionsRestored);
        return false;
    }
    registerEntries[0].wanted = spotWanted;
    registerEntries[1].wanted = sunWanted;
    registerEntries[2].wanted = settings.rainOcclusion;
    lastReport = GetTickCount64();
    enabled.store(settings.live.enabled, std::memory_order_relaxed);
    liveCadence.store(settings.live.cadence, std::memory_order_relaxed);
    liveSkipCull.store(settings.live.skipCull, std::memory_order_relaxed);
    liveWorker.store(settings.live.worker, std::memory_order_relaxed);
    liveVrRedraw.store(settings.live.vrRedraw, std::memory_order_relaxed);
    const bool vr = runtime == Runtime::VR;
    const auto why = [vr](bool desktop, bool hooked) {
        return desktop == vr ? (vr ? "(desktop only)" : "(Fallout 4 VR only)") : hooked ? "" : "(takes effect the next time the game starts)";
    };
    menuSwitches = {{
        {L"General", L"bEnabled", &enabled, "Faster Shadows", ""},
        {L"SunShadows", L"bCascadeCadence", &liveCadence, "staggered cascades", why(true, cadenceWanted)},
        {L"SunShadows", L"bCadenceSkipCull", &liveSkipCull, "cull skipping of kept cascades", why(true, cadenceWanted && settings.cadenceSkipCull)},
        {L"SunShadows", L"bCullOnWorkerThread", &liveWorker, "the sun cull worker thread", why(true, settings.sunCullWorker)},
        {L"VR", L"bCascadeRedraw", &liveVrRedraw, "VR cascade redraw", why(false, vrMaskWanted)},
    }};
    for (auto& entry : menuSwitches) entry.saved = menu::Saved(entry.section, entry.key);
    if (!menu::Watch()) logger::warn("Mod Configuration Menu: no watcher thread; its changes apply at the next start");
    logger::info("Switches: Faster Shadows {}, staggered cascades {}, cull skipping {}, worker thread {}, VR cascade redraw {}",
        settings.live.enabled, settings.live.cadence, settings.live.skipCull, settings.live.worker, settings.live.vrRedraw);
    hooksReady.store(true, std::memory_order_release);
    TryInstallRegisterHooks();
    // Call-site hooks share one trampoline (14 bytes each); a second allocation would release the first.
    const bool cullSkipWanted = cadenceWanted && settings.cadenceSkipCull;
    const auto* cullSites = cullskip::For(runtime);
    const auto* workerSites = sunworker::For(runtime);
    const auto* glareSite = sunglare::For(runtime);
    const auto* cacheSites = shadowcache::For(runtime);
    const auto* guardSite = slotguard::For(runtime);
    const bool cacheWanted = cadenceWanted && settings.holdFitFrames && (settings.optimizations & ShadowCache);
    std::size_t trampolineBytes = cullSkipWanted && cullSites ? cullskip::hookCount*14 : 0;
    if (settings.sunCullWorker && workerSites) trampolineBytes += sunworker::hookCount*14;
    if (settings.sunGlareNoFlush && glareSite) trampolineBytes += 14;
    if (cacheWanted && cacheSites) trampolineBytes += shadowcache::hookCount*14;
    if (cadenceWanted && guardSite) trampolineBytes += 14;
    const bool vrSunWanted = runtime == Runtime::VR && updateWanted;
    if (vrSunWanted) trampolineBytes += vrsun::calls.size()*14;
    if (vrMaskWanted) trampolineBytes += 14;
    if (cbro && updateWanted) trampolineBytes += cbropublish::stubBytes+14;
#if defined(FPS_ENABLE_PRIVATE_BENCHMARK)
    if (runtime == Runtime::OG) trampolineBytes += 26*14;  // light-cost and frame-cost timers
#endif
    if (trampolineBytes) F4SE::AllocTrampoline(trampolineBytes+64);
    if (cullSkipWanted) {
        if (cullSites) cullskip::Install(base, *cullSites);
        else logger::info("Cascade cull skipping has no verified sites for this runtime; reused cascades are still culled");
    }
    if (settings.sunCullWorker) {
        if (workerSites) sunworker::Install(base, *workerSites);
        else logger::info("Sun cull worker has no verified sites for this runtime; the main thread keeps gathering sun casters");
    }
    if (cadenceWanted) {
        if (guardSite) slotguard::Install(base, *guardSite);
        else logger::info("Shadow slice guard has no verified site for this runtime; only the far cascade is kept");
    }
    if (cacheWanted) {
        if (cacheSites) shadowcache::Install(base, *cacheSites);
        else logger::info("Static shadow cache has no verified sites for this runtime");
    }
    if (settings.sunGlareNoFlush) {
        if (glareSite) sunglare::Install(base, *glareSite);
        else logger::info("Sun-glare poll change has no verified site for this runtime");
    }
    if (cbro && updateWanted && cbropublish::Install(base))
        logger::info("Sun state published from the cascade cull's first call; CBRO keeps the light's update slot");
    if (vrSunWanted && vrsun::Install(base))
        logger::info("VR sun state published from the cascade gather");
    if (vrMaskWanted && vrmask::Install(base))
        logger::info("VR cascade redraw installed: a cascade the head moved {} units or turned {:.1f} degrees away from joins the next refit",
            settings.vr.maxMove, std::acos(std::clamp(settings.vr.minTurnCosine, -1.0f, 1.0f)) / radians);
#if defined(FPS_ENABLE_PRIVATE_BENCHMARK)
    lightcost::Install(base);
#endif
    logger::info("Installed Fallout shadow rules (DryRun={}; sun={}; point={}; spot={}; actors={}; rain={}; vtable hooks={}); registration filters install once the renderer has started",
        settings.dryRun, settings.sun.enabled, settings.point.enabled, spotWanted, settings.actorShadows, settings.rainOcclusion, selectedCount);
    if (cadenceWanted && settings.cadence.policy == 1)
        logger::info("Staggered sun cascades by the rules of Fallout 4 Community Shaders (comparison): middle every {} frames, far every {} frames; "
            "refresh when a kept map no longer covers the view, after a {}-unit jump or a 0.25-degree sun turn",
            settings.cadence.midInterval, settings.cadence.farInterval, StaggeredCadence::jumpUnits);
    else if (cadenceWanted)
        logger::info("Staggered sun cascades: middle every {} frames, far every {} frames, up to {} waiting per frame; refresh after {} units or {:.1f} degrees, "
            "or {}", settings.cadence.midInterval, settings.cadence.farInterval, settings.cadence.maxWaiting, settings.cadence.maxMove,
            std::acos(std::clamp(settings.cadence.minTurnCosine, -1.0f, 1.0f)) / radians,
            settings.cadence.minSunCosine >= 1.0f ? std::string("on any sun change") :
                fmt::format("on a sun turn over {:.2f} degrees", std::acos(std::clamp(settings.cadence.minSunCosine, -1.0f, 1.0f)) / radians));
    logger::info("Faster Shadows optimization mask={} (volume=1 squared=2 snapshot=4 counters=8 memo=16 sunturn=32 emptycascades=64 holdfit=128); "
        "hold fit frames={}",settings.optimizations,settings.holdFitFrames);
    return true;
}
void RetryRegisterHooks() {
    if (!hooksReady.load(std::memory_order_acquire)) return;
    std::scoped_lock lock(pollMutex);
    TryInstallRegisterHooks();
}
void ResetShadows() {
    std::scoped_lock lock(cadenceLock);
    vrmask::resetRequested.store(true, std::memory_order_release);
    ResetCadence();
    actorrule::Clear();  // root nodes of unloaded actors may be reused
    sunFrame.store(nullptr, std::memory_order_release);
    frameEpoch.fetch_add(1, std::memory_order_release);
}
#if defined(FPS_ENABLE_PRIVATE_BENCHMARK)
// Private lab build only: time Fallout 4's per-frame light assignment, so a
// light-assignment throttle (SPS LightGather) is judged on measurements. OG only.
namespace lightcost {
using ProcessAll = std::uint64_t (*)(void*);
using ObjectLighting = std::uint64_t (*)(void*, void*, void*, void*, bool, bool);
ProcessAll originalProcessAll{};
ObjectLighting originalObjectLighting{};
std::atomic<std::uint64_t> frames{0}, processTicks{0}, objectCalls{0}, objectTicks{0};
std::uint64_t Now() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}
// Once per frame: queued light adds/removals and the three light-list walks,
// including dynamic-light updates (DrawWorld::LightUpdate, with or without previs).
std::uint64_t ProcessAllQueuedLights(void* culler) {
    const auto begin = Now();
    const auto result = originalProcessAll(culler);
    processTicks.fetch_add(Now()-begin, std::memory_order_relaxed);
    frames.fetch_add(1, std::memory_order_relaxed);
    return result;
}
// Per visible fade node with queued lighting: its light list against every light.
std::uint64_t ObjectLightingStart(void* node, void* scene, void* room, void* portal, bool first, bool second) {
    const auto begin = Now();
    const auto result = originalObjectLighting(node, scene, room, portal, first, second);
    objectTicks.fetch_add(Now()-begin, std::memory_order_relaxed);
    objectCalls.fetch_add(1, std::memory_order_relaxed);
    return result;
}
struct Site { std::uint32_t call, target; };
// OG 1.10.163 (Ghidra Combined, PDB names): DrawWorld::LightUpdate -> ShadowSceneNode::
// ProcessAllQueuedLights, and BSFadeNode::OnVisible -> UpdateQueuedObjectLightingStart.
constexpr Site processSite{0x284DDED, 0x28104D0}, objectSite{0x27D12CF, 0x2810DD0};
bool IsCall(std::uintptr_t base, Site site) noexcept {
    const auto* code = reinterpret_cast<const std::uint8_t*>(base+site.call);
    std::int32_t relative{};
    std::memcpy(&relative, code+1, sizeof(relative));
    return code[0] == 0xE8 && base+site.call+5+relative == base+site.target;
}
void Install(std::uintptr_t base) {
    if (REL::Module::get().version() != REL::Version(1, 10, 163, 0)) {
        logger::info("Light-cost timers are OG-only; not installed");
        return;
    }
    if (!IsCall(base, processSite) || !IsCall(base, objectSite)) {
        logger::warn("Light-cost timer call sites differ from the verified engine; not installed");
    } else {
        // Originals first: a running render thread may reach a rewritten call immediately.
        originalProcessAll = reinterpret_cast<ProcessAll>(base+processSite.target);
        originalObjectLighting = reinterpret_cast<ObjectLighting>(base+objectSite.target);
        auto& trampoline = F4SE::GetTrampoline();
        trampoline.write_call<5>(base+processSite.call, &ProcessAllQueuedLights);
        trampoline.write_call<5>(base+objectSite.call, &ObjectLightingStart);
        logger::info("Light-cost timers installed");
    }
    framecost::Install(base);
}
// Logged at every counter read: a reset closes the warm-up interval, a sample the measured window.
void Report(bool reset) {
    if (!originalProcessAll) return;
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const auto sample = [reset](std::atomic<std::uint64_t>& counter) {
        return reset ? counter.exchange(0, std::memory_order_relaxed) : counter.load(std::memory_order_relaxed);
    };
    const auto count = sample(frames);
    const double perFrame = 1000.0/static_cast<double>(frequency.QuadPart)/static_cast<double>(std::max<std::uint64_t>(count, 1));
    const auto process = sample(processTicks), calls = sample(objectCalls), object = sample(objectTicks);
    logger::info("[LightCost] {} frames={} lightLists={:.4f} ms/frame objectLighting={:.4f} ms/frame objectCalls/frame={:.1f}",
        reset ? "interval" : "window", count, static_cast<double>(process)*perFrame, static_cast<double>(object)*perFrame,
        static_cast<double>(calls)/static_cast<double>(std::max<std::uint64_t>(count, 1)));
}
}
#endif
#if defined(FPS_ENABLE_PRIVATE_BENCHMARK)
// Private lab build only: timing while the camera moves. Once per frame, on the main thread just
// before Main::Swap (the frame's job stages already waited), the player turns or strafes along a
// triangle wave that starts at the request, so every window follows the same path: kind 1 turns
// `rate` degrees per second up to `amplitude` degrees either side of the start heading, kind 2
// moves along world x at `rate` units per second up to `amplitude` units either side, kind 3
// walks forward and back along the start heading the same way. Kind 0 stops and puts the player
// back exactly where the motion began.
namespace motion {
struct Request { std::uint32_t kind{}; float rate{}, amplitude{}; };
std::mutex requestLock;
std::optional<Request> pending;  // guarded by requestLock
std::uint32_t kind{};            // main thread only, like the rest of this state
float rate{}, amplitude{}, heading{};
RE::NiPoint3 origin{};
std::chrono::steady_clock::time_point start{};
bool Set(std::uint32_t requested, float speed, float range) noexcept {
    if (requested > 3 || (requested && !(speed > 0 && range > 0 && std::isfinite(speed) && std::isfinite(range)))) return false;
    std::scoped_lock lock(requestLock);
    pending = Request{requested, speed, range};
    return true;
}
void Apply() noexcept {
    std::optional<Request> request;
    {
        std::scoped_lock lock(requestLock);
        request.swap(pending);
    }
    if (!request && !kind) return;
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;
    if (request) {
        if (kind == 1) player->SetHeading(heading);  // a new request first ends the current motion
        if (kind == 2 || kind == 3) player->SetPosition(origin, true);
        kind = request->kind;rate = request->rate;amplitude = request->amplitude;
        heading = player->data.angle.z;
        origin = {player->data.location.x, player->data.location.y, player->data.location.z};
        start = std::chrono::steady_clock::now();
        if (!kind) return;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double phase = std::fmod(seconds * rate / amplitude, 4.0);  // 0 to 1 up, 1 to 3 down, 3 to 4 back up
    const auto offset = static_cast<float>(amplitude * (phase < 1 ? phase : phase < 3 ? 2 - phase : phase - 4));
    if (kind == 1) player->SetHeading(heading + offset * radians);
    else if (kind == 2) player->SetPosition({origin.x + offset, origin.y, origin.z}, true);
    else player->SetPosition({origin.x + offset * std::sin(heading), origin.y + offset * std::cos(heading), origin.z}, true);
}
}
// Private lab build only: where the main thread's frame goes, so an optimization
// targets what actually paces it. Each site is an E8 call in OG 1.10.163 (Ghidra
// Combined, PDB names) whose callee takes at most four integer or pointer arguments
// and none in floating-point registers; the timer forwards them unchanged.
namespace framecost {
using Call = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
struct Site { const char* name; std::uint32_t call, target; };
constexpr std::array<Site, 24> sites{{
    // Main::OnIdle: two waits for the frame's job threads, the render/present, the final job wait.
    {"stageWait3", 0xD394FD, 0x1B63210}, {"stageWait0", 0xD39506, 0x1B63210},
    {"multiBoundCulling", 0xD3950E, 0xD39A60}, {"swap", 0xD3954C, 0xD38700}, {"jobWait", 0xD3955B, 0xCBCFA0},
    // Main::Swap
    {"highActorCuller", 0xD388CC, 0xE93100}, {"preRender", 0xD388F7, 0xD3D590},
    {"drawWorldAndUI", 0xD38901, 0xD3CBE0}, {"present", 0xD38AB4, 0x1D0B670},
    // Main::DrawWorld_And_UI; the probe already times its Render_PreUI call.
    {"uiStageWait", 0xD3CD26, 0x1B63210}, {"imagespace", 0xD3CD43, 0x2857700}, {"menus", 0xD3CDE9, 0x2043A80},
    // DrawWorld::Render_PreUI stages
    {"umbraQuery", 0x2857500, 0x284F1D0}, {"lightUpdate", 0x28575E5, 0x284DDD0}, {"mainAccum", 0x28575EA, 0x284F350},
    {"mainRenderSetup", 0x28575F5, 0x28503F0}, {"prePass", 0x28575FF, 0x2850CB0}, {"decals", 0x2857609, 0x2852590},
    {"sao", 0x285760E, 0x2851540}, {"deferredLights", 0x285763F, 0x28529B0}, {"composite", 0x2857644, 0x2855E60},
    {"forward", 0x2857649, 0x2856B70}, {"refraction", 0x285764E, 0x2856FA0},
    // DrawWorld::DeferredLightsImpl: the unbatched sun cascade cull (BSShadowDirectionalLight::AccumulateFromLists)
    {"sunCull", 0x28537F9, 0x28CA7D0},
}};
constexpr std::size_t Index(std::string_view name) {
    for (std::size_t i = 0; i < sites.size(); ++i) if (sites[i].name == name) return i;
    return sites.size();
}
constexpr std::size_t swapIndex = Index("swap"), mainAccumIndex = Index("mainAccum");
constexpr std::size_t sunRenderIndex = sites.size(), loopIndex = sites.size()+1;
static_assert(swapIndex < sites.size() && mainAccumIndex < sites.size());
struct alignas(64) Totals { std::atomic<std::uint64_t> ticks{0}, calls{0}; };
std::array<Totals, sites.size()+2> totals;  // the sites, the sun cascade render, the main-loop interval
std::array<Call, sites.size()> originals{};
std::uint64_t lastSwap = 0;  // main thread only
bool timersInstalled = false;
// FO4's own batched culling (CullingJob, INI bCullingBatch:General, off by default) runs the
// main-view cull, and without previs the sun cascade cull, as jobs. A private lab run sets
// FPS_BENCH_CULLING_BATCH=1 so the benchmark Enabled switch also toggles it. The flag is
// written on the main thread just before DrawWorld::MainAccum, its first reader in a frame,
// so one frame never mixes the two paths. CullingJob::InitSDM always creates the batcher.
constexpr std::uint32_t batchGetter = 0x1CCB850, batchFlag = 0x384E778;  // QBatchEnabled, the setting's value
std::atomic<int> pendingBatch{-1};
std::uint8_t* batchByte = nullptr;
void RequestCullingBatch(bool on) noexcept {
    if (batchByte) pendingBatch.store(on ? 1 : 0, std::memory_order_release);
}
void Add(std::size_t index, std::uint64_t ticks) noexcept {
    totals[index].ticks.fetch_add(ticks, std::memory_order_relaxed);
    totals[index].calls.fetch_add(1, std::memory_order_relaxed);
}
template<std::size_t I> std::uint64_t Timed(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) {
    if constexpr (I == swapIndex) motion::Apply();  // outside the timed call
    if constexpr (I == mainAccumIndex) {
        if (const auto value = pendingBatch.exchange(-1, std::memory_order_acq_rel); value >= 0 && batchByte)
            *batchByte = static_cast<std::uint8_t>(value);
    }
    const auto begin = lightcost::Now();
    if constexpr (I == swapIndex) {
        if (lastSwap) Add(loopIndex, begin-lastSwap);
        lastSwap = begin;
    }
    const auto result = originals[I](a, b, c, d);
    Add(I, lightcost::Now()-begin);
    return result;
}
template<std::size_t... I> constexpr std::array<Call, sizeof...(I)> Wrappers(std::index_sequence<I...>) { return {&Timed<I>...}; }
void SunRendered(std::uint64_t ticks) noexcept { if (timersInstalled) Add(sunRenderIndex, ticks); }
bool Ready() noexcept { return originals[swapIndex] != nullptr; }  // motion runs from the swap timer
void Install(std::uintptr_t base) {
    constexpr auto wrappers = Wrappers(std::make_index_sequence<sites.size()>{});
    auto& trampoline = F4SE::GetTrampoline();
    std::size_t count = 0;
    std::string differing;
    // CBRO times and wraps these Render_PreUI stages and the cascade cull itself, and checks them before it trims.
    constexpr std::array<std::uint32_t, 6> cbroSites{0x28575EA, 0x28575F5, 0x28575FF, 0x285763F, 0x2857649, 0x28537F9};
    for (std::size_t i = 0; i < sites.size(); ++i) {
        if (cbro && std::ranges::find(cbroSites, sites[i].call) != cbroSites.end()) {
            differing += std::string(" ") + sites[i].name + "(CBRO)";
            continue;
        }
        if (!lightcost::IsCall(base, {sites[i].call, sites[i].target})) {
            differing += std::string(" ") + sites[i].name;
            continue;
        }
        originals[i] = reinterpret_cast<Call>(base+sites[i].target);  // valid before the call site points at us
        trampoline.write_call<5>(base+sites[i].call, wrappers[i]);
        ++count;
    }
    timersInstalled = count > 0;
    char flag[4]{};
    if (GetEnvironmentVariableA("FPS_BENCH_CULLING_BATCH", flag, sizeof(flag)) == 1 && flag[0] == '1') {
        const auto* code = reinterpret_cast<const std::uint8_t*>(base+batchGetter);
        std::int32_t relative{};
        std::memcpy(&relative, code+3, sizeof(relative));
        // movzx eax, byte ptr [rip+relative]; ret
        const bool verified = code[0] == 0x0F && code[1] == 0xB6 && code[2] == 0x05 && code[7] == 0xC3 &&
                              base+batchGetter+7+relative == base+batchFlag;
        if (verified && originals[mainAccumIndex]) batchByte = reinterpret_cast<std::uint8_t*>(base+batchFlag);
        logger::info("Culling-batch benchmark toggle {}", batchByte ? "armed" : "refused: engine differs");
    }
    logger::info("Frame-cost timers installed at {}/{} sites{}{}", count, sites.size(),
        differing.empty() ? "" : "; differing:", differing);
}
// Milliseconds per main-loop frame (Main::Swap calls), logged at every counter read like [LightCost].
void Report(bool reset) {
    if (!timersInstalled) return;
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const auto take = [reset](std::atomic<std::uint64_t>& value) {
        return reset ? value.exchange(0, std::memory_order_relaxed) : value.load(std::memory_order_relaxed);
    };
    const auto frames = take(totals[swapIndex].calls);
    const double perFrame = 1000.0/static_cast<double>(frequency.QuadPart)/static_cast<double>(std::max<std::uint64_t>(frames, 1));
    std::string text;
    char field[64];
    for (std::size_t i = 0; i < totals.size(); ++i) {
        const auto ticks = take(totals[i].ticks);
        if (i != swapIndex) take(totals[i].calls);
        if (i < sites.size() && !originals[i]) continue;
        const char* name = i < sites.size() ? sites[i].name : i == sunRenderIndex ? "sunRender" : "loop";
        std::snprintf(field, sizeof(field), " %s=%.4f", name, static_cast<double>(ticks)*perFrame);
        text += field;
    }
    if (batchByte) text += batchByte[0] ? " cullingBatch=1" : " cullingBatch=0";
    logger::info("[FrameCost] {} frames={}{}", reset ? "interval" : "window", frames, text);
}
}
#endif
#if defined(FPS_ENABLE_PRIVATE_BENCHMARK)
// Private lab build only. The shipped DLL omits these diagnostic exports. Controls
// execute between measured windows, via F4SE's game-thread task interface.
extern "C" __declspec(dllexport) std::uint32_t FPSBenchmarkProtocolVersion() { return 4; }
extern "C" __declspec(dllexport) bool FPSBenchmarkSetOptimizations(std::uint32_t mask) {
    if (!hooksReady.load(std::memory_order_acquire) || mask & ~AllOptimizations) return false;
    optimizationMask.store(mask,std::memory_order_relaxed);
    return true;
}
struct OptimizationCounters { std::uint64_t volume,memoHits,memoMisses,removed;std::uint32_t mask,enabled,dryRun,volumeAnalysisOnly; };
extern "C" __declspec(dllexport) bool FPSBenchmarkReadOptimizations(OptimizationCounters* out,bool reset) {
    if (!out || !hooksReady.load(std::memory_order_acquire)) return false;
    const auto sample=[reset](Counter counter) { return diagnostics.Sample(counter,reset); };
    *out={sample(volumeCandidates),sample(memoHits),sample(memoMisses),sample(pointRemoved),
        optimizationMask.load(std::memory_order_relaxed),enabled.load(std::memory_order_relaxed),settings.dryRun,1};
    return true;
}
// kind 0 stops (and restores the pose), 1 turns, 2 strafes, 3 walks; see motion above.
extern "C" __declspec(dllexport) bool FPSBenchmarkSetMotion(std::uint32_t kind, float rate, float amplitude) {
    if (!hooksReady.load(std::memory_order_acquire) || !framecost::Ready() || !motion::Set(kind, rate, amplitude)) return false;
    logger::info("Private benchmark: camera motion {} (rate {}, amplitude {})", kind, rate, amplitude);
    return true;
}
extern "C" __declspec(dllexport) bool FPSBenchmarkSetEnabled(bool active) {
    if (!hooksReady.load(std::memory_order_acquire)) return false;
    enabled.store(active, std::memory_order_relaxed);
    framecost::RequestCullingBatch(active);
    logger::info("Private benchmark: shadow rules {}", active ? "ON" : "OFF");
    return true;
}
// Protocol 4 layout. "point" also counts spot maps; "other" counts sun-mode
// registrations from accumulators that are not this frame's cascades.
struct BenchmarkCounters { std::uint64_t sunTested,sunCandidates,pointTested,pointCandidates,other; };
extern "C" __declspec(dllexport) bool FPSBenchmarkReadCounters(BenchmarkCounters* out, bool reset) {
    if (!out || !hooksReady.load(std::memory_order_acquire)) return false;
    const auto sample = [reset](Counter counter) { return diagnostics.Sample(counter,reset); };
    *out = {sample(sunTested),sample(sunCandidates),sample(pointTested)+sample(spotTested),
        sample(pointCandidates)+sample(spotCandidates),sample(otherCalls)};
    logger::info("[Census] {} actor candidates={} rain tested={} candidates={} removed={} cadence mid={} far={} dropped={} leaks={} slotConflicts={} sunBlending={} cullsSkipped={} "
        "cache built={} reused={} refused={} lampRedraws={} empty={}",
        reset ? "interval" : "window", sample(actorCandidates), sample(rainTested), sample(rainCandidates), sample(removedTotal),
        sample(cadenceMid), sample(cadenceFar), sample(cadenceDropped), sample(cadenceLeaks), sample(slotConflicts), sample(cadenceSunBlends), sample(cadenceCullsSkipped),
        sample(cacheBuilt), sample(cacheReused), sample(cacheRefused), sample(cadenceLampRedraws), sample(cadenceEmpty));
    lightcost::Report(reset);
    framecost::Report(reset);
    return true;
}
// Protocol 4 layout: "cullers" now carry the cascade accumulators, "vtables" their
// vtable and "append" the sun registration entry. hookMask bits 0-2 are the vtable
// sites (sun update, point begin, point append), bits 3-5 the spot/sun/rain entries,
// bit 6 the sun Render site (staggered cascades).
extern "C" __declspec(dllexport) bool FPSBenchmarkReadCadence(std::uint32_t bytes, BenchmarkCadence* out) {
    if (!out || bytes != sizeof(*out) || !hooksReady.load(std::memory_order_acquire)) return false;
    *out = {};
    out->epoch = frameEpoch.load(std::memory_order_acquire);
    out->enabled = enabled.load(std::memory_order_relaxed);
    if (const auto frame = sunFrame.load(std::memory_order_acquire)) {
        out->cadence = frame->cadence;
        out->count = frame->count;
        out->plannedSkipMask = frame->skip;
    }
    return true;
}
struct BenchmarkSunState {
    std::uint64_t epoch,present,count,hookMask;
    float eye[3],direction[3];
    std::uint64_t cullers[4],cameras[4],vtables[4],append[4];
};
extern "C" __declspec(dllexport) bool FPSBenchmarkReadSunState(BenchmarkSunState* out) {
    if(!out || !hooksReady.load(std::memory_order_acquire) || !activeProfile)return false;
    *out={};out->epoch=frameEpoch.load(std::memory_order_acquire);
    const auto* profile=activeProfile;
    const std::array<std::uintptr_t,4> ours{reinterpret_cast<std::uintptr_t>(&SunUpdate),
        reinterpret_cast<std::uintptr_t>(&PointBegin),reinterpret_cast<std::uintptr_t>(&PointAppend),
        reinterpret_cast<std::uintptr_t>(&SunRender)};
    for(std::size_t i=0;i<profile->hooks.size();++i) {
        const auto& site=profile->hooks[i];
        // Bits 3-5 were already the registration entries: the sun Render site is bit 6.
        if(Read<std::uintptr_t>(reinterpret_cast<void*>(imageBase+site.table),site.slot*8)==ours[i])out->hookMask|=1ULL<<(i==3?6:i);
    }
    if(cbropublish::installed)out->hookMask|=1;  // the sun state, published from the cascade cull next to CBRO
    for(std::size_t i=0;i<registerEntries.size();++i)
        if(*RegisterSlot(registerEntries[i])==reinterpret_cast<void*>(registerEntries[i].replacement))out->hookMask|=1ULL<<(3+i);
    if(const auto frame=sunFrame.load(std::memory_order_acquire)) {
        out->present=1;out->count=frame->count;
        out->eye[0]=frame->eye.x;out->eye[1]=frame->eye.y;out->eye[2]=frame->eye.z;
        out->direction[0]=frame->direction.x;out->direction[1]=frame->direction.y;out->direction[2]=frame->direction.z;
        for(std::size_t i=0;i<frame->count;++i) {
            out->cullers[i]=reinterpret_cast<std::uintptr_t>(frame->accumulators[i]);
            out->cameras[i]=reinterpret_cast<std::uintptr_t>(frame->cameras[i]);
            SIZE_T read{};
            ReadProcessMemory(GetCurrentProcess(),frame->accumulators[i],&out->vtables[i],8,&read);
            out->append[i]=reinterpret_cast<std::uintptr_t>(*RegisterSlot(registerEntries[1]));
        }
    }
    return true;
}
#endif
}
