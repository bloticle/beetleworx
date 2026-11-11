#include "native_sampler.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <share.h>
#include <fstream>
#include <memory>
#include <thread>

REXCVAR_DEFINE_BOOL(em2_sample_native_render,false,"Debug","Intrusively sample render-thread instruction addresses; includes waits and perturbs timings");
REXCVAR_DEFINE_BOOL(em2_sample_native_main,false,"Debug","Intrusively sample the main guest entry including startup, video and waits; not a CPU benchmark");
REXCVAR_DEFINE_STRING(em2_native_sample_file,"native-render-samples.tsv","Debug","Native render sample output; analyze with the matching executable map");

namespace {
class NativeSampler {
public:
    explicit NativeSampler(const std::filesystem::path& output):owner_(GetCurrentThreadId()) {
        target_=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|SYNCHRONIZE,FALSE,owner_);
        if(!target_) { REXLOG_ERROR("Native sampler cannot open render thread: {}",GetLastError()); return; }
        file_=_wfsopen(output.c_str(),L"wb",_SH_DENYWR);
        if(!file_) {
            CloseHandle(target_); target_=nullptr;
            REXLOG_ERROR("Native sampler cannot open output {}",output.string()); return;
        }
        std::fprintf(file_,"# Intrusive wall-time instruction samples, including waits; not a benchmark\n# image_base %llX\n# rip\tmodule_base\tmodule\n",
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))));
        std::fflush(file_);
        worker_=std::thread([this]{Run();});
    }
    ~NativeSampler() {
        quit_.store(true,std::memory_order_release);
        if(worker_.joinable()) worker_.join();
        if(file_) std::fclose(file_);
        if(target_) CloseHandle(target_);
    }
    bool Begin() {
        if(!target_ || GetCurrentThreadId()!=owner_ || (epoch_.load(std::memory_order_relaxed)&1)) return false;
        epoch_.fetch_add(1,std::memory_order_release);
        return true;
    }
    void End() {epoch_.fetch_add(1,std::memory_order_release);}
    uint64_t Samples() const {return samples_.load(std::memory_order_relaxed);}
    bool Finished() const{return finished_.load(std::memory_order_acquire);}
private:
    void Dump(const std::array<uint64_t,4096>& ips,unsigned count) {
        // Resolve modules and perform all allocations/I/O after resuming the
        // target. Nothing in the suspended interval acquires a user-mode lock.
        for(unsigned i=0;i<count;++i) {
            HMODULE module=nullptr;
            wchar_t path[MAX_PATH+1]{};
            char utf8[MAX_PATH*4+1]{};
            if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCWSTR>(ips[i]),&module)) GetModuleFileNameW(module,path,MAX_PATH);
            WideCharToMultiByte(CP_UTF8,0,path,-1,utf8,sizeof(utf8),nullptr,nullptr);
            std::fprintf(file_,"%llX\t%llX\t%s\n",static_cast<unsigned long long>(ips[i]),
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(module)),utf8);
        }
        std::fprintf(file_,"# totals samples %llu suspend_failures %llu context_failures %llu resume_failures %llu\n",
            static_cast<unsigned long long>(Samples()),static_cast<unsigned long long>(suspend_failures_),
            static_cast<unsigned long long>(context_failures_),static_cast<unsigned long long>(resume_failures_));
        std::fflush(file_);
    }
    void Run() {
        std::array<uint64_t,4096> ips{};
        unsigned count=0;
        uint32_t random=0x5EED1234;
        auto last=GetTickCount64();
        while(!quit_.load(std::memory_order_acquire)) {
            if(WaitForSingleObject(target_,0)==WAIT_OBJECT_0)break;
            random=random*1664525u+1013904223u;
            Sleep(1+((random>>16)%3));
            const auto epoch=epoch_.load(std::memory_order_acquire);
            if(epoch&1) {
                alignas(16) CONTEXT context{};
                context.ContextFlags=CONTEXT_CONTROL;
                if(SuspendThread(target_)==DWORD(-1)) {if(WaitForSingleObject(target_,0)==WAIT_OBJECT_0)break;++suspend_failures_;}
                else {
                    // While suspended: only Win32 context/resume operations,
                    // atomic reads and stack/preallocated-memory writes.
                    const bool current=epoch_.load(std::memory_order_acquire)==epoch;
                    const bool obtained=current && GetThreadContext(target_,&context);
                    const auto resumed=ResumeThread(target_);
                    if(resumed==DWORD(-1)) {++resume_failures_; break;}
                    if(obtained) {ips[count++]=context.Rip; samples_.fetch_add(1,std::memory_order_relaxed);}
                    else if(current) ++context_failures_;
                }
            }
            const auto now=GetTickCount64();
            if(count==ips.size() || now-last>=5000) {Dump(ips,count); count=0; last=now;}
        }
        Dump(ips,count);
        finished_.store(true,std::memory_order_release);
    }
    DWORD owner_;
    HANDLE target_=nullptr;
    FILE* file_=nullptr;
    std::thread worker_;
    std::atomic<bool> quit_{false};
    std::atomic<bool> finished_{false};
    std::atomic<uint64_t> epoch_{0},samples_{0};
    uint64_t suspend_failures_=0,context_failures_=0,resume_failures_=0;
};
NativeSampler& Sampler() {
    static NativeSampler sampler(REXCVAR_GET(em2_native_sample_file));
    return sampler;
}
}

em2::NativeRenderSampleScope::NativeRenderSampleScope() {
    if(REXCVAR_GET(em2_sample_native_render)) active_=Sampler().Begin();
}
em2::NativeRenderSampleScope::~NativeRenderSampleScope() {if(active_) Sampler().End();}
em2::NativeMainSampleScope::NativeMainSampleScope(){if(REXCVAR_GET(em2_sample_native_main))active_=Sampler().Begin();}
em2::NativeMainSampleScope::~NativeMainSampleScope(){if(active_)Sampler().End();}

bool em2::TestNativeRenderSampler(const std::filesystem::path& output) {
    uint64_t count=0;
    bool readable=false;
    {
        NativeSampler sampler(output);
        { std::ifstream live(output); readable=live.is_open(); }
        const bool active=sampler.Begin();
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(150);
        while(active && std::chrono::steady_clock::now()<deadline) YieldProcessor();
        if(active) sampler.End();
        count=sampler.Samples();
    }
    std::ifstream input(output);
    const std::string contents((std::istreambuf_iterator<char>(input)),{});
    auto ended_output=output;ended_output+=".ended.tsv";std::unique_ptr<NativeSampler> ended;
    std::thread owner([&]{ended=std::make_unique<NativeSampler>(ended_output);ended->Begin();Sleep(20);});owner.join();
    const auto deadline=GetTickCount64()+500;while(!ended->Finished()&&GetTickCount64()<deadline)Sleep(1);
    const bool stopped=ended->Finished();ended.reset();
    const bool ok=stopped && readable && count>0 && contents.find("# image_base ")!=std::string::npos &&
        contents.find("suspend_failures 0 context_failures 0 resume_failures 0")!=std::string::npos;
    if(ok) REXLOG_INFO("Native sampler self-test passed: {} same-process instruction samples",count);
    else REXLOG_ERROR("Native sampler self-test failed: {} samples",count);
    return ok;
}
