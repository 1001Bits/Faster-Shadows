// Private lab only: process-local folder redirection before F4SE/game entry.
// No system folder settings, registry keys or installed files are changed.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <ShlObj.h>
#include <detours.h>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
using Known=HRESULT(WINAPI*)(REFKNOWNFOLDERID,DWORD,HANDLE,PWSTR*);
using FolderW=HRESULT(WINAPI*)(HWND,int,HANDLE,DWORD,LPWSTR);
using FolderA=HRESULT(WINAPI*)(HWND,int,HANDLE,DWORD,LPSTR);
Known originalKnown=SHGetKnownFolderPath;
FolderW originalW=SHGetFolderPathW;
FolderA originalA=SHGetFolderPathA;
std::wstring documents,local,roaming,profile,saved;
wchar_t exceptionLog[32768]{};
wchar_t exitLog[32768]{};
using Exit=void(WINAPI*)(UINT);
Exit originalExit=ExitProcess;
using Terminate=LONG(NTAPI*)(HANDLE,LONG);
Terminate originalTerminate{};
void WriteExit(UINT code) {
    const auto file=CreateFileW(exitLog,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file!=INVALID_HANDLE_VALUE) {
        void* frames[24]{};
        const auto count=CaptureStackBackTrace(0,24,frames,nullptr);
        char text[8192]{};
        int length=_snprintf_s(text,sizeof(text),_TRUNCATE,"{\"exit_code\":%u,\"frames\":[",code);
        for(USHORT i=0;i<count && length>0 && length<7500;++i) {
            HMODULE module{};wchar_t name[2048]{};
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(frames[i]),&module);
            if(module)GetModuleFileNameW(module,name,2048);
            const wchar_t* leaf=wcsrchr(name,L'\\');leaf=leaf?leaf+1:name;
            char ascii[256]{};WideCharToMultiByte(CP_UTF8,0,leaf,-1,ascii,256,nullptr,nullptr);
            length+=_snprintf_s(text+length,sizeof(text)-length,_TRUNCATE,
                "%s{\"address\":%llu,\"module\":\"%s\",\"rva\":%llu}",i?",":"",
                reinterpret_cast<unsigned long long>(frames[i]),ascii,
                reinterpret_cast<unsigned long long>(frames[i])-reinterpret_cast<unsigned long long>(module));
        }
        if(length>0 && length<8000) {
            length+=_snprintf_s(text+length,sizeof(text)-length,_TRUNCATE,"]}\n");
            DWORD written{};WriteFile(file,text,length,&written,nullptr);
        }
        CloseHandle(file);
    }
}
void WINAPI RecordExit(UINT code) {
    WriteExit(code);
    // Observation only. Preserve the native termination and exit code.
    originalExit(code);
}
LONG NTAPI RecordTerminate(HANDLE process,LONG code) {
    if(!process || process==GetCurrentProcess() || GetProcessId(process)==GetCurrentProcessId())WriteExit(static_cast<UINT>(code));
    return originalTerminate(process,code);
}
LONG CALLBACK RecordAccessViolation(EXCEPTION_POINTERS* pointers) noexcept {
    if(!pointers || pointers->ExceptionRecord->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION)return EXCEPTION_CONTINUE_SEARCH;
    const auto file=CreateFileW(exceptionLog,FILE_APPEND_DATA,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return EXCEPTION_CONTINUE_SEARCH;
    char text[8192]{};const auto* c=pointers->ContextRecord;const auto* e=pointers->ExceptionRecord;
    int length=_snprintf_s(text,sizeof(text),_TRUNCATE,
        "{\"rip\":%llu,\"image_base\":%llu,\"rsp\":%llu,\"rax\":%llu,\"rbx\":%llu,\"rcx\":%llu,\"rdx\":%llu,\"rsi\":%llu,\"rdi\":%llu,\"r8\":%llu,\"r9\":%llu,\"access_kind\":%llu,\"access_address\":%llu,\"stack\":[",
        c->Rip,reinterpret_cast<unsigned long long>(GetModuleHandleW(nullptr)),c->Rsp,c->Rax,c->Rbx,c->Rcx,c->Rdx,c->Rsi,c->Rdi,c->R8,c->R9,
        e->ExceptionInformation[0],e->ExceptionInformation[1]);
    unsigned long long stack[128]{};SIZE_T bytes{};
    if(ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(c->Rsp),stack,sizeof(stack),&bytes)) {
        for(SIZE_T i=0;i<bytes/8 && length>0 && length<7000;++i)
            length+=_snprintf_s(text+length,sizeof(text)-length,_TRUNCATE,"%s%llu",i?",":"",stack[i]);
    }
    if(length>0 && length<8000) {
        length+=_snprintf_s(text+length,sizeof(text)-length,_TRUNCATE,"]}\n");
        DWORD written{};WriteFile(file,text,length,&written,nullptr);
    }
    CloseHandle(file);return EXCEPTION_CONTINUE_SEARCH;
}

const std::wstring* Path(int csidl) noexcept {
    switch(csidl&0xFF) {
    case CSIDL_PERSONAL: return &documents;
    case CSIDL_LOCAL_APPDATA: return &local;
    case CSIDL_APPDATA: return &roaming;
    case CSIDL_PROFILE: return &profile;
    default: return nullptr;
    }
}
HRESULT WINAPI RedirectKnown(REFKNOWNFOLDERID id,DWORD flags,HANDLE token,PWSTR* out) {
    const std::wstring* path=nullptr;
    if(IsEqualGUID(id,FOLDERID_Documents))path=&documents;
    else if(IsEqualGUID(id,FOLDERID_LocalAppData))path=&local;
    else if(IsEqualGUID(id,FOLDERID_RoamingAppData))path=&roaming;
    else if(IsEqualGUID(id,FOLDERID_Profile))path=&profile;
    else if(IsEqualGUID(id,FOLDERID_SavedGames))path=&saved;
    if(!path)return originalKnown(id,flags,token,out);
    if(!out)return E_POINTER;
    *out=static_cast<PWSTR>(CoTaskMemAlloc((path->size()+1)*sizeof(wchar_t)));
    if(!*out)return E_OUTOFMEMORY;
    std::memcpy(*out,path->c_str(),(path->size()+1)*sizeof(wchar_t));
    return S_OK;
}
HRESULT WINAPI RedirectW(HWND window,int csidl,HANDLE token,DWORD flags,LPWSTR out) {
    if(const auto* path=Path(csidl)) {
        if(!out || path->size()>=MAX_PATH)return E_INVALIDARG;
        wcscpy_s(out,MAX_PATH,path->c_str());return S_OK;
    }
    return originalW(window,csidl,token,flags,out);
}
HRESULT WINAPI RedirectA(HWND window,int csidl,HANDLE token,DWORD flags,LPSTR out) {
    if(const auto* path=Path(csidl)) {
        if(!out)return E_POINTER;
        return WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,path->c_str(),-1,out,MAX_PATH,nullptr,nullptr)>0?S_OK:E_INVALIDARG;
    }
    return originalA(window,csidl,token,flags,out);
}
}
extern "C" __declspec(dllexport) BOOL Initialize() noexcept {
    try {
        wchar_t folder[2048]{};
        const auto length=GetEnvironmentVariableW(L"FPS_BENCH_USER_ROOT",folder,2048);
        if(!length || length>=2048)return FALSE;
        const auto root=std::filesystem::canonical(folder);
        if(!root.is_absolute() || root.native().size()>180)return FALSE;
        profile=root.wstring();documents=(root/L"Documents").wstring();
        local=(root/L"AppData/Local").wstring();roaming=(root/L"AppData/Roaming").wstring();saved=(root/L"Saved Games").wstring();
        for(const auto* path:{&documents,&local,&roaming,&saved})
            if(!std::filesystem::is_directory(*path))return FALSE;
        if(DetourTransactionBegin()!=NO_ERROR)return FALSE;
        if(DetourUpdateThread(GetCurrentThread())!=NO_ERROR ||
           DetourAttach(reinterpret_cast<PVOID*>(&originalKnown),RedirectKnown)!=NO_ERROR ||
           DetourAttach(reinterpret_cast<PVOID*>(&originalW),RedirectW)!=NO_ERROR ||
           DetourAttach(reinterpret_cast<PVOID*>(&originalA),RedirectA)!=NO_ERROR) {
            DetourTransactionAbort();return FALSE;
        }
        if(DetourTransactionCommit()!=NO_ERROR)return FALSE;
        // Validate both allocation and fixed-buffer APIs before the child resumes.
        PWSTR probe=nullptr;wchar_t fixed[MAX_PATH]{};
        const bool valid=SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents,0,nullptr,&probe)) && probe && documents==probe &&
            SUCCEEDED(SHGetFolderPathW(nullptr,CSIDL_LOCAL_APPDATA,nullptr,0,fixed)) && local==fixed;
        CoTaskMemFree(probe);
        if(!valid)return FALSE;
        // Startup diagnostics only. The handler observes faults and always
        // lets native exception dispatch continue; clean timing runs omit it.
        wchar_t diagnosis[4]{};
        if(GetEnvironmentVariableW(L"FPS_BENCH_STARTUP_DIAG",diagnosis,4)==1 && diagnosis[0]==L'1') {
            wchar_t destination[32768]{};
            if(!GetEnvironmentVariableW(L"DTL_BENCH_DIR",destination,32768))return FALSE;
            const auto path=std::filesystem::canonical(destination)/L"first-chance-av.jsonl";
            if(path.native().size()>=32768)return FALSE;
            wcscpy_s(exceptionLog,path.c_str());
            if(!AddVectoredExceptionHandler(1,RecordAccessViolation))return FALSE;
            const auto exitPath=path.parent_path()/L"startup-exit.json";
            wcscpy_s(exitLog,exitPath.c_str());
            originalTerminate=reinterpret_cast<Terminate>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtTerminateProcess"));
            if(!originalTerminate)return FALSE;
            if(DetourTransactionBegin()!=NO_ERROR)return FALSE;
            if(DetourUpdateThread(GetCurrentThread())!=NO_ERROR ||
               DetourAttach(reinterpret_cast<PVOID*>(&originalExit),RecordExit)!=NO_ERROR ||
               DetourAttach(reinterpret_cast<PVOID*>(&originalTerminate),RecordTerminate)!=NO_ERROR) {
                DetourTransactionAbort();return FALSE;
            }
            if(DetourTransactionCommit()!=NO_ERROR)return FALSE;
        }
        std::ofstream(root/L"folder-guard.txt")<<"Process-local Documents/AppData redirection verified; pid="<<GetCurrentProcessId()<<'\n';
        return TRUE;
    } catch(...) {return FALSE;}
}
BOOL WINAPI DllMain(HINSTANCE,DWORD,LPVOID) {return TRUE;}
