#include "gpu_sync.h"
#include "gpu_constants_internal.h"
#include <rex/runtime.h>
#include <rex/kernel/kernel_state.h>
#include <rex/kernel/xthread.h>
#include <rex/graphics/graphics_system.h>
#include <rex/logging.h>
#include <rex/cvar.h>
#include <rex/graphics/d3d12/texture_cache.h>
#include <rex/graphics/d3d12/shared_memory.h>
#include <rex/graphics/d3d12/primitive_processor.h>
#include <string>
#include <future>
#include <thread>
#include <chrono>
#include <bit>
#include <cstring>
#include <immintrin.h>
#include <wrl/client.h>
REXCVAR_DEFINE_BOOL(em2_profile_world_memory,false,"Debug","Read-only GPU texture-cache and system commit snapshots at native loads and agreed world boundaries");
namespace {
using CP=rex::graphics::d3d12::D3D12CommandProcessor;
// The SDK has no public flush-and-wait method. Use its declared native method
// without changing SDK headers/layout or reimplementing submission accounting.
template<class Tag,auto Member>struct Access {friend auto MemberOf(Tag){return Member;}};
using TC=rex::graphics::TextureCache;
struct TexturePointerTag{friend auto MemberOf(TexturePointerTag);};template struct Access<TexturePointerTag,&CP::texture_cache_>;
struct TextureBytesTag{friend auto MemberOf(TextureBytesTag);};template struct Access<TextureBytesTag,&TC::textures_total_host_memory_usage_>;
struct TextureCountTag{friend auto MemberOf(TextureCountTag);};template struct Access<TextureCountTag,&TC::textures_>;
using UP=rex::ui::GraphicsUploadBufferPool;
struct ConstantPoolTag{friend auto MemberOf(ConstantPoolTag);};template struct Access<ConstantPoolTag,&CP::constant_buffer_pool_>;
struct SharedPoolTag{friend auto MemberOf(SharedPoolTag);};template struct Access<SharedPoolTag,&rex::graphics::d3d12::D3D12SharedMemory::upload_buffer_pool_>;
struct PrimitiveTag{friend auto MemberOf(PrimitiveTag);};template struct Access<PrimitiveTag,&CP::primitive_processor_>;
struct IndexPoolTag{friend auto MemberOf(IndexPoolTag);};template struct Access<IndexPoolTag,&rex::graphics::d3d12::D3D12PrimitiveProcessor::frame_index_buffer_pool_>;
struct WritableTag{friend auto MemberOf(WritableTag);};template struct Access<WritableTag,&UP::writable_first_>;
struct SubmittedTag{friend auto MemberOf(SubmittedTag);};template struct Access<SubmittedTag,&UP::submitted_first_>;
struct PageSizeTag{friend auto MemberOf(PageSizeTag);};template struct Access<PageSizeTag,&UP::page_size_>;
struct GpuHeapsTag{friend auto MemberOf(GpuHeapsTag);};template struct Access<GpuHeapsTag,&rex::graphics::d3d12::D3D12SharedMemory::buffer_tiled_heaps_>;
struct UploadStats{uint64_t writable=0,submitted=0,page_size=0;};
UploadStats UploadState(UP* pool){
    UploadStats result;if(!pool)return result;result.page_size=pool->*MemberOf(PageSizeTag{});
    for(auto* page=pool->*MemberOf(WritableTag{});page && result.writable<4096;page=page->next_)++result.writable;
    for(auto* page=pool->*MemberOf(SubmittedTag{});page && result.submitted<4096;page=page->next_)++result.submitted;
    return result;
}

struct SharedTag{friend auto MemberOf(SharedTag);};template struct Access<SharedTag,&CP::shared_memory_>;
struct WaitTag{friend auto MemberOf(WaitTag);};
template struct Access<WaitTag,&CP::CheckSubmissionFence>;
struct BeginTag{friend auto MemberOf(BeginTag);};
template struct Access<BeginTag,&CP::BeginSubmission>;
struct OpenTag{friend auto MemberOf(OpenTag);};
template struct Access<OpenTag,&CP::submission_open_>;
struct QueuePendingTag{friend auto MemberOf(QueuePendingTag);};
template struct Access<QueuePendingTag,&CP::queue_operations_done_since_submission_signal_>;

bool Synchronize(rex::graphics::GraphicsSystem& graphics) {
    if(graphics.name().find("D3D12")==std::string::npos && graphics.name().find("Direct3D 12")==std::string::npos)return false;
    auto done=std::make_shared<std::promise<bool>>();auto result=done->get_future();
    auto* cp=static_cast<em2::Em2CommandProcessor*>(graphics.command_processor());
    if(!cp)return false;
    // Native interrupts may run on this worker from inside packet execution.
    // Waiting here could self-deadlock or reenter the primary ring interpreter.
    if(cp->IsGraphicsWorker())return false;
    cp->CallInThread([cp,done] {
        const auto csr=_mm_getcsr();
        bool success=false;
        try{success=cp->DrainForResourceChange();}catch(...){success=false;}
        _mm_setcsr(csr);done->set_value(success);
    });
    if(result.wait_for(std::chrono::seconds(30))!=std::future_status::ready) {
        // Callback owns its promise and only drains work. It may finish later;
        // no pointers to a timed-out caller's stack or resources are captured.
        REXLOG_ERROR("Graphics resource synchronization timed out; retain existing resources");return false;
    }
    return result.get();
}
}
bool em2::Em2CommandProcessor::IsGraphicsWorker() const{return rex::kernel::XThread::IsInThread(worker_thread_.get());}
em2::GraphicsResourceCheckpoint::GraphicsResourceCheckpoint(Em2CommandProcessor& cp):
    memory_lock_(rex::thread::global_critical_region::mutex(),std::defer_lock) {
    if(!cp.IsGraphicsWorker() || cp.resource_checkpoint_)return;
    // Do not wait behind a guest thread holding the global region while it
    // needs GPU progress. Busy callers retry at a later command-worker boundary.
    if(!memory_lock_.try_lock())return;memory_lock_.unlock();
    if(!cp.DrainForResourceChange())return;
    cp.resource_checkpoint_=true;
    try {if(memory_lock_.try_lock())processor_=&cp;else cp.resource_checkpoint_=false;}
    catch(...){cp.resource_checkpoint_=false;}
}
em2::GraphicsResourceCheckpoint::~GraphicsResourceCheckpoint() {
    if(processor_)processor_->resource_checkpoint_=false;
}
bool em2::Em2CommandProcessor::DrainForResourceChange() {
    const auto write=write_ptr_index_.load();
    if(!resource_checkpoint_ && write!=0xBAADF00D && read_ptr_index_!=write) {
        if(!primary_buffer_ptr_ || primary_buffer_size_<4 || (primary_buffer_size_&3) ||
           read_ptr_index_>=primary_buffer_size_/4 || write>=primary_buffer_size_/4)return false;
        read_ptr_index_=ExecutePrimaryBuffer(read_ptr_index_,write);
        if(read_ptr_index_!=write)return false;
        if(read_ptr_writeback_ptr_)*memory_->TranslatePhysical<uint32_t*>(read_ptr_writeback_ptr_)=std::byteswap(read_ptr_index_);
    }
    (this->*MemberOf(WaitTag{}))(GetCurrentSubmission());
    return !(this->*MemberOf(OpenTag{})) && !(this->*MemberOf(QueuePendingTag{})) &&
        SUCCEEDED(GetD3D12Provider().GetDevice()->GetDeviceRemovedReason()) && GetCompletedSubmission()+1>=GetCurrentSubmission();
}
void em2::ReportWorldGraphicsMemory(std::string_view phase,uint64_t generation) noexcept {
    const auto csr=_mm_getcsr();const auto error=GetLastError();
    struct Host {unsigned csr;DWORD error;~Host(){_mm_setcsr(csr);SetLastError(error);}} host{csr,error};
    if(!REXCVAR_GET(em2_profile_world_memory))return;
    try {
        auto* runtime=rex::Runtime::instance();auto* graphics=runtime?runtime->graphics_system():nullptr;
        if(!graphics || (graphics->name().find("D3D12")==std::string::npos && graphics->name().find("Direct3D 12")==std::string::npos))return;
        auto* cp=static_cast<CP*>(graphics->command_processor());if(!cp)return;
        cp->CallInThread([cp,generation,phase=std::string(phase)]{
            const auto csr=_mm_getcsr();const auto error=GetLastError();
            struct Host {unsigned csr;DWORD error;~Host(){_mm_setcsr(csr);SetLastError(error);}} host{csr,error};
            auto* cache=(cp->*MemberOf(TexturePointerTag{})).get();if(!cache)return;
            MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);if(!GlobalMemoryStatusEx(&memory))return;
            REXLOG_INFO("Native world graphics memory: phase '{}' generation {} textures {} texture bytes {} system available commit {}; GPU-worker snapshot only, no cache changes",phase,generation,
                (cache->*MemberOf(TextureCountTag{})).size(),cache->*MemberOf(TextureBytesTag{}),memory.ullAvailPageFile);
            auto* shared=(cp->*MemberOf(SharedTag{})).get();auto* primitive=(cp->*MemberOf(PrimitiveTag{})).get();
            const auto constants=UploadState((cp->*MemberOf(ConstantPoolTag{})).get());
            const auto uploads=UploadState(shared?(shared->*MemberOf(SharedPoolTag{})).get():nullptr);
            const auto indices=UploadState(primitive?(primitive->*MemberOf(IndexPoolTag{})).get():nullptr);
            uint64_t heap_bytes=0,heap_count=0;
            if(shared)for(auto* heap:shared->*MemberOf(GpuHeapsTag{}))if(heap){heap_bytes+=heap->GetDesc().SizeInBytes;++heap_count;}
            REXLOG_INFO("Native world GPU allocation pools: phase '{}' generation {} shared heaps {}/{} bytes; constant upload writable/submitted {}/{} page {}; shared upload {}/{} page {}; index upload {}/{} page {}; completed submission {}",phase,generation,heap_count,heap_bytes,
                constants.writable,constants.submitted,constants.page_size,uploads.writable,uploads.submitted,uploads.page_size,indices.writable,indices.submitted,indices.page_size,cp->GetCompletedSubmission());
        });
    }catch(...){/* A diagnostic must not change native loading or input. */}
}
bool em2::SynchronizeGraphicsForResourceChange() {
    auto* kernel=rex::kernel::kernel_state();
    auto* runtime=kernel?kernel->emulator():nullptr;
    auto* graphics=runtime?runtime->graphics_system():nullptr;
    if(!graphics)return false;
    const auto csr=_mm_getcsr();const bool success=Synchronize(*graphics);_mm_setcsr(csr);return success;
}
bool em2::TestGraphicsResourceSynchronization(rex::graphics::GraphicsSystem& graphics) {
    // Called on the real GPU worker by the renderer diagnostic. Restore the
    // unstarted title's ring state before the worker resumes its ordinary loop.
    auto& cp=*static_cast<Em2CommandProcessor*>(graphics.command_processor());
    if(!cp.IsGraphicsWorker() || Synchronize(graphics))return false;
    auto* memory=graphics.memory();
    auto* heap=memory->LookupHeapByType(true,0x1000);uint32_t allocation=0;
    if(!heap || !heap->Alloc(0x2000,0x1000,rex::memory::kMemoryAllocationReserve|rex::memory::kMemoryAllocationCommit,
        rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite,false,&allocation))return false;
    struct Restore {
        Em2CommandProcessor& cp;rex::memory::BaseHeap* heap;uint32_t allocation,ptr,size,read,write,back;unsigned csr;
        ~Restore(){cp.primary_buffer_ptr_=ptr;cp.primary_buffer_size_=size;cp.read_ptr_index_=read;cp.write_ptr_index_.store(write);cp.read_ptr_writeback_ptr_=back;heap->Release(allocation,nullptr);_mm_setcsr(csr);}
    }restore{cp,heap,allocation,cp.primary_buffer_ptr_,cp.primary_buffer_size_,cp.read_ptr_index_,cp.write_ptr_index_.load(),cp.read_ptr_writeback_ptr_,_mm_getcsr()};
    const auto physical=memory->GetPhysicalAddress(allocation);if(physical==UINT32_MAX)return false;
    auto* words=memory->TranslateVirtual<uint32_t*>(allocation);
    constexpr uint32_t reg=0x4028,value=0x3F123456;
    const auto previous=cp.ConstantRegisterValues()[reg];
    struct RegisterRestore {Em2CommandProcessor& cp;uint32_t value;~RegisterRestore(){cp.WriteNativeConstant(reg,value);}}register_restore{cp,previous};
    cp.primary_buffer_ptr_=physical;cp.primary_buffer_size_=0x1000;cp.read_ptr_index_=0;cp.read_ptr_writeback_ptr_=physical+0x1000;
    words[0]=std::byteswap(reg);words[1]=std::byteswap(value);words[1024]=0xFFFFFFFF;
    cp.write_ptr_index_.store(2);
    if(!cp.DrainForResourceChange() || cp.read_ptr_index_!=2 || std::byteswap(words[1024])!=2 || cp.ConstantRegisterValues()[reg]!=value)return false;
    // Same packet crossing the physical ring end, including native read-pointer
    // writeback. The queue callback must not leave it for WorkerThreadMain.
    cp.read_ptr_index_=1023;words[1023]=std::byteswap(reg);words[0]=std::byteswap(value^0x80000000);cp.write_ptr_index_.store(1);
    if(!cp.DrainForResourceChange() || cp.read_ptr_index_!=1 || std::byteswap(words[1024])!=1 || cp.ConstantRegisterValues()[reg]!=(value^0x80000000))return false;
    cp.read_ptr_index_=1024;cp.write_ptr_index_.store(2);
    if(cp.DrainForResourceChange() || cp.read_ptr_index_!=1024)return false;
    cp.read_ptr_index_=1;cp.write_ptr_index_.store(1);
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D12Resource> source,destination;
    auto* device=cp.GetD3D12Provider().GetDevice();
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=4096;desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES properties{};properties.Type=D3D12_HEAP_TYPE_UPLOAD;
    if(FAILED(device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&source))))return false;
    properties.Type=D3D12_HEAP_TYPE_READBACK;
    if(FAILED(device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&destination))))return false;
    void* mapped=nullptr;D3D12_RANGE empty{0,0};if(FAILED(source->Map(0,&empty,&mapped)))return false;
    std::memset(mapped,0x6D,4096);D3D12_RANGE written{0,4096};source->Unmap(0,&written);
    // A contended native region must reject without waiting for its owner,
    // which may itself need this GPU worker to make progress.
    std::promise<void> held,release;auto held_ready=held.get_future();auto release_ready=release.get_future();
    std::jthread holder([&] {
        rex::thread::global_critical_region region;auto lock=region.Acquire();held.set_value();release_ready.wait();
    });
    const bool holder_ready=held_ready.wait_for(std::chrono::seconds(2))==std::future_status::ready;
    bool busy_rejected=false;if(holder_ready){GraphicsResourceCheckpoint busy(cp);busy_rejected=!busy.valid();}
    release.set_value();holder.join();if(!holder_ready || !busy_rejected)return false;
    std::promise<bool> published;auto publication=published.get_future();std::atomic<bool> invalidated=false;
    std::jthread producer;
    {
        GraphicsResourceCheckpoint checkpoint(cp);if(!checkpoint.valid())return false;
        GraphicsResourceCheckpoint nested(cp);if(nested.valid())return false;
        auto* shared=(cp.*MemberOf(SharedTag{})).get();
        producer=std::jthread([&,shared] {
            words[1]=std::byteswap(reg);words[2]=std::byteswap(value^0x12345678u);cp.UpdateWritePointer(3);
            rex::thread::global_critical_region region;auto attempt=region.TryAcquire();
            published.set_value(!attempt.owns_lock());if(attempt.owns_lock())attempt.unlock();
            shared->MemoryInvalidationCallback(physical+0x1000,1,true);invalidated.store(true,std::memory_order_release);
        });
        if(publication.wait_for(std::chrono::seconds(2))!=std::future_status::ready || !publication.get())return false;
        if(!(cp.*MemberOf(BeginTag{}))(false))return false;
        cp.GetDeferredCommandList().D3DCopyBufferRegion(destination.Get(),0,source.Get(),0,4096);
        if(!cp.DrainForResourceChange()){source.Detach();destination.Detach();return false;}
        if(cp.read_ptr_index_!=1 || std::byteswap(words[1024])!=1 ||
           cp.ConstantRegisterValues()[reg]!=(value^0x80000000) || invalidated.load(std::memory_order_acquire))return false;
    }
    producer.join();
    if(!invalidated.load(std::memory_order_acquire) || !cp.DrainForResourceChange() || cp.read_ptr_index_!=3 ||
       std::byteswap(words[1024])!=3 || cp.ConstantRegisterValues()[reg]!=(value^0x12345678u))return false;
    REXLOG_INFO("Graphics resource checkpoint passed: native producer publishes a new PM4 packet while pinned; real global-lock exclusion and CPU invalidation deferral, busy-region and nested checkpoint rejection, GPU copy/fence under lock, queued register/writeback preserved until scope exit then drained; no guest title execution");
    if(!(cp.*MemberOf(BeginTag{}))(false))return false;
    cp.GetDeferredCommandList().D3DCopyBufferRegion(destination.Get(),0,source.Get(),0,4096);
    // No guest presentation or manual EndSubmission: production drain must
    // flush the SDK deferred list and fence this actual GPU copy itself.
    if(!cp.DrainForResourceChange()){source.Detach();destination.Detach();return false;}
    if(FAILED(destination->Map(0,&written,&mapped)))return false;
    bool valid=true;for(unsigned i=0;i<4096;++i)valid&=static_cast<uint8_t*>(mapped)[i]==0x6D;
    destination->Unmap(0,&empty);if(!valid)return false;
    if(!cp.DrainForResourceChange())return false;
    REXLOG_INFO("Graphics resource synchronization passed: real pending/wrapped PM4 ring, read-pointer writeback, native deferred GPU copy flush/fence/readback and idle repeat; no guest title execution");return true;
}
bool em2::TestGraphicsResourceSynchronizationDispatch() {
    auto* kernel=rex::kernel::kernel_state();auto* runtime=kernel?kernel->emulator():nullptr;
    auto* graphics=runtime?runtime->graphics_system():nullptr;
    auto* cp=graphics?static_cast<Em2CommandProcessor*>(graphics->command_processor()):nullptr;if(!cp)return false;
    GraphicsResourceCheckpoint wrong_thread(*cp);if(wrong_thread.valid())return false;
    const auto csr=_mm_getcsr();
    if(!SynchronizeGraphicsForResourceChange() || _mm_getcsr()!=csr)return false;
    REXLOG_INFO("Graphics resource synchronization dispatch passed: real calling-thread to GPU-worker wait, caller MXCSR preserved; worker self-wait and invalid-ring cases rejected");return true;
}
