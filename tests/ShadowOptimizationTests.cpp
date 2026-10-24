#include "ShadowOptimizations.h"
#include "CascadeCadence.h"
#include "StaggeredCadence.h"
#include "ShadowCache.h"
#include "HookTransaction.h"
#include "SunWorkerSite.h"
#include "VrCadence.h"
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <cstdlib>
#include <thread>
#include <vector>
namespace {
std::uint64_t checks=0;
void Check(bool value,const char* why) {
    ++checks;if (!value) { std::cerr << "FAILED " << why << '\n';std::exit(1); }
}
bool Inside(const fps::ReceiverFrustum& view,fps::Vec3 p) {
    for (const auto& plane:view.sides) if (fps::SignedDistance(plane,p)<0) return false;
    return true;
}
struct FakePages {
    std::array<int,3> slots{1,1,1};
    int restoreFailures{}, conflict{-1}, rollbackFailure{-1}, restoreCalls{};
    bool prepare{true}, concurrentReplacement{};
    bool Prepare() { return prepare; }
    bool Restore() { ++restoreCalls;return restoreCalls>restoreFailures; }
    bool Replace(std::size_t index) {
        if (static_cast<int>(index)==conflict) { slots[index]=3;return false; }
        slots[index]=2;return true;
    }
    void Rollback(std::size_t index) {
        if (static_cast<int>(index)==rollbackFailure) return;
        if (concurrentReplacement && index==0) slots[index]=3;
        if (slots[index]==2) slots[index]=1;
    }
    bool Owned(std::size_t index) const { return slots[index]==2; }
};
}
int main() {
    using namespace fps;
    {
        // Bytes of the sun accumulate query from Ghidra Combined: OG AccumulateSunShadowLight+0x31
        // (rva 28503b1) and AE 1.11.240 MainRenderSetup+0x195 (rva 21f3245).
        const std::uint8_t og[]={0xe8,0x9a,0xb4,0x47,0xff,0x84,0xc0,0x74,0x27,0x80,0x3d,0xcb,0x8f,0x07,0x01,0x00,0x74,0x1e,
            0x48,0x8b,0x0d,0xa6,0x17,0xed,0x03,0x48,0x8d,0x15,0xef,0x2f,0xed,0x03,0x48,0x8b,0x89,0x08,0x02,0x00,0x00};
        const std::uint8_t ae[]={0xe8,0xd6,0xb6,0x5e,0xff,0x84,0xc0,0x74,0x23,0x80,0x3d,0xef,0x48,0xda,0x00,0x00,0x74,0x1a,
            0x48,0x8b,0x0d,0x02,0xad,0xc6,0x01,0x48,0x8d,0x15,0xdb,0x49,0xda,0x00,0x48,0x8b,0x89,0x08,0x02,0x00,0x00};
        const auto o=DecodeSunAccumulateSite(og,0x28503b1),a=DecodeSunAccumulateSite(ae,0x21f3245);
        Check(o && o->shadows==0x38c938c && o->sceneNode==0x6721b70 && o->cullingGroup==0x67233c0,
            "OG sun accumulate site decodes bShadows, St and kCullingGroup");
        Check(a && a->shadows==0x2f97b44 && a->sceneNode==0x3e5df60 && a->cullingGroup==0x2f97c40,
            "AE sun accumulate site decodes bShadows, St and kCullingGroup");
        // Next-Gen 1.10.984 (rva 20960a5): AE's code at its own addresses.
        const std::uint8_t ng[]={0xe8,0x16,0xd6,0x62,0xff,0x84,0xc0,0x74,0x23,0x80,0x3d,0xef,0xfe,0xc8,0x00,0x00,0x74,0x1a,
            0x48,0x8b,0x0d,0xb2,0xc5,0xc0,0x01,0x48,0x8d,0x15,0xdb,0xff,0xc8,0x00,0x48,0x8b,0x89,0x08,0x02,0x00,0x00};
        const auto n=DecodeSunAccumulateSite(ng,0x20960a5);
        Check(n && n->shadows==0x2d25fa4 && n->sceneNode==0x3ca2670 && n->cullingGroup==0x2d260a0,
            "NG sun accumulate site decodes bShadows, St and kCullingGroup");
        std::uint8_t changed[sizeof(og)];std::memcpy(changed,og,sizeof(og));changed[0x20]=0x4c;
        Check(!DecodeSunAccumulateSite(changed,0x28503b1),"a different instruction rejects the site");
    }
    Rule r{true,1000,150,.035f,0};PreparedPointRule prepared(r);
    std::mt19937 generator(9332);
    std::uniform_real_distribution<float> position(-60000,60000), radius(.01f,300), unit(-1,1), reachRange(0,20000);
    for (int i=0;i<200000;++i) {
        Sphere s{{position(generator),position(generator),position(generator)},radius(generator)};
        Vec3 eye{position(generator),position(generator),position(generator)};
        Vec3 lamp{position(generator),position(generator),position(generator)};
        const float reach=i%4 ? reachRange(generator) : 0.0f;
        Check(!CullPointSquared(prepared,s,eye,lamp,reach)||CullPoint(r,s,eye,lamp,reach),"squared rule never removes a reference-kept caster");
    }
    // Lamps close to small casters exercise the clamp and the near-light boundary.
    std::uniform_real_distribution<float> nearOffset(-3000,3000), smallRadius(.01f,40);
    for (int i=0;i<200000;++i) {
        Sphere s{{position(generator),position(generator),position(generator)},smallRadius(generator)};
        Vec3 lamp{s.center.x+nearOffset(generator),s.center.y+nearOffset(generator),s.center.z+nearOffset(generator)};
        Vec3 eye{s.center.x+position(generator)/4,s.center.y+position(generator)/4,s.center.z};
        const float reach=reachRange(generator)/4;
        Check(!CullPointSquared(prepared,s,eye,lamp,reach)||CullPoint(r,s,eye,lamp,reach),"squared rule keeps every lamp-scaled reference decision");
    }
    for (float rad:{.1f,1.f,10.f,99.f}) {
        const float boundary=rad+std::max(r.minDistance,rad/r.minAngularSize);
        for (float d:{std::nextafter(boundary,0.f),boundary,std::nextafter(boundary,INFINITY)})
            Check(!CullPointSquared(prepared,{{d,0,0},rad},{},{-10000,0,0}),"ambiguous distance boundary retained");
        const float nearLamp=rad+std::max(1.0f,rad/kNearLightAngular);
        for (float d:{std::nextafter(nearLamp,0.f),nearLamp,std::nextafter(nearLamp,INFINITY)})
            Check(!CullPointSquared(prepared,{{10000,0,0},rad},{},{10000+d,0,0}),"near-lamp threshold retained");
    }
    const float nan=std::numeric_limits<float>::quiet_NaN(),inf=std::numeric_limits<float>::infinity();
    for (float f:{nan,inf,-inf,0.f,-1.f}) {
        Check(!CullPointSquared(prepared,{{10000,0,0},f},{},{20000,0,0}),"invalid radius retained");
        Check(!CullPointSquared(prepared,{{f,0,0},10},{nan,0,0},{20000,0,0}),"invalid coordinates retained");
    }
    r.enabled=false;Check(!CullPointSquared(PreparedPointRule(r),{{10000,0,0},10},{},{20000,0,0}),"disabled rule");
    const float matrix[4][4]={{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,1,0}};
    const auto view=WideReceiverFrustum(matrix);
    Check(view.valid,"perspective matrix accepted");
    Check(CullPointShadowVolume(view,{{1000,0,100},10},{0,0,100}),"entire shadow cone outside view");
    Check(!CullPointShadowVolume(view,{{1000,0,100},10},{1200,0,100}),"offscreen caster with shadow toward visible receivers retained");
    Check(!CullPointShadowVolume(view,{{0,0,100},10},{1000,0,100}),"visible caster retained");
    Check(!CullPointShadowVolume(view,{{1000,0,100},1000},{0,0,100}),"large intersecting precombine retained");
    Check(!CullPointShadowVolume(view,{{1000,0,100},10},{1001,0,100}),"lamp inside caster retained");
    float invalid[4][4]{};Check(!WideReceiverFrustum(invalid).valid,"zero matrix refused");
    invalid[0][0]=nan;Check(!WideReceiverFrustum(invalid).valid,"NaN matrix refused");
    Check(!CullPointShadowVolume({},{{1000,0,100},10},{0,0,100}),"unrecognized camera forwards");
    // Independent point samples along the actual sphere's infinite shadow rays.
    // A rejection certificate must never have a visible witness.
    for (int i=0;i<10000;++i) {
        Sphere s{{position(generator),position(generator),position(generator)},radius(generator)};
        Vec3 lamp{position(generator),position(generator),position(generator)};
        if (!CullPointShadowVolume(view,s,lamp)) continue;
        for (int sample=0;sample<32;++sample) {
            Vec3 d{unit(generator),unit(generator),unit(generator)};
            const float length=std::hypot(d.x,d.y,d.z);
            if (length<.0001f) continue;
            const Vec3 surface{s.center.x+s.radius*d.x/length,s.center.y+s.radius*d.y/length,s.center.z+s.radius*d.z/length};
            for (float t:{1.f,2.f,10.f,1000.f}) {
                Vec3 receiver{lamp.x+t*(surface.x-lamp.x),lamp.y+t*(surface.y-lamp.y),lamp.z+t*(surface.z-lamp.z)};
                Check(!Inside(view,receiver),"rejected cone has no visible sampled receiver");
            }
        }
    }
    DecisionCache cache;auto traversal=cache.Begin();
    DecisionKey key{0x12340,{{10000,0,0},10},{20000,0,0}};
    std::uint8_t result{};
    Check(!cache.Find(traversal,key,result),"fresh memo empty");
    cache.Put(traversal,key,2);Check(cache.Find(traversal,key,result)&&result==2,"exact memo hit");
    auto moved=key;moved.bound.center.x+=1;
    Check(!cache.Find(traversal,moved,result),"moving caster invalidates memo");
    moved=key;moved.light.x+=1;Check(!cache.Find(traversal,moved,result),"moving light invalidates memo");
    moved=key;moved.bound.radius+=1;Check(!cache.Find(traversal,moved,result),"changed radius invalidates memo");
    Check(!cache.Find(cache.Begin(),key,result),"new light/traversal cannot reuse old result");
    auto inner=cache.Begin();cache.Put(inner,key,0);
    Check(!cache.Find(traversal,key,result),"nested traversal collision cannot reuse inner decision");
    Check(cache.Find(inner,key,result)&&result==0,"kept decisions are memoized too");
    Check(!cache.Find(0,key,result),"missing traversal fails open");
    // Exercise the same transaction algorithm used by the native installer.
    FakePages success;
    Check(InstallTransaction(3,success).active,"complete transaction enables hooks");
    FakePages failedPrepare;failedPrepare.prepare=false;
    Check(!InstallTransaction(3,failedPrepare).active && failedPrepare.slots[0]==1,
        "prepare failure writes no pointers");
    FakePages failedRestore;failedRestore.restoreFailures=1;
    const auto rolledBack=InstallTransaction(3,failedRestore);
    Check(!rolledBack.active && rolledBack.rollbackComplete && rolledBack.protectionsRestored &&
        failedRestore.slots==std::array<int,3>{1,1,1},"restore failure rolls back every installed pointer");
    FakePages conflict;conflict.conflict=2;
    const auto collided=InstallTransaction(3,conflict);
    Check(!collided.active && collided.rollbackComplete && conflict.slots==std::array<int,3>{1,1,3},
        "CAS conflict rolls back our writes and preserves foreign pointer");
    FakePages changed;changed.conflict=2;changed.concurrentReplacement=true;
    Check(InstallTransaction(3,changed).rollbackComplete && changed.slots[0]==3,
        "concurrent replacement during rollback survives");
    FakePages failedRollback;failedRollback.restoreFailures=1;failedRollback.rollbackFailure=1;
    const auto residual=InstallTransaction(3,failedRollback);
    Check(!residual.active && !residual.rollbackComplete,"failed rollback never enables residual callbacks");
    FakePages persistent; persistent.restoreFailures=100;
    const auto protection=InstallTransaction(3,persistent);
    Check(!protection.active && protection.rollbackComplete && !protection.protectionsRestored,
        "persistent page failure is reported after pointer rollback");
    for (std::uint8_t decision:std::array<std::uint8_t,5>{kKeepCaster,kSmallCaster,kVolumeOnly,kFarActor,4}) {
        Check(!RemovePointDecision(decision,true),"dry run never removes");
        Check(RemovePointDecision(decision,false)==(decision==kSmallCaster || decision==kFarActor),
            "unvalidated volumes always forward");
    }
    struct CountingAtomic {
        std::uint64_t sum{},calls{};
        void fetch_add(std::uint64_t n,std::memory_order) { sum+=n;++calls; }
    } counter;
    FlushCounter(counter,0);Check(counter.calls==0,"empty traversal performs no atomic writes");
    FlushCounter(counter,123);Check(counter.calls==1 && counter.sum==123,"one flush accounts for entire traversal");
    {
        Check(ShardedCounters<3>::shardAlignment>=64,"each thread's counters own whole cache lines");
        ShardedCounters<3> sharded;
        std::vector<std::thread> threads;
        for (int t=0;t<8;++t) threads.emplace_back([&sharded,t] {
            for (int i=0;i<100000;++i) sharded.Add(0);
            sharded.Add(1,static_cast<std::uint64_t>(t));
            sharded.Add(2,0);
        });
        for (auto& thread:threads) thread.join();
        Check(sharded.Sample(0,false)==800000,"concurrent per-thread adds sum exactly");
        Check(sharded.Sample(1,false)==28 && sharded.Sample(2,false)==0,"counters stay separate");
        Check(sharded.Sample(0,true)==800000 && sharded.Sample(0,false)==0,"a reset reports the total and restarts at zero");
        sharded.Add(0,5);
        Check(sharded.Sample(0,false)==5 && sharded.Sample(1,false)==28,"counts after a reset; other counters unaffected");
        Check(sharded.Sample(3,true)==0,"an out-of-range counter reads zero");
        ShardedCounters<3> other;
        other.Add(0,7);
        Check(other.Sample(0,false)==7 && sharded.Sample(0,false)==5,"instances never share a thread's shard");
    }
    {
        CadenceSettings s;
        CascadeView view;
        view.count=3;view.eye={100,200,300};view.sun={0.3f,0.4f,0.866f};view.cell=7;
        view.rotation={1,0,0,0,1,0,0,0,1};view.slots={6ull<<32,6ull<<32|1,6ull<<32|2,0};
        view.frustum={-1,1,1,-1,5,10000};view.light=100;
        view.cameras={101,102,103,0};view.accumulators={201,202,203,0};
        CascadeCadence cadence;
        // A new cascade set renders everything once.
        Check(cadence.Plan(s,view)==0,"a new cascade set renders every cascade");
        cadence.Rendered(7,view);
        std::array<int,3> lastRender{1,1,1};
        for (int frame=2;frame<40;++frame) {
            const auto skip=cadence.Plan(s,view);
            Check(!(skip&1),"cascade 0 always renders");
            Check(std::popcount(skip)<=1,"at most one cascade waits per frame");
            for (int c=0;c<3;++c) if (!(skip>>c&1)) lastRender[c]=frame;
            for (int c=0;c<3;++c) Check(frame-lastRender[c]<2,"every cascade renders at least every other frame");
            if (frame>2) Check(skip==2 || skip==4,"middle and far take turns");
            cadence.Rendered(7&~skip,view);
        }
        const auto guarded=[&](auto change,const char* why) {
            CascadeCadence c;CascadeView v=view;
            c.Plan(s,v);c.Rendered(7,v);
            change(v);
            Check(c.Plan(s,v)==0,why);
        };
        guarded([](CascadeView& v){ v.eye.x+=65; },"a move past the limit renders all");
        guarded([](CascadeView& v){ v.rotation={0.99863f,-0.05234f,0,0.05234f,0.99863f,0,0,0,1}; },"a 3 degree turn renders all");
        guarded([](CascadeView& v){ v.sun.x+=0.001f; },"a sun change renders all");
        guarded([](CascadeView& v){ v.sunPrevious.x+=0.001f; },"a previous sun direction change renders all");
        for (float phase : {-0.1f, 1.1f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
            guarded([phase](CascadeView& v){ v.sunBlendProgress=phase; },"invalid sun blend progress renders all");
        guarded([](CascadeView& v){ v.sunPrevious.x=std::numeric_limits<float>::quiet_NaN(); },"a non-finite previous sun renders all");
        {
            CascadeCadence c;CascadeView v=view;
            v.sunPrevious={0.31f,0.39f,0.866f};v.sunBlendProgress=0;
            c.Plan(s,v);c.Rendered(7,v);
            // Fallout changes the target once, then advances the camera blend
            // while that target stays constant. Target-only history misses it.
            for (int frame=1;frame<=20;++frame) {
                v.sunBlendProgress=static_cast<float>(frame)/20;
                Check(c.Plan(s,v)==0,"an advancing native sun blend refreshes all cascades despite a fixed target");
                c.Rendered(7,v);
            }
            v.sunPrevious=v.sun;
            Check(c.Plan(s,v)==0,"completing native sun history refreshes all cascades");
            c.Rendered(7,v);
            Check(c.Plan(s,v)!=0,"cascade reuse resumes when the native sun blend settles");
        }
        {
            // CadenceMaxSunTurn: a slow native sun blend no longer refreshes every cascade, a bigger
            // turn still does, and the default (cosine 1) keeps the exact comparison above.
            CadenceSettings tolerant=s;tolerant.minSunCosine=std::cos(0.25f*0.0174532925f);
            CascadeCadence c;CascadeView v=view;
            v.sunPrevious={0.31f,0.39f,0.866f};v.sunBlendProgress=0;
            c.Plan(tolerant,v);c.Rendered(7,v);
            int waited=0;
            for (int frame=1;frame<=20;++frame) {
                v.sunBlendProgress=static_cast<float>(frame)/20;
                const auto skip=c.Plan(tolerant,v);
                waited+=skip!=0;
                c.Rendered(7&~skip,v);
            }
            Check(waited>=15,"within the sun-turn limit a native sun blend lets cascades keep waiting");
            CascadeView turned=v;turned.sun={0.29297f,0.40517f,0.866f};  // the settled sun turned 1 degree
            Check(c.Plan(tolerant,turned)==0,"a sun turn past the limit refreshes every cascade");
            CascadeView broken=v;broken.sun={0,0,0};broken.sunBlendProgress=1;
            Check(c.Plan(tolerant,broken)==0,"an unusable sun direction refreshes every cascade");
        }
        {
            CascadeCadence c;CascadeView v=view;
            v.sunPrevious={0.31f,0.39f,0.866f};v.sunBlendProgress=0.5f;
            c.Plan(s,v);c.Rendered(7,v);
            Check(c.Plan(s,v)!=0,"an unchanged blend pose can reuse a map when time is paused");
        }
        guarded([](CascadeView& v){ v.cell=8; },"a cell change renders all");
        for (std::size_t plane=0;plane<view.frustum.size();++plane)
            guarded([plane](CascadeView& v){ v.frustum[plane]*=1.5f; },"a viewer projection change refreshes every cascade");
        guarded([](CascadeView& v){ v.orthographic=true; },"switching projection type refreshes all cascades");
        guarded([](CascadeView& v){ v.frustum[0]=std::numeric_limits<float>::quiet_NaN(); },"invalid projection never reuses a map");
        guarded([](CascadeView& v){ ++v.light; },"a new sun light cannot reuse the previous light's maps");
        {
            CascadeCadence c;CascadeView v=view;
            c.Plan(s,v);c.Rendered(7,v);
            ++v.cameras[2];
            Check(!(c.Plan(s,v)&4),"replacing a cascade camera invalidates its map");
            c.Rendered(7,v);
            ++v.accumulators[1];
            Check(!(c.Plan(s,v)&2),"replacing a cascade accumulator invalidates its map");
        }
        {
            struct SavedCamera { bool valid{true}; };
            std::array<SavedCamera,4> cameras;
            CascadeCadence c;c.Plan(s,view);c.Rendered(7,view);
            Check(c.Plan(s,view)!=0,"unchanged scene is reusable before load invalidation");
            ResetCascadeHistory(c,cameras);
            Check(std::none_of(cameras.begin(),cameras.end(),[](const auto& camera){return camera.valid;}),
                "load invalidates every saved cascade camera");
            Check(c.Plan(s,view)==0,"load at exactly the same pose, time and slots still refreshes every cascade");
            c.Rendered(7,view);
            Check(c.Plan(s,view)!=0,"reuse resumes after a post-load full render");
        }
        {
            CascadeCadence c;CascadeView v=view;
            c.Plan(s,v);c.Rendered(7,v);
            v.slots[2]^=1;
            Check(!(c.Plan(s,v)&4),"a different slice renders that cascade");
        }
        guarded([](CascadeView& v){ v.count=2; },"a different cascade count renders all");
        guarded([](CascadeView& v){ v.eye.x=std::numeric_limits<float>::quiet_NaN(); },"a non-finite eye renders all");
        {
            CascadeCadence c;CascadeView v=view;
            c.Plan(s,v);c.Rendered(7,v);
            v.eye.x+=60;v.rotation={0.99985f,-0.01745f,0,0.01745f,0.99985f,0,0,0,1};
            Check(c.Plan(s,v)!=0,"a small move and a 1 degree turn may still wait");
        }
        {
            CascadeCadence c;CascadeView v=view;
            c.Plan(s,v);c.Rendered(7,v);
            c.Plan(s,v);  // a frame planned but never rendered
            Check(c.Plan(s,v)==0,"after the interval without a render, everything renders");
        }
        {
            CadenceSettings every;every.midInterval=1;every.farInterval=1;
            CascadeCadence c;c.Plan(every,view);c.Rendered(7,view);
            for (int frame=0;frame<5;++frame) { Check(c.Plan(every,view)==0,"interval 1 never waits");c.Rendered(7,view); }
        }
        {
            CadenceSettings far3;far3.midInterval=1;far3.farInterval=3;
            CascadeCadence c;c.Plan(far3,view);c.Rendered(7,view);
            int farRenders=0;
            for (int frame=0;frame<30;++frame) {
                const auto skip=c.Plan(far3,view);
                Check(!(skip&3),"only the far cascade waits");
                farRenders+=!(skip&4);
                c.Rendered(7&~skip,view);
            }
            Check(farRenders==10,"interval 3 renders the far cascade every third frame");
        }
        {
            CadenceSettings two;two.maxWaiting=2;two.farInterval=4;
            CascadeCadence c;c.Plan(two,view);c.Rendered(7,view);
            std::array<int,3> last{1,1,1};
            int waits=0;
            for (int frame=2;frame<60;++frame) {
                const auto skip=c.Plan(two,view);
                Check(!(skip&1),"cascade 0 always renders with two waiting");
                Check(std::popcount(~skip&6u)<=1,"middle and far never render on the same frame");
                for (int k=0;k<3;++k) if (!(skip>>k&1)) last[k]=frame;
                Check(frame-last[1]<2 && frame-last[2]<=4,"the middle renders every other frame, the far waits at most one extra frame");
                Check(frame<8 || frame-last[2]<4,"once out of step with the middle, the far renders every fourth frame");
                if (frame>=20) waits+=std::popcount(skip);
                c.Rendered(7&~skip,view);
            }
            Check(waits==50,"in steady state 5 of every 8 cascade updates wait");
            CascadeView moved=view;moved.eye.x+=65;
            Check(c.Plan(two,moved)==0,"a move past the limit renders all with two waiting");
        }
        {
            CadenceSettings two;two.maxWaiting=2;
            CascadeCadence c;c.Plan(two,view);c.Rendered(7,view);
            Check(c.Plan(two,view)==6,"two waiting at equal intervals lets both wait once");
            c.Rendered(1,view);
            Check(c.Plan(two,view)==4,"cascades due together render one at a time, nearest first");
            c.Rendered(3,view);
            Check(c.Plan(two,view)==2,"the deferred cascade renders the next frame");
        }
        {
            CascadeCadence c;CascadeView one=view;one.count=1;
            c.Plan(s,one);c.Rendered(1,one);
            Check(c.Plan(s,one)==0,"a single cascade never waits");
        }
    }
    {
        // Staggered (FO4CS) rules. The view looks along +x from the origin; the sun shines straight
        // down, so a cascade's box spans world y (box x) and world x (box y).
        CadenceSettings s;s.policy=1;
        CascadeView base;
        base.count=3;base.eye={0,0,0};base.cell=7;base.light=100;
        base.cameras={101,102,103,0};base.accumulators={201,202,203,0};base.slots={6ull<<32,6ull<<32|1,6ull<<32|2,0};
        StaggeredView view;
        view.eye=base.eye;view.axes={Vec3{1,0,0},Vec3{0,0,1},Vec3{0,1,0}};
        view.halfWidth=1;view.halfHeight=0.6f;view.nearPlane=5;view.splitEnds={1000,3000,8000,0};view.blend=100;
        const Vec3 down{0,0,-1};
        view.fitForward={down,down,down,down};
        // A box around cascade `i`'s receiver slice with `margin` units to spare on every side.
        const auto fit=[&](const StaggeredView& v,std::uint32_t i,float margin) {
            const float nearDepth=i==0?v.nearPlane:std::max(v.nearPlane,v.splitEnds[i-1]-v.blend);
            const float farDepth=v.splitEnds[i]+v.blend;
            const float wide=farDepth*v.halfWidth+margin;
            return MakeKeptBox({v.eye.x,v.eye.y,10000},{down,Vec3{1,0,0},Vec3{0,1,0}},
                {v.eye.y-wide,v.eye.y+wide,v.eye.x+farDepth+margin,v.eye.x+nearDepth-margin,0,20000},2048,2048);
        };
        const auto boxes=[&](const StaggeredView& v,float margin) {
            std::array<KeptBox,4> b{};
            for (std::uint32_t i=0;i<3;++i) b[i]=fit(v,i,margin);
            return b;
        };
        Check(KeptBoxCovers(fit(view,1,50),view,1),"a box fitted around the slice covers it");
        Check(!KeptBoxCovers(fit(view,1,50),view,2),"a nearer cascade's box does not cover a farther slice");
        StaggeredView narrow=view;narrow.blend=150;
        Check(!KeptBoxCovers(fit(view,1,40),narrow,1),"a wider blend needs a larger box");
        Check(!KeptBoxCovers(KeptBox{},view,1),"an empty box covers nothing");
        Check(!MakeKeptBox({},{down,Vec3{1,0,0},Vec3{0,1,0}},{1,-1,1,-1,0,1},2048,2048).valid,"an inverted box is refused");
        Check(!MakeKeptBox({},{down,Vec3{1,0,0},Vec3{0,1,0}},{-1,1,1,-1,0,1},0,2048).valid,"a box without texels is refused");
        {
            StaggeredCadence c;
            Check(c.Plan(s,base,view)==0,"a new cascade set draws every cascade");
            c.Rendered(7,base,boxes(view,50));
            int waits=0;
            for (int frame=2;frame<42;++frame) {
                const auto skip=c.Plan(s,base,view);
                Check(!(skip&1),"the nearest cascade always draws");
                Check(std::popcount(skip)==1,"at 1/2/2 exactly one of middle and far waits every frame");
                waits+=std::popcount(skip);
                c.Rendered(7&~skip,base,boxes(view,50));
            }
            Check(waits==40,"middle and far take turns");
        }
        {
            CadenceSettings perf=s;perf.farInterval=4;
            StaggeredCadence c;c.Plan(perf,base,view);c.Rendered(7,base,boxes(view,50));
            std::array<int,3> last{1,1,1};int waits=0,both=0;
            for (int frame=2;frame<42;++frame) {
                const auto skip=c.Plan(perf,base,view);
                for (int k=0;k<3;++k) if (!(skip>>k&1)) last[k]=frame;
                Check(frame-last[1]<2 && frame-last[2]<4,"at 1/2/4 the middle draws every other frame, the far every fourth");
                Check(std::popcount(~skip&6u)<=1,"middle and far never draw on the same frame");
                waits+=std::popcount(skip);both+=skip==6;
                c.Rendered(7&~skip,base,boxes(view,50));
            }
            Check(waits==50 && both==10,"in steady state 5 of every 8 cascade updates wait, both on every fourth frame");
        }
        const auto settled=[&](StaggeredCadence& c) {
            c.Plan(s,base,view);c.Rendered(7,base,boxes(view,50));
            const auto skip=c.Plan(s,base,view);c.Rendered(7&~skip,base,boxes(view,50));
            return skip;
        };
        const auto after=[&](auto change,const char* why) {
            StaggeredCadence c;settled(c);
            CascadeView b=base;StaggeredView v=view;
            change(b,v);
            Check(c.Plan(s,b,v)==0,why);
        };
        after([](CascadeView& b,StaggeredView& v){ b.eye.x+=257;v.eye=b.eye; },"a camera jump draws every cascade");
        after([](CascadeView& b,StaggeredView&){ b.cell=8; },"a cell change draws every cascade");
        after([](CascadeView& b,StaggeredView&){ ++b.light; },"another light draws every cascade");
        after([](CascadeView& b,StaggeredView&){ b.eye.x=std::numeric_limits<float>::quiet_NaN(); },"a non-finite eye draws every cascade");
        after([](CascadeView&,StaggeredView& v){ v.fitForward[2]={0.0076f,0,-1}; },"a sun turn over 0.25 degrees draws every cascade");
        after([](CascadeView&,StaggeredView& v){ v.fitForward[2]={0,0,0}; },"a degenerate fitted camera draws every cascade");
        after([](CascadeView& b,StaggeredView&){ b.count=2; },"a different cascade count draws every cascade");
        {
            StaggeredCadence c;settled(c);
            StaggeredView v=view;v.fitForward[2]={0.0035f,0,-1};
            Check(c.Plan(s,base,v)!=0,"a sun turn of 0.2 degrees may still wait");
        }
        {
            StaggeredCadence c;settled(c);
            CascadeView b=base;StaggeredView v=view;
            b.eye.x+=200;v.eye=b.eye;  // under a jump, but the slice now ends past the kept boxes
            Check(c.Plan(s,b,v)==0,"receivers outside their kept box draw that cascade");
        }
        {
            StaggeredCadence c;
            c.Plan(s,base,view);c.Rendered(7,base,boxes(view,600));
            CascadeView b=base;StaggeredView v=view;
            b.eye.x+=200;v.eye=b.eye;
            Check(c.Plan(s,b,v)!=0,"a move the kept boxes still cover may wait");
        }
        {
            StaggeredCadence c;settled(c);
            CascadeView b=base;++b.cameras[1];++b.cameras[2];
            Check(c.Plan(s,b,view)==0,"a replaced cascade camera draws that cascade");
            c.Rendered(7,b,boxes(view,50));
            b.slots[2]^=1;b.slots[1]^=1;
            Check(c.Plan(s,b,view)==0,"a different slice draws that cascade");
        }
        {
            StaggeredCadence c;settled(c);
            c.Plan(s,base,view);  // planned but never rendered
            Check(c.Plan(s,base,view)==0,"a cascade past its interval without a draw is due");
        }
        {
            StaggeredCadence c;StaggeredView v=view;v.blend=std::numeric_limits<float>::quiet_NaN();
            c.Plan(s,base,v);c.Rendered(7,base,boxes(view,50));
            for (int frame=0;frame<4;++frame) Check(c.Plan(s,base,v)==0,"without the blend setting nothing waits");
        }
        {
            StaggeredCadence c;settled(c);
            c.Reset();
            Check(c.Plan(s,base,view)==0,"after a reset every cascade draws");
        }
        {
            // HoldFitFrames: rendered cascades keep their fitted box for N frames while it covers the view.
            CadenceSettings exact;  // minSunCosine 1: any sun change refits
            FitHold hold;
            Check(hold.Plan(3,exact,base,view,0)==0,"nothing is held before a first render");
            hold.Rendered(7,0,base,boxes(view,50));
            for (int frame=0;frame<3;++frame) Check(hold.Plan(3,exact,base,view,0)==7,"a covering box is held for N frames");
            Check(hold.Plan(3,exact,base,view,0)==0,"after N frames every cascade refits");
            hold.Rendered(7,0,base,boxes(view,50));
            Check(hold.Plan(3,exact,base,view,4)==3,"a cascade the cadence keeps is not held");
            FitHold moved;moved.Rendered(7,0,base,boxes(view,50));
            CascadeView b=base;StaggeredView v=view;b.eye.x+=200;v.eye=b.eye;
            Check(moved.Plan(3,exact,b,v,0)==0,"a view that leaves the held boxes refits");
            FitHold sun;sun.Rendered(7,0,base,boxes(view,50));
            CascadeView turned=base;turned.sun.x+=0.001f;
            Check(sun.Plan(3,exact,turned,view,0)==0,"a sun change refits in exact mode");
            FitHold replaced;replaced.Rendered(7,0,base,boxes(view,50));
            CascadeView other=base;++other.cameras[1];
            Check(replaced.Plan(3,exact,other,view,0)==5,"a replaced cascade camera refits that cascade");
            FitHold kept;kept.Rendered(7,0,base,boxes(view,50));
            kept.Plan(3,exact,base,view,0);kept.Rendered(7,7,base,boxes(view,600));
            Check(kept.Plan(3,exact,b,v,0)==0,"a held render does not replace the held box");
        }
    }
    {
        // SkipEmptyCascades: which depth ranges are empty (ends as UpdateCamerasI writes them).
        Check(EmptyCascades({800,3000,20000,0},3)==0,"Ultra and High keep every cascade");
        Check(EmptyCascades({800,3000,3000,0},3)==4,"3 splits at a 3000 shadow distance: the last cascade is empty");
        Check(EmptyCascades({800,3000,2000,0},3)==4,"a shadow distance below 3000 empties the last cascade too");
        Check(EmptyCascades({800,3000,1,14000},4)==4,"4 splits: the engine's 1.0 middle end is empty, the far one is not");
        Check(EmptyCascades({800,700,0,0},2)==2,"2 splits below 800 empty the second cascade");
        Check(EmptyCascades({800,3000,0,0},2)==0 && EmptyCascades({800,3000,3000,0},1)==0,"cascade 0 is never empty");
        const float nan=std::numeric_limits<float>::quiet_NaN();
        Check(EmptyCascades({800,nan,3000,0},3)==0 && EmptyCascades({nan,3000,3000,0},3)==0,"unreadable ends keep every cascade");
    }
    {
        // Static shadow cache: which shadow passes draw a caster that may move (fake engine objects).
        alignas(8) std::byte pass[0x48]{},geometry[0x148]{},property[0x38]{};
        const auto put=[](std::byte* at,const void* value){std::memcpy(at,&value,sizeof(value));};
        put(pass+passGeometry,geometry);put(pass+passProperty,property);
        const float transform[16]={1,0,0,0, 0,1,0,0, 0,0,1,0, 100,200,300,1};
        std::memcpy(geometry+geometryWorld,transform,sizeof(transform));
        std::memcpy(geometry+geometryPreviousWorld,transform,sizeof(transform));
        constexpr std::uint64_t skinned=2;
        Check(!MovingCaster(pass,skinned),"an unmoved, unskinned caster comes from the cache");
        float moved[16];std::memcpy(moved,transform,sizeof(moved));moved[12]+=0.5f;
        std::memcpy(geometry+geometryWorld,moved,sizeof(moved));
        Check(MovingCaster(pass,skinned),"a caster whose world transform changed draws every frame");
        std::memcpy(geometry+geometryWorld,transform,sizeof(transform));
        put(geometry+geometrySkin,property);
        Check(MovingCaster(pass,skinned),"a skinned geometry draws every frame");
        put(geometry+geometrySkin,nullptr);
        const std::uint64_t flags=skinned;std::memcpy(property+propertyFlagsOffset,&flags,sizeof(flags));
        Check(MovingCaster(pass,skinned),"a skinned shader property draws every frame");
        put(pass+passGeometry,nullptr);
        Check(MovingCaster(pass,skinned) && MovingCaster(nullptr,skinned),"an unreadable pass draws every frame");
    }
    {
        // Slice guard: a kept cascade whose slice a lamp map reused redraws; nothing else changes.
        const std::array<std::uint64_t,4> slots{6ull<<32,6ull<<32|1,6ull<<32|2,0};
        Check(RedrawReusedSlices(6,slots,3,0b010,6)==2,"a point lamp's map in slice 1 redraws a kept middle cascade");
        Check(RedrawReusedSlices(4,slots,3,0b010,6)==0,"a middle cascade that renders anyway needs nothing");
        Check(RedrawReusedSlices(6,slots,3,0b001,6)==0,"a spot lamp's slice 0 never touches a kept cascade");
        Check(RedrawReusedSlices(6,slots,3,0b110,6)==6,"every reused kept slice redraws");
        Check(RedrawReusedSlices(6,slots,3,0b010,7)==0,"another target's slices are not the sun's");
        Check(RedrawReusedSlices(6,slots,3,~0u&~(1u<<2),6)==2,"without the guard only the far slice may stay kept");
    }
    {
        // VR: bits are only ever added, in whole update windows, and only where the native writer emitted them.
        const std::array<float,9> ahead{1,0,0,0,1,0,0,0,1};
        const float c=std::cos(0.3f),n=std::sin(0.3f);  // about 17 degrees about z
        const std::array<float,9> turned{c,-n,0,n,c,0,0,0,1};
        VrCadenceSettings rules;
        VrFrame in;in.allowed=0xF;in.delay=3;in.sun={0,0.5f,0.8f};in.rotation=ahead;in.cell=7;
        VrCadence vr;
        // Warm up: native {3,5,3,9} rotation, update frames every 3, camera still.
        const std::array<std::uint32_t,4> table{3,5,3,9};
        // The engine's next update frame: frame f updates when f is a multiple of the period.
        const auto next=[](std::int32_t f) { return f%3 ? f+3-f%3 : f; };
        for (std::int32_t f=0;f<12;++f) {
            in.frame=f;in.native=table[f&3];in.nextUpdate=next(f);
            const auto plan=vr.Plan(rules,in);
            // The first two windows fit cascades that have no fit yet.
            if (f>=6) Check(plan.mask==in.native && !plan.forceUpdate,"a still camera adds nothing to the native mask");
        }
        // The head turns; the next frames are not update frames, so nothing is added until one comes.
        in.rotation=turned;in.frame=13;in.native=5;in.nextUpdate=15;
        Check(vr.Plan(rules,in).added==0,"stale cascades wait for the next update frame");
        in.frame=14;in.native=3;
        Check(vr.Plan(rules,in).added==0,"still waiting for the update frame");
        in.frame=15;in.native=9;in.nextUpdate=15;
        auto plan=vr.Plan(rules,in);
        Check(plan.added==0b0110 && plan.mask==0xF,"the update frame adds every stale cascade the writer emits");
        in.frame=16;in.native=3;in.nextUpdate=18;
        Check(vr.Plan(rules,in).mask==0b1111,"the window keeps them through the period");
        in.frame=17;in.native=5;
        Check(vr.Plan(rules,in).mask==0b1111,"until its last (swap) frame");
        in.frame=18;in.native=3;in.nextUpdate=18;
        Check(vr.Plan(rules,in).added==0,"refit cascades are fresh in the next window");
        // Shadow Boost's two-cascade lock: the writer never emits cascades 2 and 3.
        VrCadence locked;
        for (std::int32_t f=0;f<12;++f) { in.frame=f;in.native=3;in.nextUpdate=next(f);in.rotation=(f&1)?turned:ahead;
            Check((locked.Plan(rules,in).mask&~3u)==0,"never a bit the native writer has not emitted"); }
        // Shadow Boost's safe mode, then incomplete cascades, then its live fit wrapper.
        VrCadence guarded;in.rotation=ahead;
        for (std::int32_t f=0;f<9;++f) { in.frame=f;in.native=3;in.nextUpdate=next(f);guarded.Plan(rules,in); }
        in.frame=9;in.native=9;in.nextUpdate=9;in.rotation=turned;in.allowed=0b0011;
        Check((guarded.Plan(rules,in).added&~3u)==0,"an incomplete cascade gets no bit");
        in.frame=12;in.nextUpdate=12;in.allowed=0xF;in.wrapperLive=true;in.rotation=ahead;
        Check(guarded.Plan(rules,in).added==0,"nothing while Shadow Boost's fit wrapper is live");
        in.wrapperLive=false;
        // A teleport forces an update frame, at most once per period.
        VrCadence jump;in.rotation=ahead;in.eye={0,0,0};
        for (std::int32_t f=0;f<12;++f) { in.frame=f;in.native=table[f&3];in.nextUpdate=next(f);jump.Plan(rules,in); }
        in.eye={10000,0,0};in.frame=13;in.native=5;in.nextUpdate=15;
        plan=jump.Plan(rules,in);
        Check(plan.forceUpdate && plan.added==0b1010,"a teleport refits now");
        in.frame=14;in.native=3;in.eye={20000,0,0};
        Check(!jump.Plan(rules,in).forceUpdate,"one forced update per period");
        rules.enabled=false;
        Check(jump.Plan(rules,in).mask==in.native,"switched off, the native mask passes unchanged");
    }
    int cameraA{},cameraB{},reads{};
    const Vec3 snapshot{1,2,3}, live{4,5,6};
    auto reader=[&reads,live](const int*) { ++reads;return live; };
    Check(Exact(PointLightPosition(LightSnapshot,&cameraA,&cameraA,snapshot,reader),snapshot) && reads==0,
        "same-camera snapshot avoids native position read");
    Check(Exact(PointLightPosition(LightSnapshot,&cameraA,&cameraB,snapshot,reader),live) && reads==1,
        "changed camera uses live position");
    Check(Exact(PointLightPosition(0,&cameraA,&cameraA,snapshot,reader),live) && reads==2,
        "disabled snapshot reads live position");
    std::cout << checks << " optimization/property checks passed\n";
}
