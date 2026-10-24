// Adapted from dnai92/SPS 1.0.52 shadow rules (ShadowCulling.cpp: ShouldCull,
// NearPointLight, PointShadowScale, CullActorShadow; SkylightingCulling).
// Copyright (c) 2026 dnai92; Fallout adaptation (c) 2026 Noud.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fps {
struct Vec3 { float x{}, y{}, z{}; };
struct Sphere { Vec3 center; float radius{}; };
// SPS 1.0.52 defaults: sun 1500/150/0.035 from cascade 1; point/spot 1000/150/0.035.
struct Rule {
    bool enabled{true};
    float minDistance{1500.0f};
    float maxRadius{150.0f};
    float minAngularSize{0.035f};
    std::uint32_t minCascade{1};
};
// A caster this large relative to its distance from the lamp casts a large,
// clearly visible shadow even when it is small itself (SPS NearPointLight).
inline constexpr float kNearLightAngular = 0.1f;
// Upper bound of the lamp shadow magnification (SPS PointShadowScale).
inline constexpr float kMaxLampShadowScale = 50.0f;
// Actor shadows are never removed closer than ~30 m (SPS ActorShadowCulling).
inline constexpr float kMinActorShadowDistance = 2100.0f;

inline bool Finite(Vec3 p) noexcept {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
inline float Distance(Vec3 a, Vec3 b) noexcept {
    return std::hypot(a.x-b.x, a.y-b.y, a.z-b.z);
}
inline float SunSine(Vec3 direction) noexcept {
    if (!Finite(direction)) return 0.0f;
    const float length = std::hypot(direction.x, direction.y, direction.z);
    return length > 0.001f && std::isfinite(length) ? std::abs(direction.z)/length : 0.0f;
}
// UpdateCamerasI uses the normalized blend while +0x200 remains its target.
// A settled target is already the native direction; preserve it exactly.
inline Vec3 BlendedSunDirection(Vec3 target, Vec3 previous, float progress) noexcept {
    if (!Finite(target) || !Finite(previous) || !std::isfinite(progress) ||
        progress < 0.0f || progress > 1.0f) return {};
    const Vec3 blended = progress == 1.0f ? target : Vec3{
        previous.x + progress*(target.x-previous.x),
        previous.y + progress*(target.y-previous.y),
        previous.z + progress*(target.z-previous.z)};
    if (!Finite(blended)) return {};
    const float length = std::hypot(blended.x, blended.y, blended.z);
    if (!std::isfinite(length) || length <= 0.001f) return {};
    return progress == 1.0f ? target : Vec3{blended.x/length, blended.y/length, blended.z/length};
}
inline constexpr bool ShouldEvaluateCasterPolicies(bool dryRun, bool diagnostics) noexcept {
    return !dryRun || diagnostics;
}
inline constexpr bool LampCellAllowed(bool knownCell, bool interior, bool allowInteriors) noexcept {
    return knownCell && (allowInteriors || !interior);
}
inline bool CullSmall(const Rule& rule, Sphere object, Vec3 eye, float radiusScale=1.0f) noexcept {
    if (!rule.enabled || !Finite(object.center) || !Finite(eye) ||
        !std::isfinite(object.radius) || object.radius <= 0.0f ||
        !std::isfinite(radiusScale) || radiusScale < 1.0f) return false;
    const float radius = object.radius*radiusScale;
    if (!std::isfinite(radius) || radius >= rule.maxRadius) return false;
    const float nearest = Distance(object.center, eye)-object.radius;
    return std::isfinite(nearest) && nearest > rule.minDistance &&
           radius/nearest < rule.minAngularSize;
}
inline bool CullSun(const Rule& rule, Sphere object, Vec3 eye, Vec3 direction,
                    std::uint32_t cascade, float minElevationDegrees) noexcept {
    if (cascade < rule.minCascade) return false;
    const float sine = SunSine(direction);
    constexpr float radians = 0.0174532925199433f;
    const float cutoff = std::sin(minElevationDegrees*radians);
    return sine > 0.05f && sine >= cutoff && CullSmall(rule, object, eye, 1.0f/sine);
}
// Shared by all casters in one sun update, including the actor elevation gate.
struct PreparedSunRule {
    Rule rule{};
    float radiusScale{1.0f};
    bool elevationAllowed{}, valid{};
    PreparedSunRule() noexcept = default;
    PreparedSunRule(Rule r, Vec3 direction, float minElevationDegrees) noexcept : rule(r) {
        const float sine = SunSine(direction);
        constexpr float radians = 0.0174532925199433f;
        const float cutoff = std::sin(minElevationDegrees*radians);
        elevationAllowed = sine > 0.05f && sine >= cutoff;
        valid = rule.enabled && elevationAllowed;
        if (elevationAllowed) radiusScale = 1.0f/sine;
    }
};
inline bool CullSunPrepared(const PreparedSunRule& prepared, Sphere object, Vec3 eye,
                            std::uint32_t cascade) noexcept {
    if (!prepared.valid || cascade < prepared.rule.minCascade ||
        object.radius >= prepared.rule.maxRadius) return false;
    return CullSmall(prepared.rule, object, eye, prepared.radiusScale);
}
inline bool NearLight(Sphere object, Vec3 light) noexcept {
    const float nearest = Distance(object.center, light)-object.radius;
    return !std::isfinite(nearest) || nearest <= 1.0f || object.radius/nearest >= kNearLightAngular;
}
// A shadow on a surface behind the caster is magnified by (light-surface /
// light-caster) distance, and the surface lies within the light's reach.
// An unknown reach keeps the unscaled rule, as SPS does for unlisted cameras.
inline float LampShadowScale(Sphere object, Vec3 light, float reach) noexcept {
    if (!std::isfinite(reach) || reach <= 0.0f) return 1.0f;
    const float nearest = std::max(Distance(object.center, light)-object.radius, 1.0f);
    const float scale = reach/nearest;
    return std::isfinite(scale) ? std::clamp(scale, 1.0f, kMaxLampShadowScale) : kMaxLampShadowScale;
}
inline bool CullPoint(const Rule& rule, Sphere object, Vec3 eye, Vec3 light, float reach=0.0f) noexcept {
    if (!rule.enabled || !Finite(light) || !Finite(object.center) || NearLight(object, light)) return false;
    return CullSmall(rule, object, eye, LampShadowScale(object, light, reach));
}
// The caller establishes skinned geometry below an actor reference root.
inline bool CullActorShadow(bool enabled, float minDistance, Sphere object, Vec3 eye) noexcept {
    if (!enabled || !Finite(object.center) || !Finite(eye) || !std::isfinite(object.radius)) return false;
    const float limit = std::isfinite(minDistance) ? std::max(minDistance, kMinActorShadowDistance) : kMinActorShadowDistance;
    const float nearest = Distance(object.center, eye)-object.radius;
    return std::isfinite(nearest) && nearest > limit;
}
// SPS 1.0.60 actor rule: one decision per actor, by its position, with hysteresis. A shadow
// that went off beyond the limit returns only within kActorShadowReturn of it, so an NPC near
// the limit does not flicker and its body parts never switch separately.
inline constexpr float kActorShadowReturn = 0.9f;
inline bool ActorShadowCulled(bool wasCulled, float minDistance, Vec3 actor, Vec3 eye) noexcept {
    if (!Finite(actor) || !Finite(eye)) return false;
    const float limit = std::isfinite(minDistance) ? std::max(minDistance, kMinActorShadowDistance) : kMinActorShadowDistance;
    const float distance = Distance(actor, eye);
    return std::isfinite(distance) && distance > (wasCulled ? kActorShadowReturn * limit : limit);
}
// Small objects are left out of the top-down rain occlusion map (SPS SkylightingCulling).
inline bool CullRainOccluder(bool enabled, float minRadius, float radius) noexcept {
    return enabled && std::isfinite(minRadius) && std::isfinite(radius) && radius > 0.0f && radius < minRadius;
}
} // namespace fps
