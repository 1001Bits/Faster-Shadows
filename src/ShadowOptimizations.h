// Copyright (c) 2026 Noud. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ShadowPolicy.h"
#include <array>
#include <bit>
#include <cfloat>
#include <cstddef>
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace fps {
enum Optimization : std::uint32_t {
    ShadowVolume = 1, SquaredDistance = 2, LightSnapshot = 4,
    BatchCounters = 8, DecisionMemo = 16,
    SunTurnTolerance = 32,  // cadence keeps cascades through sun turns up to CadenceMaxSunTurn
    SkipEmptyCascades = 64,  // cascades with an empty depth range keep their map (SkipEmptyCascades)
    HoldFit = 128,  // rendered cascades keep their fitted camera for HoldFitFrames frames
    ShadowCache = 256,  // held cascades draw unmoving casters once and copy them back (StaticShadowCache)
    SuspendCasterRules = 512,  // lab A/B windows only: every caster rule stands down; the INI never sets it
    AllOptimizations = 1023
};
struct PreparedPointRule {
    Rule rule{};
    double inverseAngular{};
    bool valid{};
    explicit PreparedPointRule(Rule r = {}) noexcept : rule(r),
        inverseAngular(1.0 / static_cast<double>(r.minAngularSize)),
        valid(r.enabled && std::isfinite(r.minDistance) && r.minDistance >= 0 &&
              std::isfinite(r.maxRadius) && r.maxRadius > 0 &&
              std::isfinite(r.minAngularSize) && r.minAngularSize > 0) {}
};
inline double DistanceSquared(Vec3 a, Vec3 b) noexcept {
    const double x=static_cast<double>(a.x)-b.x;
    const double y=static_cast<double>(a.y)-b.y;
    const double z=static_cast<double>(a.z)-b.z;
    return x*x+y*y+z*z;
}
inline double RoundingGuard(Vec3 a, Vec3 b, double threshold) noexcept {
    // Keep ambiguous float-boundary decisions. Never turn a rounding difference
    // into an additional removal relative to the original hypot-based policy.
    return 32.0*FLT_EPSILON*(std::abs(a.x)+std::abs(a.y)+std::abs(a.z)+
        std::abs(b.x)+std::abs(b.y)+std::abs(b.z)+std::abs(threshold)+1.0);
}
inline bool CullPointSquared(const PreparedPointRule& prepared, Sphere object,
                             Vec3 eye, Vec3 light, float reach=0.0f) noexcept {
    if (!prepared.valid || !std::isfinite(object.radius) || object.radius <= 0 ||
        object.radius >= prepared.rule.maxRadius || !Finite(object.center) ||
        !Finite(eye) || !Finite(light)) return false;
    const double r=object.radius;
    // NearLight keeps nearest <= 1 or r/nearest >= .1: removal needs distance > r+max(1,10r).
    double nearLamp=r+std::max(1.0,r/kNearLightAngular);
    nearLamp+=RoundingGuard(object.center,light,nearLamp);
    const double l2=DistanceSquared(object.center,light);
    if (!std::isfinite(l2) || l2 >= static_cast<double>(FLT_MAX)*FLT_MAX || l2 <= nearLamp*nearLamp) return false;
    // The lamp scale needs the lamp distance itself. Round it up so a float-boundary
    // difference can only retain more than the reference LampShadowScale.
    double scale=1.0;
    if (std::isfinite(reach) && reach > 0.0f) {
        const double nearest=std::max(std::sqrt(l2)-r,1.0);
        scale=std::clamp(static_cast<double>(reach)/nearest,1.0,static_cast<double>(kMaxLampShadowScale))*(1.0+64.0*FLT_EPSILON);
    }
    const double scaled=r*scale;
    if (scaled >= prepared.rule.maxRadius) return false;
    double threshold=r+std::max(static_cast<double>(prepared.rule.minDistance),scaled*prepared.inverseAngular);
    threshold+=RoundingGuard(object.center,eye,threshold);
    const double d2=DistanceSquared(object.center,eye);
    return std::isfinite(d2) && d2 < static_cast<double>(FLT_MAX)*FLT_MAX && d2 > threshold*threshold;
}
struct Plane { double x{},y{},z{},d{}; };
struct ReceiverFrustum {
    std::array<Plane,4> sides{};
    bool valid{};
};
inline ReceiverFrustum WideReceiverFrustum(const float (&matrix)[4][4]) noexcept {
    ReceiverFrustum result{};
    for (const auto& row:matrix) for (float f:row) if (!std::isfinite(f)) return result;
    // NiCamera's homogeneous world-to-camera matrix. Use ONLY side planes,
    // widened to [-2*w,2*w]: contains both [-w,w] and [0,w] screen conventions,
    // and leaves a substantial guard band for projection jitter. No far-plane
    // rejection, reversed-depth assumption, cached depth, or near clipping.
    const double w2=static_cast<double>(matrix[3][0])*matrix[3][0]+
        static_cast<double>(matrix[3][1])*matrix[3][1]+
        static_cast<double>(matrix[3][2])*matrix[3][2];
    if (w2 < .5 || w2 > 2.0) return result; // refuse ortho/degenerate/unrecognized matrices
    for (std::size_t i=0;i<4;++i) {
        const auto axis=i/2;
        const double sign=i%2 ? -1.0 : 1.0;
        Plane p{2.0*matrix[3][0]+sign*matrix[axis][0],
                2.0*matrix[3][1]+sign*matrix[axis][1],
                2.0*matrix[3][2]+sign*matrix[axis][2],
                2.0*matrix[3][3]+sign*matrix[axis][3]};
        const double length=std::hypot(p.x,p.y,p.z);
        if (!std::isfinite(length) || length < 1e-6) return {};
        p.x/=length;p.y/=length;p.z/=length;p.d/=length;
        result.sides[i]=p;
    }
    result.valid=true;
    return result;
}
inline double SignedDistance(const Plane& p, Vec3 v) noexcept {
    return p.x*v.x+p.y*v.y+p.z*v.z+p.d;
}
inline bool CullPointShadowVolume(const ReceiverFrustum& view, Sphere caster, Vec3 light,
                                  double margin=64.0) noexcept {
    if (!view.valid || !Finite(caster.center) || !Finite(light) ||
        !std::isfinite(caster.radius) || caster.radius<=0 ||
        !std::isfinite(margin) || margin<0) return false;
    const double r=caster.radius;
    if (DistanceSquared(caster.center,light)<=r*r) return false;
    for (const auto& p:view.sides) {
        const double c=SignedDistance(p,caster.center);
        const double l=SignedDistance(p,light);
        const double error=RoundingGuard(caster.center,light,std::abs(c)+std::abs(l)+r);
        // X=L+t*(S-L), t>=1, for any S in the caster sphere.
        // max P(S)=P(C)+r. If this is outside AND its ray derivative
        // max(P(S)-P(L))<=0, the entire infinite shadow cone stays outside
        // this plane. An offscreen caster that shadows the view is retained.
        if (c+r < -margin-error && c-l+r < -error) return true;
    }
    return false;
}
enum PointDecision : std::uint8_t { kKeepCaster = 0, kSmallCaster = 1, kVolumeOnly = 2, kFarActor = 3 };
// Receiver coverage has not been verified for reflections/secondary views.
// Volume decisions remain analysis-only even with DryRun=0.
inline bool RemovePointDecision(std::uint8_t decision, bool dryRun) noexcept {
    return (decision == kSmallCaster || decision == kFarActor) && !dryRun;
}
template<class Counter>
inline void FlushCounter(Counter& destination, std::uint64_t value) noexcept {
    if (value) destination.fetch_add(value, std::memory_order_relaxed);
}
// Diagnostic counters written by many threads at once (sun cascade jobs, lamp culling).
// One shared atomic per counter made every write contend for one cache line: live
// 2026-10-12, Diamond City lost 5-17% with DryRun=1 and Diagnostics=1. Each thread
// owns a cache-line-aligned shard that only it writes; readers sum the shards, and a
// reset moves a baseline instead of writing into another thread's shard.
template<std::size_t Count>
class ShardedCounters {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4324)  // padding to the alignment is the point
#endif
    struct alignas(64) Shard { std::array<std::atomic<std::uint64_t>,Count> values{}; };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    static inline std::atomic<std::uint64_t> nextId{1};
    const std::uint64_t id=nextId.fetch_add(1,std::memory_order_relaxed);
    std::mutex lock;
    std::vector<std::unique_ptr<Shard>> shards;
    std::array<std::uint64_t,Count> base{};
    Shard& Local() {
        // Keyed by instance id, never by address: a destroyed instance is never matched.
        thread_local std::vector<std::pair<std::uint64_t,Shard*>> owned;
        for (const auto& [owner,shard]:owned) if (owner==id) return *shard;
        auto created=std::make_unique<Shard>();
        auto* shard=created.get();
        { std::scoped_lock guard(lock);shards.push_back(std::move(created)); }
        owned.emplace_back(id,shard);
        return *shard;
    }
public:
    static constexpr std::size_t shardAlignment=alignof(Shard);
    ShardedCounters()=default;
    ShardedCounters(const ShardedCounters&)=delete;
    ShardedCounters& operator=(const ShardedCounters&)=delete;
    void Add(std::size_t counter,std::uint64_t value=1) {
        if (!value || counter>=Count) return;
        auto& slot=Local().values[counter];
        // Single writer per shard: a plain load and store, no locked read-modify-write.
        slot.store(slot.load(std::memory_order_relaxed)+value,std::memory_order_relaxed);
    }
    std::uint64_t Sample(std::size_t counter,bool reset) {
        if (counter>=Count) return 0;
        std::scoped_lock guard(lock);
        std::uint64_t total=0;
        for (const auto& shard:shards) total+=shard->values[counter].load(std::memory_order_relaxed);
        const auto value=total-base[counter];
        if (reset) base[counter]=total;
        return value;
    }
};
template<class Camera, class Reader>
inline Vec3 PointLightPosition(std::uint32_t mask, const Camera* original, const Camera* current,
                              Vec3 snapshot, Reader&& live) noexcept {
    return (mask & LightSnapshot) && original == current ? snapshot : live(current);
}
struct DecisionKey {
    std::uintptr_t geometry{};
    Sphere bound{};
    Vec3 light{};
};
inline bool Exact(Vec3 a,Vec3 b) noexcept {
    return std::bit_cast<std::uint32_t>(a.x)==std::bit_cast<std::uint32_t>(b.x) &&
           std::bit_cast<std::uint32_t>(a.y)==std::bit_cast<std::uint32_t>(b.y) &&
           std::bit_cast<std::uint32_t>(a.z)==std::bit_cast<std::uint32_t>(b.z);
}
inline bool Exact(const DecisionKey& a,const DecisionKey& b) noexcept {
    return a.geometry==b.geometry && Exact(a.bound.center,b.bound.center) && Exact(a.light,b.light) &&
        std::bit_cast<std::uint32_t>(a.bound.radius)==std::bit_cast<std::uint32_t>(b.bound.radius);
}
class DecisionCache {
    struct Entry { std::uint64_t traversal{};DecisionKey key{};std::uint8_t decision{}; };
    std::array<Entry,128> entries{};
    std::uint64_t sequence{};
    static std::size_t Index(std::uintptr_t geometry) noexcept { return ((geometry>>4)^(geometry>>12))&127; }
public:
    std::uint64_t Begin() noexcept {
        if (++sequence==0) { entries={};sequence=1; }
        return sequence;
    }
    bool Find(std::uint64_t traversal,const DecisionKey& key,std::uint8_t& value) const noexcept {
        const auto& entry=entries[Index(key.geometry)];
        if (traversal && entry.traversal==traversal && Exact(entry.key,key)) { value=entry.decision;return true; }
        return false;
    }
    void Put(std::uint64_t traversal,const DecisionKey& key,std::uint8_t value) noexcept {
        entries[Index(key.geometry)]={traversal,key,value};
    }
};
} // namespace fps
