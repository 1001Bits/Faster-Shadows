"""Copy existing private diagnostics into this workspace; never deploy to games."""
import hashlib
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(r'C:\Development\Texture Downscaler FO4\tools\benchmark')
SUPPORT = ROOT/'.runtime-tests/support'


def temporal_support(text):
    """Patch only the owned copy; exact anchors reject upstream layout changes."""
    capture=SUPPORT/'probe-common/PresentCapture.h'
    source=capture.read_text()
    edits=[
        ('inline std::atomic<AfterPresentCallback> afterPresentCallback{};',
         'inline std::atomic<AfterPresentCallback> afterPresentCallback{};\n'
         '        inline std::atomic<FrameCallback> sequenceCallback{};\n'
         '        inline std::atomic<AfterPresentCallback> sequenceAfterCallback{};'),
        ('if (const auto callback = beforePresentCallback.load(std::memory_order_acquire)) callback(swap);',
         'if (const auto callback = beforePresentCallback.load(std::memory_order_acquire)) callback(swap);\n'
         '                if (const auto callback = sequenceCallback.load(std::memory_order_acquire)) callback(swap);'),
        ('if (const auto callback = afterPresentCallback.load(std::memory_order_acquire)) callback(swap,hr);',
         'if (const auto callback = sequenceAfterCallback.load(std::memory_order_acquire)) callback(swap,hr);\n'
         '                if (const auto callback = afterPresentCallback.load(std::memory_order_acquire)) callback(swap,hr);'),
        ('    inline void SetTimingCallbacks(',
         '    inline void SetSequenceCallbacks(FrameCallback before, AfterPresentCallback after) {\n'
         '        detail::sequenceCallback.store(before,std::memory_order_release);\n'
         '        detail::sequenceAfterCallback.store(after,std::memory_order_release);\n'
         '    }\n\n    inline void SetTimingCallbacks('),
    ]
    for old,new in edits:
        assert source.count(old)==1,old
        source=source.replace(old,new)
    capture.write_text(source)
    shutil.copy2(ROOT/'tools/benchmark/TemporalCapture.h',SUPPORT/'probe-common/TemporalCapture.h')
    edits=[
        ('#include "../probe-common/RenderedThroughput.h"',
         '#include "../probe-common/RenderedThroughput.h"\n#include "../probe-common/TemporalCapture.h"'),
        ('        dtl_probe::rendered::BeginWorld();',
         '        fps_temporal::BeginWorld();\n        dtl_probe::rendered::BeginWorld();'),
        ('        dtl_probe::rendered::EndWorld();',
         '        dtl_probe::rendered::EndWorld();\n        fps_temporal::EndWorld();'),
        ('                } else if (command.starts_with("@capture ")) {',
         '                } else if (command.starts_with("@sequence ")) {\n'
         '                    if(!g_worldHookInstalled)throw std::runtime_error("Sequence requires verified world boundaries");\n'
         '                    InstallPresentProbe();\n'
         '                    fps_temporal::Request(*g_directory,std::string_view(command).substr(10));\n'
         '                } else if (command.starts_with("@capture ")) {'),
        ('dtl_probe::rendered::StatusJson();',
         'dtl_probe::rendered::StatusJson() + ",\\\"sequence\\\":" + fps_temporal::StatusJson();'),
    ]
    for operation in ('Capture','Pressure','GpuTiming','Throughput'):
        old=f'    void {operation}(std::string_view argument)\n    {{'
        edits.append((old,old+'\n        fps_temporal::RequireIdle();'))
    for old,new in edits:
        assert text.count(old)==1,old
        text=text.replace(old,new)
    return text


def placement_support():
    """Per world frame: the render thread's core and on-CPU cycles at both boundaries.

    Separates a slower core (efficiency core, lower clock) from preemption or waiting,
    which identical work at different speeds cannot otherwise distinguish."""
    path=SUPPORT/'probe-common/RenderedThroughput.h'
    source=path.read_text()
    edits=[
        ('inline std::uint64_t Qpc() {LARGE_INTEGER value{};QueryPerformanceCounter(&value);return value.QuadPart;}',
         'inline std::uint64_t Qpc() {LARGE_INTEGER value{};QueryPerformanceCounter(&value);return value.QuadPart;}\n'
         '    inline void Placement(std::uint64_t& cycles,std::uint32_t& cpu) {\n'
         '        ULONG64 value{};QueryThreadCycleTime(GetCurrentThread(),&value);cycles=value;cpu=GetCurrentProcessorNumber();\n'
         '    }'),
        ('std::uint64_t window{},id{},presentId{},beginQpc{},worldEndQpc{},endQpc{},readQpc{},beginTick{},endTick{};',
         'std::uint64_t window{},id{},presentId{},beginQpc{},worldEndQpc{},endQpc{},readQpc{},beginTick{},endTick{};\n'
         '        std::uint64_t beginCycles{},worldEndCycles{};std::uint32_t beginCpu{},worldEndCpu{};'),
        ('slot.frame.beginQpc=Qpc();slot.submittedMs=now;',
         'slot.frame.beginQpc=Qpc();Placement(slot.frame.beginCycles,slot.frame.beginCpu);slot.submittedMs=now;'),
        ('slot.frame.worldEndQpc=Qpc();slot.frame.boundaryPaired=paired;',
         'slot.frame.worldEndQpc=Qpc();Placement(slot.frame.worldEndCycles,slot.frame.worldEndCpu);slot.frame.boundaryPaired=paired;'),
        (r'<<",\"read_qpc\":"<<f.readQpc<<",\"begin_tick\":"',
         r'<<",\"read_qpc\":"<<f.readQpc<<",\"begin_cpu\":"<<f.beginCpu<<",\"world_end_cpu\":"<<f.worldEndCpu'
         r'<<",\"begin_cycles\":"<<f.beginCycles<<",\"world_end_cycles\":"<<f.worldEndCycles<<",\"begin_tick\":"'),
    ]
    for old,new in edits:
        assert source.count(old)==1,old
        source=source.replace(old,new)
    path.write_text(source)


def coexist_support():
    """Untimed runs (Slot exclusive=False, DTL_BENCH_COEXIST=1) launch alongside shared reservations,
    which guard exclusive timing work. Local holds still block. Patched here, not in the source project."""
    path=SUPPORT/'launch_coordination.py';source=path.read_text()
    edits=[
        ("OWNED_LEASE_ENV = 'DTL_BENCH_OWNED_LEASE'\n",
         "OWNED_LEASE_ENV = 'DTL_BENCH_OWNED_LEASE'\n"
         "# Set by a non-exclusive (untimed) slot: shared-machine reservations guard exclusive\n"
         "# timing work, so an untimed visual run launches alongside them. Local holds still apply.\n"
         "COEXIST_ENV = 'DTL_BENCH_COEXIST'\n"),
        ("        block('Local launch hold exists: ' + str(hold))\n\n    try:\n        owned = _owned_lease()",
         "        block('Local launch hold exists: ' + str(hold))\n\n"
         "    if os.environ.get(COEXIST_ENV) == '1':\n"
         "        if os.environ.get(OWNED_LEASE_ENV) is not None:\n"
         "            block('An untimed run cannot also own the shared lease')\n"
         "        result['coexist'] = True\n"
         "        return result\n\n"
         "    try:\n        owned = _owned_lease()"),
    ]
    for old,new in edits:
        assert source.count(old)==1,old
        source=source.replace(old,new)
    path.write_text(source)


def main():
    SUPPORT.mkdir(parents=True,exist_ok=True)
    for name in ['private_desktop.py','launch_coordination.py','gpu_timing.py','rendered_throughput.py']:
        shutil.copy2(SOURCE/name,SUPPORT/name)
    coexist_support()
    for name in ['private-loader/main.cpp','private-loader/remote.asm','private-loader/input-guard.h',
                 'private-loader/build/DTLBenchInputGuard.dll','probe-fo4/Probe.cpp',
                 'probe-common/PresentCapture.h','probe-common/GpuTiming.h',
                 'probe-common/MemoryPressure.h','probe-common/RenderedThroughput.h']:
        target=SUPPORT/name
        target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(SOURCE/name,target)
    placement_support()
    launcher=SUPPORT/'private-loader/main.cpp'
    text=launcher.read_text()
    old='if (!Inject(child.info.hProcess, inputGuard, true, true))'
    new='''const auto folderGuard = fs::canonical(Environment(L"FPS_BENCH_FOLDER_GUARD"));
        if (folderGuard.parent_path() != game.parent_path())
            throw std::runtime_error("Folder guard must be local to the isolated game");
        if (!Inject(child.info.hProcess, folderGuard, true, true))
            throw std::runtime_error("Private folder redirection failed; game remains suspended");
        if (!Inject(child.info.hProcess, inputGuard, true, true))'''
    assert text.count(old)==1
    launcher.write_text(text.replace(old,new))
    probe=SUPPORT/'probe-fo4/Probe.cpp'
    text=probe.read_text()
    old='if (command == "@present") {'
    new='''if (command.starts_with("@fps-options ")) {
                    const auto module=GetModuleHandleW(L"FasterShadows.dll");
                    using Control=bool(*)(std::uint32_t);
                    const auto control=reinterpret_cast<Control>(GetProcAddress(module,"FPSBenchmarkSetOptimizations"));
                    if(!control || !control(static_cast<std::uint32_t>(std::stoul(command.substr(13)))))
                        throw std::runtime_error("Faster Shadows option control unavailable");
                } else if (command.starts_with("@fps ")) {
                    const auto module=GetModuleHandleW(L"FasterShadows.dll");
                    using Control=bool(*)(bool);
                    const auto control=reinterpret_cast<Control>(GetProcAddress(module,"FPSBenchmarkSetEnabled"));
                    if (!control || !control(command.substr(5)=="1")) throw std::runtime_error("FPS control unavailable");
                } else if (command.starts_with("@fps-motion ")) {
                    const auto module=GetModuleHandleW(L"FasterShadows.dll");
                    using Control=bool(*)(std::uint32_t,float,float);
                    const auto control=reinterpret_cast<Control>(GetProcAddress(module,"FPSBenchmarkSetMotion"));
                    std::uint32_t kind{};float rate{},amplitude{};
                    if(!control || std::sscanf(command.c_str()+12,"%u %f %f",&kind,&rate,&amplitude)!=3 || !control(kind,rate,amplitude))
                        throw std::runtime_error("Faster Shadows camera motion unavailable");
                } else if (command == "@fps-sun-state") {
                    const auto module=GetModuleHandleW(L"FasterShadows.dll");
                    using Version=std::uint32_t(*)();
                    const auto version=reinterpret_cast<Version>(GetProcAddress(module,"FPSBenchmarkProtocolVersion"));
                    if(!version || version()!=4)throw std::runtime_error("FPS diagnostic ABI differs");
                    struct State {std::uint64_t epoch,present,count,hookMask;float eye[3],direction[3];std::uint64_t cullers[4],cameras[4],vtables[4],append[4];};
                    using Read=bool(*)(State*);
                    const auto read=reinterpret_cast<Read>(GetProcAddress(module,"FPSBenchmarkReadSunState"));
                    State state{};
                    if(!read || !read(&state))throw std::runtime_error("FPS sun state unavailable");
                    std::string output=fmt::format("{{\\\"epoch\\\":{},\\\"present\\\":{},\\\"count\\\":{},\\\"hook_mask\\\":{},\\\"eye\\\":[{},{},{}],\\\"direction\\\":[{},{},{}],\\\"cullers\\\":[",
                        state.epoch,state.present,state.count,state.hookMask,state.eye[0],state.eye[1],state.eye[2],state.direction[0],state.direction[1],state.direction[2]);
                    for(std::size_t i=0;i<4;++i)output+=fmt::format("{}{{\\\"object\\\":{},\\\"camera\\\":{},\\\"vtable\\\":{},\\\"append\\\":{}}}",i?",":"",state.cullers[i],state.cameras[i],state.vtables[i],state.append[i]);
                    output+="]}";WriteJson("sun-state.json",output);
                } else if (command.starts_with("@fps-optimizations ")) {
                    const auto module=GetModuleHandleW(L"FasterShadows.dll");
                    using Version=std::uint32_t(*)();
                    const auto version=reinterpret_cast<Version>(GetProcAddress(module,"FPSBenchmarkProtocolVersion"));
                    if(!version || version()!=4)throw std::runtime_error("FPS diagnostic ABI differs");
                    struct Counters {std::uint64_t volume,hits,misses,removed;std::uint32_t mask,enabled,dryRun,volumeAnalysisOnly;};
                    using Read=bool(*)(Counters*,bool);
                    const auto read=reinterpret_cast<Read>(GetProcAddress(module,"FPSBenchmarkReadOptimizations"));
                    Counters counters{};
                    if(!read || !read(&counters,command.substr(19)=="reset"))throw std::runtime_error("FPS optimization counters unavailable");
                    WriteJson("optimization-counters.json",fmt::format("{{\\\"volume_candidates\\\":{},\\\"memo_hits\\\":{},\\\"memo_misses\\\":{},\\\"point_removed\\\":{},\\\"mask\\\":{},\\\"enabled\\\":{},\\\"dry_run\\\":{},\\\"volume_analysis_only\\\":{}}}",
                        counters.volume,counters.hits,counters.misses,counters.removed,counters.mask,counters.enabled,counters.dryRun,counters.volumeAnalysisOnly));
                } else if (command.starts_with("@fps-stats ")) {
                    const auto module=GetModuleHandleW(L"FasterShadows.dll");
                    struct Counters {std::uint64_t sunTested,sunCandidates,pointTested,pointCandidates,other;};
                    using Read=bool(*)(Counters*,bool);
                    const auto read=reinterpret_cast<Read>(GetProcAddress(module,"FPSBenchmarkReadCounters"));
                    Counters counters{};
                    if (!read || !read(&counters,command.substr(11)=="reset")) throw std::runtime_error("FPS counters unavailable");
                    WriteJson("shadow-counters.json",fmt::format("{{\\\"sun_tested\\\":{},\\\"sun_candidates\\\":{},\\\"point_tested\\\":{},\\\"point_candidates\\\":{},\\\"other\\\":{}}}",
                        counters.sunTested,counters.sunCandidates,counters.pointTested,counters.pointCandidates,counters.other));
                } else if (command == "@present") {'''
    assert text.count(old)==1
    text=text.replace(old,new).replace('data.IsLayoutDependent(true);','data.IsLayoutDependent(false);\n    data.addressIndependence |= 1 << 2;')
    task='g_tasks->AddTask([sequence, commands = std::move(commands)]() mutable { Run(sequence, std::move(commands)); });'
    assert text.count(task)==1
    text=text.replace(task,task.replace('AddTask','AddUITask'))
    includes='#include <vector>\n'
    assert text.count(includes)==1
    text=text.replace(includes,'#include <cstdio>\n#include <mutex>\n#include <optional>\n#include <utility>\n'+includes)
    entry='''void Run(std::uint64_t sequence, std::vector<std::string> commands)
    {'''
    assert text.count(entry)==1
    # Live 2026-10-11: a Main::threadID check failed every gameplay batch. OG calls
    # UI::ProcessMessages (F4SE's UI task runner) from Main::OnIdle on the main thread
    # in menus, from JobListManager::UIJob on a job thread in gameplay (skipped while
    # TES::bLoadingMenuOpen), and from the loading-screen thread beside a cell load.
    # The game's console runs commands in that UI context (Console::Call ->
    # Console::ExecuteCommand), so only the loading case is deferred. F4SE drains its
    # UI queue in a loop, so a task must not re-post itself; the watcher re-posts it.
    text=text.replace(entry,'''std::mutex g_retryLock;
    std::optional<std::pair<std::uint64_t, std::vector<std::string>>> g_retry;
    std::uint64_t g_retryDeadline = 0;

    bool LoadingMenuOpen()
    {
        const auto* ui = ReadSingleton<RE::UI>(g_statusSingletons.ui);
        return !ui || BorrowedMenuOpen(ui, RE::BSFixedString("LoadingMenu"));
    }

    '''+entry+'''
        // F4SE's ordinary tasks are worker-pool tasks on OG. Native console
        // commands run as UI tasks, where the game's own console runs them,
        // but never on the loading-screen thread while a cell loads.
        if(LoadingMenuOpen()) {
            const auto now=GetTickCount64();
            {
                std::scoped_lock lock(g_retryLock);
                if(!g_retryDeadline) g_retryDeadline=now+40000;  // below the runner's 45 s command timeout
                if(now<g_retryDeadline) {
                    g_retry.emplace(sequence,std::move(commands));
                    g_pending.store(false,std::memory_order_release);
                    return;
                }
                g_retryDeadline=0;
            }
            WriteJson(std::to_string(sequence)+".ack.json","{\\\"error\\\":\\\"loading menu still open after 40 s\\\"}");
            g_pending.store(false,std::memory_order_release);
            return;
        }
        {
            std::scoped_lock lock(g_retryLock);
            g_retryDeadline=0;
        }''')
    poll='            if (g_pending.load(std::memory_order_acquire)) continue;\n'
    assert text.count(poll)==1
    text=text.replace(poll,poll+'''            {
                std::unique_lock lock(g_retryLock);
                if (g_retry) {
                    auto batch = std::move(*g_retry);
                    g_retry.reset();
                    lock.unlock();
                    g_pending.store(true, std::memory_order_release);
                    g_tasks->AddUITask([sequence = batch.first, commands = std::move(batch.second)]() mutable {
                        Run(sequence, std::move(commands));
                    });
                    continue;
                }
            }
''')
    probe.write_text(temporal_support(text))
    print('Private diagnostics copied and adapted inside',SUPPORT)


if __name__=='__main__':
    main()
