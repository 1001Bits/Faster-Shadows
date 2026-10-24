// CPU microbenchmark of actual policy/cache helpers, not game FPS or native draw time.
#include "ShadowOptimizations.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <vector>
#include <random>
#include <iomanip>
#include <cstdlib>
using namespace fps;
struct Geometry { Sphere bound;std::uintptr_t identity; };
enum BenchmarkCounter : std::size_t {
    benchTested,benchSmall,benchVolume,benchHits,benchMisses,benchPointRemoved,benchTotalRemoved,benchCounterCount
};
ShardedCounters<benchCounterCount> counters;
std::atomic<std::uint64_t> sink{0};
volatile float lightCoordinates[3]={500,0,100};
Vec3 ReadLight() { return {lightCoordinates[0],lightCoordinates[1],lightCoordinates[2]}; }
struct Result { double ns;std::uint64_t candidates,volumeCandidates,hits,reads; };
Result Run(const std::vector<Geometry>& geometry,std::uint32_t mask,int repeats,bool diagnostics) {
    Rule rule{true,1000,150,.035f,0};PreparedPointRule prepared(rule);
    constexpr float reach=600.0f;  // typical lamp radius: exercises the SPS shadow-size scale
    DecisionCache cache;
    const float matrix[4][4]={{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,1,0}};
    const auto view=WideReceiverFrustum(matrix);
    std::uint64_t candidates=0,volumeCandidates=0,hits=0,reads=0;
    // Production shards persist across traversals. Exclude first-use allocation and
    // reporting from the steady-state policy timing, then verify every sampled counter.
    counters.Add(benchTested);
    for (std::size_t i=0;i<benchCounterCount;++i) counters.Sample(i,true);
    const auto start=std::chrono::steady_clock::now();
    for (int repetition=0;repetition<repeats;++repetition) {
        const auto traversal=(mask & DecisionMemo) ? cache.Begin() : 0;
        const auto snapshot=ReadLight();++reads;
        std::uint64_t batchTested=0,batchSmall=0,batchVolume=0,batchHits=0,batchMisses=0,batchRemoved=0;
        for (const auto& g:geometry) {
            if (diagnostics) {
                if (mask & BatchCounters) ++batchTested;else counters.Add(benchTested);
            }
            int camera{};
            const auto light=PointLightPosition(mask,&camera,&camera,snapshot,[](const int*) { return ReadLight(); });
            if (!(mask & LightSnapshot)) ++reads;
            const DecisionKey key{g.identity,g.bound,light};std::uint8_t decision=kKeepCaster;
            if ((mask & DecisionMemo) && cache.Find(traversal,key,decision)) {
                ++hits;
                if (diagnostics) ++batchHits;
            }
            else {
                if (diagnostics && (mask & DecisionMemo)) ++batchMisses;
                if ((mask & SquaredDistance) ? CullPointSquared(prepared,g.bound,{},light,reach) : CullPoint(rule,g.bound,{},light,reach)) decision=kSmallCaster;
                else if ((mask & ShadowVolume) && CullPointShadowVolume(view,g.bound,light)) decision=kVolumeOnly;
                if (mask & DecisionMemo) cache.Put(traversal,key,decision);
            }
            if (decision==kSmallCaster) {
                ++candidates;
                if (diagnostics) {
                    if (mask & BatchCounters) ++batchSmall;else counters.Add(benchSmall);
                }
            } else if (decision==kVolumeOnly) {
                ++volumeCandidates;
                if (diagnostics) ++batchVolume;
            }
            // Match the live-removal point path: volume analysis never removes a caster.
            if (diagnostics && RemovePointDecision(decision,false)) ++batchRemoved;
        }
        if (mask & BatchCounters) { counters.Add(benchTested,batchTested);counters.Add(benchSmall,batchSmall); }
        counters.Add(benchVolume,batchVolume);
        counters.Add(benchHits,batchHits);
        counters.Add(benchMisses,batchMisses);
        counters.Add(benchPointRemoved,batchRemoved);
        counters.Add(benchTotalRemoved,batchRemoved);
    }
    const auto stop=std::chrono::steady_clock::now();
    const std::uint64_t policyTests=geometry.size()*static_cast<std::uint64_t>(repeats);
    const std::array<std::uint64_t,benchCounterCount> expected{
        policyTests,candidates,volumeCandidates,hits,(mask & DecisionMemo) ? policyTests-hits : 0,candidates,candidates};
    for (std::size_t i=0;i<benchCounterCount;++i) {
        if (counters.Sample(i,true)!=(diagnostics ? expected[i] : 0)) {
            std::cerr << "Counter accounting mismatch at " << i << '\n';std::exit(1);
        }
    }
    sink.fetch_add(candidates+volumeCandidates+hits+reads,std::memory_order_relaxed);
    return {std::chrono::duration<double,std::nano>(stop-start).count()/policyTests,candidates,volumeCandidates,hits,reads};
}
int main() {
    std::mt19937 random(42);std::uniform_real_distribution<float> coordinate(-6000,6000),radius(5,250);
    const std::array<std::uint32_t,13> masks{0,1,2,4,8,16,14,31,30,29,27,23,15};
    std::cout << std::setprecision(10) << "{\"metric\":\"cpu_nanoseconds_per_policy_caster\",\"game_fps\":false,\"native_submission_cost_included\":false,\"counter_implementation\":\"sharded\",\"candidate_count_includes_volume\":false,\"samples\":[";
    bool comma=false;
    for (int scenario=0;scenario<3;++scenario) {
        std::vector<Geometry> geometry;
        for (std::uintptr_t i=0;i<2048;++i) {
            Geometry g{{{coordinate(random),coordinate(random),coordinate(random)+3000},radius(random)},0x10000+i*64};
            const int duplicates=scenario==1 ? 8 : 1;
            if (scenario==2) g.bound.radius=200; // large/precombined caster workload
            for (int j=0;j<duplicates;++j) geometry.push_back(g);
        }
        for (bool diagnostics:{false,true}) for (int sample=0;sample<9;++sample) for (std::size_t k=0;k<masks.size();++k) {
            // Rotate order across repeats to avoid always measuring baseline cold.
            const auto mask=masks[(k+sample)%masks.size()];
            const auto result=Run(geometry,mask,12,diagnostics);
            if (comma) std::cout << ',';comma=true;
            std::cout << "{\"scenario\":\"" << (scenario==0 ? "unique" : scenario==1 ? "repeated" : "large_precombined")
                << "\",\"diagnostics\":" << (diagnostics ? "true" : "false") << ",\"mask\":" << mask << ",\"sample\":" << sample << ",\"ns_per_caster\":" << result.ns
                << ",\"policy_tests\":" << geometry.size()*12 << ",\"candidates\":" << result.candidates
                << ",\"volume_candidates\":" << result.volumeCandidates
                << ",\"memo_hits\":" << result.hits << ",\"light_reads\":" << result.reads << '}';
        }
    }
    std::cout << "],\"sink\":" << sink.load() << "}\n";
}
