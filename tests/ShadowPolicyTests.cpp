#include "ShadowPolicy.h"
#include "EngineProfiles.h"
#include "HookGate.h"
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
namespace {
int checks = 0;
void Check(bool condition, const char* name) {
    ++checks;
    if (!condition) { std::cerr << "FAILED: " << name << '\n'; std::exit(1); }
}
}
int main() {
    using namespace fps;
    // SPS 1.0.52 shipped rules.
    Rule sun{true, 1500, 150, 0.035f, 1};
    Rule point{true, 1000, 150, 0.035f, 0};
    const Vec3 eye{0,0,0}, overhead{0,0,-1};
    const Sphere small{{4000,0,0}, 10};
    Check(CullSun(sun, small, eye, overhead, 1, 10), "far small caster");
    Check(!CullSun(sun, small, eye, overhead, 0, 10), "nearest cascade preserved");
    Check(!CullSun(sun, small, eye, {1,0,-0.1f}, 1, 10), "sun below 10 degrees preserved");
    Check(CullSun(sun, {{4000,0,0},70}, eye, overhead, 1, 10), "overhead sun removes a mid-size far caster");
    Check(!CullSun(sun, {{4000,0,0},70}, eye, {1,0,-0.5f}, 1, 10), "low sun lengthens the shadow and keeps it");
    Check(!CullSmall(sun, {{1500,0,0},10}, eye), "nearest sphere surface protects nearby object");
    Check(!CullSmall(sun, {{4000,0,0},150}, eye), "large radius protected at threshold");
    Check(!CullSmall(sun, {{4000,0,0},145}, eye), "large angular caster protected");
    Check(CullSmall(sun, {{4000,0,0},100}, eye), "SPS angular cutoff removes a far 100-unit caster");
    Check(!CullSmall(sun, small, eye, 20), "long shadow protected");
    Check(!CullPoint(point, small, eye, {4010,0,0}), "lamp inside bound protected");
    Check(!CullPoint(point, small, eye, {4050,0,0}), "caster near lamp protected");
    Check(!CullPoint(point, small, eye, {4100,0,0}), "near-light exemption at radius/distance 0.1");
    Check(CullPoint(point, small, eye, {4500,0,0}), "far small point caster");
    Check(!CullPoint(point, {{1005,0,0},10}, eye, {3000,0,0}), "point minimum distance counts from the sphere surface");
    Check(!CullPoint(point, small, eye, {4500,0,0}, 8000), "lamp reach magnifies a wall shadow and keeps it");
    Check(CullPoint(point, small, eye, {4500,0,0}, 980), "short reach keeps the removal");
    Check(LampShadowScale(small, {4500,0,0}, 1e9f) == kMaxLampShadowScale, "lamp scale is clamped");
    Check(LampShadowScale(small, {4500,0,0}, 0) == 1.0f, "unknown reach keeps the unscaled rule");
    Check(LampShadowScale(small, {4500,0,0}, 100) == 1.0f, "reach inside the lamp distance never shrinks a shadow");
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto inf = std::numeric_limits<float>::infinity();
    Check(!CullSmall(sun, {{nan,0,0},10}, eye), "invalid center protected");
    Check(!CullSmall(sun, {{4000,0,0},nan}, eye), "invalid radius protected");
    Check(!CullSmall(sun, {{4000,0,0},0}, eye), "zero bound protected");
    Check(!CullSmall(sun, small, {inf,0,0}), "invalid eye protected");
    Check(!CullSmall(sun, small, eye, inf), "invalid shadow scale protected");
    Check(!CullPoint(point, small, eye, {nan,0,0}), "invalid lamp protected");
    Check(CullPoint(point, small, eye, {4500,0,0}, nan) == CullPoint(point, small, eye, {4500,0,0}),
          "invalid reach falls back to the unscaled rule");
    Check(!CullSun(sun, small, eye, {}, 1, 10), "missing sun protected");
    {
        constexpr float radians = 0.0174532925199433f;
        const auto direction = [](float degrees) {
            const float angle = degrees*0.0174532925199433f;
            return Vec3{std::cos(angle),0,-std::sin(angle)};
        };
        const auto previous = direction(9.9f), target = direction(10.1f);
        const auto blended = BlendedSunDirection(target,previous,0.1f);
        Check(std::abs(std::hypot(blended.x,blended.y,blended.z)-1.0f)<1e-6f,
            "native sun blend is normalized");
        Check(std::abs(std::asin(SunSine(blended))/radians-9.92f)<0.001f,
            "native sun blend uses previous direction and progress");
        const PreparedSunRule rising(sun,blended,10);
        Check(CullSun(sun,small,eye,target,1,10),"target alone would prematurely remove a sunrise caster");
        Check(!rising.elevationAllowed && !CullSunPrepared(rising,small,eye,1),
            "sunrise blend below the cutoff keeps casters and actor shadows");
        const PreparedSunRule settled(sun,BlendedSunDirection(target,previous,1),10);
        Check(settled.elevationAllowed && CullSunPrepared(settled,small,eye,1),
            "settled sun resumes filtering above the cutoff");
        const auto stable = BlendedSunDirection(target,previous,1);
        Check(stable.x==target.x && stable.y==target.y && stable.z==target.z,
            "settled target retains the exact native direction");
        Check(std::abs(SunSine(BlendedSunDirection(target,previous,0))-SunSine(previous))<1e-6f,
            "zero progress uses the previous direction");
        for (float progress : {-0.01f,1.01f,nan,inf}) {
            const PreparedSunRule invalid(sun,BlendedSunDirection(target,previous,progress),10);
            Check(!invalid.elevationAllowed && !CullSunPrepared(invalid,small,eye,1),
                "invalid blend progress fails open");
        }
        for (Vec3 invalid : {Vec3{nan,0,0},Vec3{0,inf,0}}) {
            Check(!PreparedSunRule(sun,BlendedSunDirection(invalid,previous,0.5f),10).elevationAllowed,
                "nonfinite target fails open");
            Check(!PreparedSunRule(sun,BlendedSunDirection(target,invalid,0.5f),10).elevationAllowed,
                "nonfinite previous direction fails open");
        }
        Check(!PreparedSunRule(sun,BlendedSunDirection({1,0,0},{-1,0,0},0.5f),0).elevationAllowed,
            "degenerate opposing sun blend fails open");
        Check(!PreparedSunRule(sun,BlendedSunDirection({},previous,1),0).elevationAllowed,
            "missing settled sun fails open");
        Check(!PreparedSunRule(sun,target,nan).elevationAllowed &&
              !PreparedSunRule(sun,target,inf).elevationAllowed,"invalid sun cutoff fails open");
        Rule disabled=sun;disabled.enabled=false;
        const PreparedSunRule actorsOnly(disabled,overhead,10);
        Check(!actorsOnly.valid && actorsOnly.elevationAllowed && !CullSunPrepared(actorsOnly,small,eye,1),
            "actor elevation gate remains available when the small-caster rule is disabled");
        const PreparedSunRule overheadRule(sun,overhead,90);
        Check(overheadRule.elevationAllowed && !CullSunPrepared(overheadRule,small,eye,0),
            "inclusive elevation cutoff preserves the nearest cascade");
        Check(!CullSunPrepared(overheadRule,{{4000,0,0},150},eye,1),"prepared maximum radius is exclusive");
        Check(!CullSunPrepared(overheadRule,{{1510,0,0},10},eye,1),"prepared minimum distance is exclusive");
        for (float radius : {-1.0f,0.0f,nan,inf})
            Check(!CullSunPrepared(overheadRule,{{4000,0,0},radius},eye,1),"prepared invalid radius kept");
        Check(!CullSunPrepared(overheadRule,{{nan,0,0},10},eye,1) &&
              !CullSunPrepared(overheadRule,small,{inf,0,0},1),"prepared invalid positions kept");
        for (float radius : {1.0f,10.0f,100.0f}) {
            const float boundary=radius+std::max(sun.minDistance,radius/sun.minAngularSize);
            for (float distance : {std::nextafter(boundary,0.0f),boundary,std::nextafter(boundary,inf)}) {
                const Sphere caster{{distance,0,0},radius};
                Check(CullSunPrepared(overheadRule,caster,eye,1)==CullSun(sun,caster,eye,overhead,1,90),
                    "prepared rule matches reference at angular and distance boundaries");
            }
        }
        // Compare independently evaluated per-caster math with per-frame preparation.
        std::mt19937 random(15773);
        std::uniform_real_distribution<float> coordinate(-60000,60000),radius(-1,1000),
            component(-1,1),elevation(0,90),distance(0,100000),angular(0.00001f,0.1f);
        for (int i=0;i<30000;++i) {
            const Rule rule{i%11!=0,distance(random),radius(random)+2,angular(random),static_cast<std::uint32_t>(i%4)};
            const Vec3 sunDirection{component(random),component(random),component(random)};
            const float cutoff=elevation(random);
            const Sphere caster{{coordinate(random),coordinate(random),coordinate(random)},radius(random)};
            const Vec3 viewer{coordinate(random),coordinate(random),coordinate(random)};
            const auto cascade=static_cast<std::uint32_t>(i%5);
            Check(CullSunPrepared(PreparedSunRule(rule,sunDirection,cutoff),caster,viewer,cascade)==
                  CullSun(rule,caster,viewer,sunDirection,cascade,cutoff),"prepared sun rule matches reference");
        }
    }
    Check(!ShouldEvaluateCasterPolicies(true,false),"dry run without diagnostics skips unused caster work");
    Check(ShouldEvaluateCasterPolicies(true,true),"diagnostic dry run still evaluates candidates");
    Check(ShouldEvaluateCasterPolicies(false,false) && ShouldEvaluateCasterPolicies(false,true),
        "live removal evaluates with either diagnostics setting");
    for (bool interior : {false,true}) for (bool allowInteriors : {false,true})
        Check(!LampCellAllowed(false,interior,allowInteriors),"unknown cell keeps all lamp rules native");
    Check(LampCellAllowed(true,false,false) && LampCellAllowed(true,false,true),
        "known exteriors allow lamp rules with either interior setting");
    Check(!LampCellAllowed(true,true,false),"interior opt-out protects ordinary and actor lamp shadows");
    Check(LampCellAllowed(true,true,true),"interior opt-in allows ordinary and actor lamp shadows");
    Check(CullActorShadow(true, 3500, {{4000,0,0},20}, eye), "far actor shadow removed");
    Check(!CullActorShadow(true, 3500, {{3000,0,0},20}, eye), "near actor shadow kept");
    Check(!CullActorShadow(true, 1000, {{2000,0,0},20}, eye), "actor distance is raised to 30 m");
    Check(!CullActorShadow(false, 3500, {{9000,0,0},20}, eye), "disabled actor rule forwards");
    Check(!CullActorShadow(true, 3500, {{nan,0,0},20}, eye), "invalid actor bound kept");
    Check(ActorShadowCulled(false, 3500, {3600,0,0}, eye), "an actor beyond the limit loses its shadow");
    Check(!ActorShadowCulled(false, 3500, {3400,0,0}, eye), "an actor within the limit keeps its shadow");
    Check(ActorShadowCulled(true, 3500, {3400,0,0}, eye), "a culled actor stays culled between 90% and the limit");
    Check(!ActorShadowCulled(true, 3500, {3100,0,0}, eye), "a culled actor regains its shadow within 90% of the limit");
    Check(ActorShadowCulled(true, 1000, {2000,0,0}, eye) && !ActorShadowCulled(true, 1000, {1800,0,0}, eye),
        "hysteresis uses the raised 30 m limit");
    Check(!ActorShadowCulled(true, 3500, {nan,0,0}, eye), "an invalid actor position keeps its shadow");
    Check(CullRainOccluder(true, 128, 60), "small rain occluder removed");
    Check(!CullRainOccluder(true, 128, 128), "rain occluder at the radius threshold kept");
    Check(!CullRainOccluder(true, 128, 0), "empty rain occluder bound kept");
    Check(!CullRainOccluder(true, 128, nan), "invalid rain occluder bound kept");
    Check(!CullRainOccluder(false, 128, 10), "disabled rain rule forwards");
    sun.enabled = false;
    point.enabled = false;
    Check(!CullSun(sun, small, eye, overhead, 1, 10), "disabled sun rule forwards");
    Check(!CullPoint(point, small, eye, {4500,0,0}), "disabled point rule forwards");
    // Containment property behind SPS pruning: rejecting a valid parent sphere must
    // also reject contained smaller spheres under the same non-skinned rule.
    sun.enabled = true;
    for (int i=1; i<=20; ++i) {
        const Sphere parent{{5000,0,0},20};
        const Sphere child{{5000+static_cast<float>(i)/2,0,0},5};
        Check(!CullSmall(sun,parent,eye) || CullSmall(sun,child,eye), "contained sphere rule");
    }
    Check(profiles.size()==4, "OG, AE, Next-Gen and VR profiles");
    for (std::size_t i = 0; i < 3; ++i)
        Check(profiles[i].hooks.size()==4 && profiles[i].hooks[2].slot==0x1B, "Fallout parabolic append slot");
    // VR: the parabolic culler's Begin/Append are slots 0x1C/0x1D; it has no sun camera-update or
    // Render slot to hook (the plugin uses the cascade gather), so those sites are empty.
    const auto& vr = profiles[3];
    Check(vr.version[0]==1 && vr.version[1]==2 && vr.version[2]==72, "VR 1.2.72 profile");
    Check(vr.hooks[1].slot==0x1C && vr.hooks[2].slot==0x1D && vr.hooks[1].table==vr.hooks[2].table, "VR parabolic slots");
    Check(!vr.hooks[0].table && !vr.hooks[0].target && !vr.hooks[3].table && !vr.hooks[3].target, "VR has no sun vtable sites");
    Check(vr.registerTable && vr.shadowHandler.target != vr.occlusionHandler.target, "VR registration table and handlers");
    Check(profiles[0].hooks[3].slot==0x0A && profiles[0].hooks[3].table==profiles[0].hooks[0].table &&
          profiles[1].hooks[3].table==profiles[1].hooks[0].table, "sun Render slot in the directional light vtable");
    Check(profiles[0].registerTable && profiles[1].registerTable &&
          profiles[0].shadowHandler.target != profiles[0].occlusionHandler.target, "registration table and handlers profiled");
    const auto& native = profiles[0].hooks[0];
    Check(NativeHookSite(native.target, native.target, native.prefix, native.prefix), "native hook accepted");
    Check(!NativeHookSite(native.target+16, native.target, native.prefix, native.prefix), "foreign vtable hook rejected");
    auto detour = native.prefix;
    detour[0] = 0xE9;
    Check(!NativeHookSite(native.target, native.target, detour, native.prefix), "entry detour rejected");
    Check(!NativeHookSite(native.target, native.target,
          std::span(native.prefix).first(16), native.prefix), "truncated fingerprint rejected");
    std::cout << checks << " policy checks passed\n";
}
