// Private, bounded visual diagnostics. Included only by the copied probe.
// Synchronous readback and optional pacing deliberately affect frame timing.
// These captures must never supply performance measurements.
#pragma once
#include "EngineProfiles.h"
#include "BenchmarkCadence.h"
#include <array>
#include <cmath>
#include <fstream>
#include <mutex>
#include <sstream>

namespace fps_temporal {
using CameraGetter = RE::NiCamera* (*)();
using CadenceReader = bool (*)(std::uint32_t, fps::BenchmarkCadence*);
inline CameraGetter cameraGetter{};
inline CadenceReader cadenceReader{};
inline std::mutex mutex;
inline std::atomic_bool active{};
inline std::uint64_t worldCounter{}, requestId{}, duplicates{}, lastCapturedWorld{};
inline std::uint32_t expected{}, written{}, paceMs{}, renderThread{};
inline std::uint64_t startMs{}, outputBytes{};
inline bool inWorld{}, ready{}, awaitingPresent{}, succeeded{};
inline std::string error;
inline std::filesystem::path directory;
inline std::ofstream journal;

inline std::uint64_t Qpc() { LARGE_INTEGER v{}; QueryPerformanceCounter(&v); return v.QuadPart; }
inline std::uint64_t Frequency() { LARGE_INTEGER v{}; QueryPerformanceFrequency(&v); return v.QuadPart; }
template<std::size_t N> std::string Array(const std::array<float,N>& values) {
    std::string result="[";
    for (std::size_t i=0;i<N;++i) result+=(i?",":"")+fmt::format("{}",values[i]);
    return result+"]";
}
struct Camera {
    std::array<float,3> eye{};
    std::array<float,9> rotation{};
    std::array<float,16> worldToCam{};
    std::array<float,6> frustum{};
    bool valid{};
};
inline Camera ReadCamera() {
    Camera result;
    const auto* c=cameraGetter?cameraGetter():nullptr;
    if (!c) return result;
    result.eye={c->world.translate.x,c->world.translate.y,c->world.translate.z};
    for (int i=0;i<3;++i) {
        const auto& row=c->world.rotate.entry[i];
        result.rotation[i*3]=row.x;result.rotation[i*3+1]=row.y;result.rotation[i*3+2]=row.z;
    }
    std::memcpy(result.worldToCam.data(),c->worldToCam,sizeof(c->worldToCam));
    const auto& f=c->viewFrustum;
    // Windows SDK defines near/far macros after CommonLib's declarations.
    // The six contiguous scalar planes precede the ortho byte in both runtimes.
    static_assert(offsetof(RE::NiFrustum,ortho)==6*sizeof(float));
    std::memcpy(result.frustum.data(),&f,sizeof(result.frustum));
    result.valid=!f.ortho;
    for(const auto value:result.eye)result.valid=result.valid && std::isfinite(value);
    for(const auto value:result.rotation)result.valid=result.valid && std::isfinite(value);
    for(const auto value:result.worldToCam)result.valid=result.valid && std::isfinite(value);
    for(const auto value:result.frustum)result.valid=result.valid && std::isfinite(value);
    return result;
}
inline std::string Json(const Camera& c) {
    return "{\"eye\":"+Array(c.eye)+",\"rotation\":"+Array(c.rotation)+
        ",\"world_to_cam\":"+Array(c.worldToCam)+",\"frustum\":"+Array(c.frustum)+"}";
}
struct Frame {
    std::uint64_t world{}, begin{}, end{}, captureBegin{}, captureEnd{}, forward{}, present{};
    Camera before, after;
    fps::BenchmarkCadence cadence{};
    bool cadenceKnown{};
    dtl_probe::Snapshot image;
    std::uint64_t pacingMs{};
};
inline Frame frame;

inline void Finish(bool ok,const std::string& why={}) {
    succeeded=ok;error=why;
    if(journal.is_open()) {
        // Errors are exposed by StatusJson; keep this terminal row fixed-format.
        journal << fmt::format("{{\"type\":\"terminal\",\"success\":{},\"written\":{},\"expected\":{},\"duplicate_presents\":{}}}\n",
            ok,written,expected,duplicates);
        journal.flush();
        if(!journal){succeeded=false;error="Sequence journal write failed";}
        journal.close();
    }
    active.store(false,std::memory_order_release);
    inWorld=ready=awaitingPresent=false;
}
inline void Initialize() {
    const auto version=REL::Module::get().version();
    const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    for(const auto& profile:fps::profiles) {
        if(version!=REL::Version(profile.version[0],profile.version[1],profile.version[2],profile.version[3]))continue;
        if(std::memcmp(reinterpret_cast<void*>(base+profile.camera),profile.cameraPrefix.data(),profile.cameraPrefix.size()))
            throw std::runtime_error("Temporal camera getter differs from verified engine");
        cameraGetter=reinterpret_cast<CameraGetter>(base+profile.camera);return;
    }
    throw std::runtime_error("Temporal camera getter has no runtime profile");
}
inline void BeginWorld() noexcept {
    if(!active.load(std::memory_order_acquire))return;
    try {
        std::lock_guard lock(mutex);
        if(!active.load())return;
        if(inWorld || ready || awaitingPresent){Finish(false,"World frame was not paired with one Present");return;}
        if(renderThread && renderThread!=GetCurrentThreadId()){Finish(false,"World render thread changed");return;}
        renderThread=GetCurrentThreadId();
        frame={};frame.world=++worldCounter;frame.begin=Qpc();frame.before=ReadCamera();
        if(!frame.before.valid){Finish(false,"Invalid camera at world begin");return;}
        inWorld=true;
    } catch(...) { std::lock_guard lock(mutex);Finish(false,"World-begin capture failed"); }
}
inline void EndWorld() noexcept {
    if(!active.load(std::memory_order_acquire))return;
    try {
        std::lock_guard lock(mutex);
        if(!active.load())return;
        // A UI request can arrive after this world's BeginWorld. Start at the next complete boundary.
        if(!inWorld)return;
        if(renderThread!=GetCurrentThreadId()){Finish(false,"World-end thread changed");return;}
        frame.end=Qpc();frame.after=ReadCamera();
        if(!frame.after.valid){Finish(false,"Invalid camera at world end");return;}
        frame.cadenceKnown=cadenceReader && cadenceReader(sizeof(frame.cadence),&frame.cadence);
        inWorld=false;ready=true;
    } catch(...) { std::lock_guard lock(mutex);Finish(false,"World-end capture failed"); }
}
inline void BeforePresent(IDXGISwapChain* swap) noexcept {
    if(!active.load(std::memory_order_acquire))return;
    try {
        std::lock_guard lock(mutex);
        if(!active.load())return;
        if(GetTickCount64()-startMs>30000){Finish(false,"Sequence exceeded 30 seconds");return;}
        if(!ready){++duplicates;return;}
        if(renderThread!=GetCurrentThreadId() || inWorld || awaitingPresent){Finish(false,"Unpaired Present");return;}
        if(lastCapturedWorld && frame.world!=lastCapturedWorld+1){Finish(false,"World frame gap");return;}
        Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
        if(FAILED(swap->GetBuffer(0,IID_PPV_ARGS(back.GetAddressOf())))){Finish(false,"Backbuffer unavailable");return;}
        D3D11_TEXTURE2D_DESC desc{};back->GetDesc(&desc);
        const auto bytes=std::uint64_t(desc.Width)*desc.Height*3;
        if(!bytes || bytes>16ull*1024*1024 || outputBytes+bytes>384ull*1024*1024){Finish(false,"Sequence byte limit");return;}
        frame.present=dtl_probe::detail::presents.load();
        frame.image.requestId=requestId;
        frame.image.capturePath=(directory/fmt::format("frame-{:03}.ppm",written)).wstring();
        frame.captureBegin=Qpc();dtl_probe::detail::Capture(swap,frame.image);frame.captureEnd=Qpc();
        if(frame.image.captureState!=dtl_probe::CaptureState::Succeeded || frame.image.captureAllZero){
            Finish(false,"Frame readback failed or blank: "+frame.image.captureError);return;
        }
        outputBytes+=bytes;
        const auto elapsed=(Qpc()-frame.begin)*1000/Frequency();
        if(paceMs>elapsed){frame.pacingMs=paceMs-elapsed;Sleep(static_cast<DWORD>(frame.pacingMs));}
        frame.forward=Qpc();ready=false;awaitingPresent=true;
    } catch(...) { std::lock_guard lock(mutex);Finish(false,"Frame capture failed"); }
}
inline void AfterPresent(IDXGISwapChain*,HRESULT hr) noexcept {
    if(!active.load(std::memory_order_acquire))return;
    try {
        std::lock_guard lock(mutex);
        if(!active.load() || !awaitingPresent)return;
        if(hr!=S_OK && hr!=DXGI_STATUS_OCCLUDED){Finish(false,"Present failed");return;}
        const auto returned=Qpc();
        journal << fmt::format("{{\"type\":\"frame\",\"index\":{},\"world_id\":{},\"present_id\":{},"
            "\"world_begin_qpc\":{},\"world_end_qpc\":{},\"capture_begin_qpc\":{},\"capture_end_qpc\":{},"
            "\"forward_qpc\":{},\"returned_qpc\":{},\"pacing_ms\":{},\"hresult\":{},"
            "\"file\":\"frame-{:03}.ppm\",\"width\":{},\"height\":{},\"all_zero\":false,"
            "\"camera_begin\":{},\"camera_end\":{},\"cadence_known\":{},\"enabled\":{},\"cadence\":{},\"planned_skip_mask\":{}}}\n",
            written,frame.world,frame.present,frame.begin,frame.end,frame.captureBegin,frame.captureEnd,
            frame.forward,returned,frame.pacingMs,static_cast<long>(hr),written,frame.image.captureWidth,frame.image.captureHeight,
            Json(frame.before),Json(frame.after),frame.cadenceKnown,frame.cadence.enabled,frame.cadence.cadence,frame.cadence.plannedSkipMask);
        journal.flush();
        if(!journal){Finish(false,"Frame journal write failed");return;}
        lastCapturedWorld=frame.world;++written;awaitingPresent=false;
        if(written==expected)Finish(true);
    } catch(...) { std::lock_guard lock(mutex);Finish(false,"Present receipt failed"); }
}
inline void Request(const std::filesystem::path& root,std::string_view argument) {
    std::istringstream input{std::string(argument)};
    std::string name,extra;unsigned count{},pacing{};
    if(!(input>>name>>count>>pacing) || input>>extra || name.empty() || name.size()>80 ||
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos ||
        count<8 || count>48 || (pacing!=0 && pacing!=33 && pacing!=50))
        throw std::runtime_error("Sequence requires simple-name frames[8..48] pacing-ms[0|33|50]");
    std::lock_guard lock(mutex);
    if(active.load())throw std::runtime_error("Sequence already active");
    if(!cameraGetter)Initialize();
    const auto single=dtl_probe::GetSnapshot();
    if(single.captureState==dtl_probe::CaptureState::Pending || single.captureState==dtl_probe::CaptureState::Capturing ||
        dtl_probe::rendered::controller.Requested())throw std::runtime_error("Capture/timing already active");
    const auto gpu=dtl_probe::gpu_timing::controller.GetSnapshot();
    if(gpu.requested || gpu.pending)throw std::runtime_error("GPU timing must be inactive");
    const auto target=root/name;
    if(!std::filesystem::create_directory(target))throw std::runtime_error("Sequence output must be fresh");
    directory=target;journal.clear();journal.open(target/"frames.jsonl",std::ios::out|std::ios::trunc);
    if(!journal)throw std::runtime_error("Cannot create sequence journal");
    cadenceReader=reinterpret_cast<CadenceReader>(GetProcAddress(GetModuleHandleW(L"FasterShadows.dll"),"FPSBenchmarkReadCadence"));
    ++requestId;expected=count;paceMs=pacing;written=renderThread=0;
    duplicates=lastCapturedWorld=outputBytes=0;inWorld=ready=awaitingPresent=succeeded=false;error.clear();
    journal << fmt::format("{{\"type\":\"metadata\",\"schema\":1,\"request_id\":{},\"expected\":{},"
        "\"qpc_frequency\":{},\"pacing_ms\":{},\"performance_valid\":false,\"capture\":\"synchronous_backbuffer_readback\"}}\n",
        requestId,expected,Frequency(),paceMs);
    journal.flush();if(!journal){journal.close();throw std::runtime_error("Cannot write sequence metadata");}
    dtl_probe::SetSequenceCallbacks(&BeforePresent,&AfterPresent);
    startMs=GetTickCount64();active.store(true,std::memory_order_release);
}
inline std::string StatusJson() {
    std::lock_guard lock(mutex);
    // Escape error text without relying on a game/UI utility.
    std::string escaped;for(char c:error){if(c=='"' || c=='\\')escaped+='\\';escaped+=c<' '?' ':c;}
    return fmt::format("{{\"request_id\":{},\"active\":{},\"success\":{},\"written\":{},\"expected\":{},\"error\":\"{}\"}}",
        requestId,active.load(),succeeded,written,expected,escaped);
}
inline void RequireIdle() {
    if(active.load(std::memory_order_acquire))throw std::runtime_error("Temporal capture must finish before other capture/timing operations");
}
}
