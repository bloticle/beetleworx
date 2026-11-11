#include "shadow_readback.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include "gpu_constants_internal.h"
#include "gpu_sync.h"
#include "native_pixel_fixture.h"
#include <rex/runtime.h>
#include <rex/kernel/kernel_state.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/d3d12/shared_memory.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <string>

REXCVAR_DEFINE_BOOL(em2_probe_shadow_pixels,false,"Debug","Private-only one-shot numeric readback of the actual native opaque-caster shadow texture; 1x internal resolution required, no image output");
REXCVAR_DEFINE_BOOL(em2_probe_shadow_depth_pixels,false,"Debug","Private-only: read the native NiDepthTexture D24FS8 allocation instead of the R32 color attachment; requires shadow pixel probe");
REXCVAR_DEFINE_BOOL(em2_probe_shadow_pixel_transitions,false,"Debug","Private-only bounded numeric readback after stable shadow-map replacements; requires the shadow pixel probe and isolated user data");
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(std::string,em2_user_data_directory);
namespace {
struct HostState {DWORD error=GetLastError();unsigned csr=_mm_getcsr();~HostState(){SetLastError(error);_mm_setcsr(csr);}};
bool Readable(uint8_t* base,uint32_t address,size_t bytes){return base && address>=4096 && uint64_t(address)+bytes<=0x100000000ull && em2::GuestReadBatch::Readable(PPC_RAW_ADDR(address),bytes);}
template<class Tag,auto Member>struct Access {friend auto MemberOf(Tag){return Member;}};
using CP=rex::graphics::d3d12::D3D12CommandProcessor;
struct SharedTag{friend auto MemberOf(SharedTag);};template struct Access<SharedTag,&CP::shared_memory_>;
struct BeginTag{friend auto MemberOf(BeginTag);};template struct Access<BeginTag,&CP::BeginSubmission>;
struct Request {
    uint32_t address,side,click,header,fetch0,fetch1,fetch2,slot;uint8_t* base;
    bool operator==(const Request&)const=default;
};
std::atomic<bool> queued{false};std::atomic<unsigned> attempts{0};
std::atomic<uint32_t> probe_address{0};
struct ProbeHistory {
    std::mutex mutex;
    std::optional<Request> candidate,completed;
    std::chrono::steady_clock::time_point stable_since{};
    uint32_t slot=0;
};
ProbeHistory history;
bool SameHeader(const Request& request){
    auto* base=request.base;em2::GuestReadBatch inspection;
    return Readable(base,request.header,52) && (PPC_LOAD_U32(request.header)&15)==3 &&
        PPC_LOAD_U32(request.header+28)==request.fetch0 && PPC_LOAD_U32(request.header+32)==request.fetch1 && PPC_LOAD_U32(request.header+36)==request.fetch2;
}
bool Capture(em2::Em2CommandProcessor& cp,const Request request){
    em2::GraphicsResourceCheckpoint checkpoint(cp);if(!checkpoint.valid())return false;
    if(!SameHeader(request))return false;
    em2::ResolveRangeLedger ledger;if(!cp.CopyTrackedResolveLedger(ledger) || ledger.scale_x()!=1 || ledger.scale_y()!=1)return false;
    const auto bytes=uint64_t(request.side)*request.side*4;
    std::vector<em2::ResolvedStorageRange> ranges;if(!ledger.Snapshot(ranges))return false;
    // Refuse holes, incompatible texel widths and CPU-invalidated bytes. Never request an
    // upload of stale guest memory to make an incomplete map appear available.
    uint64_t covered=request.address,end=covered+bytes;
    for(const auto& range:ranges){
        if(uint64_t(range.address)+range.length<=covered)continue;
        if(range.address>covered || range.bytes_per_texel!=4)break;
        covered=std::min(end,uint64_t(range.address)+range.length);if(covered==end)break;
    }
    if(covered!=end)return false;
    auto* shared=(cp.*MemberOf(SharedTag{})).get();if(!shared || !shared->GetBuffer() || end>shared->GetBuffer()->GetDesc().Width)return false;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    if(!em2::pixel_test::Buffer(cp.GetD3D12Provider().GetDevice(),bytes,D3D12_HEAP_TYPE_READBACK,&readback) || !(cp.*MemberOf(BeginTag{}))(false))return false;
    struct RetainPending {Microsoft::WRL::ComPtr<ID3D12Resource>& buffer;bool armed=false;~RetainPending(){if(armed)buffer.Detach();}} pending{readback};
    shared->UseAsCopySource();cp.SubmitBarriers();pending.armed=true;
    cp.GetDeferredCommandList().D3DCopyBufferRegion(readback.Get(),0,shared->GetBuffer(),request.address,bytes);
    if(!cp.DrainForResourceChange())return false; // Retain on unknown GPU completion/unwind.
    pending.armed=false;
    if(!SameHeader(request))return false; // A resized/replaced descriptor is not this capture.
    void* mapping=nullptr;D3D12_RANGE range{0,size_t(bytes)};if(FAILED(readback->Map(0,&range,&mapping)))return false;
    uint64_t zeros=0,ones=0,nonfinite=0,different=0,outside=0,stencil_nonzero=0;
    const bool depth=(request.fetch1&63)==23;
    float low=std::numeric_limits<float>::infinity(),high=-low;
    uint32_t first=0;std::memcpy(&first,mapping,4);
    if(depth)first=std::byteswap(first)>>8;
    for(size_t i=0;i<size_t(bytes)/4;++i){
        uint32_t bits;std::memcpy(&bits,static_cast<const uint8_t*>(mapping)+i*4,4);
        if(depth){bits=std::byteswap(bits);stencil_nonzero+=bool(bits&255);bits>>=8;}
        different+=bits!=first;
        const auto value=depth?rex::graphics::xenos::Float20e4To32(bits):std::bit_cast<float>(bits);
        if(!std::isfinite(value)){++nonfinite;continue;}
        zeros+=value==0;ones+=value==1;outside+=value<0 || value>1;low=std::min(low,value);high=std::max(high,value);
    }
    D3D12_RANGE none{0,0};readback->Unmap(0,&none);
    if(depth)REXLOG_INFO("Native shadow depth GPU pixels: click={:08X} header={:08X} physical={:08X} side={} format=D24FS8 endian=8in32 tiled=1 scale=1x texels={} guest_min={:.9f} guest_max={:.9f} zeros={} ones={} nonfinite={} outside_unit_range={} different_depth_from_first={} stencil_nonzero={}; native Float20e4To32(packed>>8) after header endian, complete GPU-resolved coverage, numeric only; visible receivers unproven",request.click,request.header,request.address,request.side,bytes/4,low,high,zeros,ones,nonfinite,outside,different,stencil_nonzero);
    else REXLOG_INFO("Native shadow GPU pixels: click={:08X} header={:08X} physical={:08X} side={} format=R32_FLOAT endian=none tiled=1 scale=1x texels={} min={:.9f} max={:.9f} zeros={} ones={} nonfinite={} outside_depth={} different_from_first={}; exact native header and fully GPU-resolved coverage, numeric only, no image; nonuniform contents are not visual receiver acceptance",request.click,request.header,request.address,request.side,bytes/4,low,high,zeros,ones,nonfinite,outside,different);
    return true;
}
}
uint32_t em2::ShadowPixelProbeAddress() noexcept {return probe_address.load(std::memory_order_relaxed);}
void em2::ProbeNativeShadowPixels(uint8_t* base,uint32_t click) noexcept {
    if(!REXCVAR_GET(em2_probe_shadow_pixels) || !REXCVAR_GET(em2_private_profile) || queued.load())return;
    const bool follow=REXCVAR_GET(em2_probe_shadow_pixel_transitions);
    if(attempts.load()>=(follow?12u:5u))return;
    HostState restore;
    try{
        if(REXCVAR_GET(em2_user_data_directory).empty())return;
        Request request{};
        {
            GuestReadBatch inspection;
            if(!Readable(base,click,152) || PPC_LOAD_U32(click)!=0x8203EAA8 || !PPC_LOAD_U32(click+128))return;
            const auto generator=PPC_LOAD_U32(click+140);if(!Readable(base,generator,108) || PPC_LOAD_U32(generator)!=0x8203DEC0)return;
            const auto target=PPC_LOAD_U32(generator+84);if(!Readable(base,target,36) || PPC_LOAD_U32(target)!=0x8203D708 || PPC_LOAD_U32(target+24)!=click)return;
            const auto slot=PPC_LOAD_U32(target+32);if(slot<2 || slot>4)return;
            const bool depth=REXCVAR_GET(em2_probe_shadow_depth_pixels);
            const auto surface=PPC_LOAD_U32(target+(depth?20:16));
            if(!Readable(base,surface,56) || PPC_LOAD_U32(surface)!=(depth?0x821DE978u:0x82043108u))return;
            const auto data=PPC_LOAD_U32(surface+52);if(!Readable(base,data,24))return;
            const auto side=PPC_LOAD_U32(data+12);if(side<128 || side>2048 || !std::has_single_bit(side) || PPC_LOAD_U32(data+16)!=side)return;
            const auto native=PPC_LOAD_U32(data+20);if(!Readable(base,native,depth?40:28) || PPC_LOAD_U32(native)!=(depth?0x82055A3Cu:0x82055A1Cu))return;
            const auto header=PPC_LOAD_U32(native+(depth?28:24));if(!Readable(base,header,52) || (PPC_LOAD_U32(header)&15)!=3)return;
            const auto word0=PPC_LOAD_U32(header+28),word1=PPC_LOAD_U32(header+32),word2=PPC_LOAD_U32(header+36);
            if(!(word0>>31) || ((word0>>22)&511)*32!=side || (word1&255)!=(depth?0x97u:36u) || (word2&8191)+1!=side || ((word2>>13)&8191)+1!=side)return;
            const auto address=word1&0x1FFFF000u;
            if(!address || uint64_t(address)+uint64_t(side)*side*4>0x20000000)return;
            request={address,side,click,header,word0,word1,word2,slot,base};
        }
        {
            std::lock_guard lock(history.mutex);
            // Follow one native slot through replacements, not arbitrary maps
            // that alternate during the same frame. Completion is per exact
            // header/fetch/click identity, never just a recycled host address.
            if(!history.slot)history.slot=request.slot;
            if(history.slot!=request.slot || (history.completed && *history.completed==request))return;
            probe_address.store(request.address,std::memory_order_relaxed);
            const auto now=std::chrono::steady_clock::now();
            if(!history.candidate || *history.candidate!=request){history.candidate=request;history.stable_since=now;return;}
            if(now-history.stable_since<std::chrono::seconds(2))return;
        }
        auto* kernel=rex::kernel::kernel_state();auto* runtime=kernel?kernel->emulator():nullptr;
        auto* graphics=runtime?runtime->graphics_system():nullptr;
        if(!graphics || (graphics->name().find("D3D12")==std::string::npos && graphics->name().find("Direct3D 12")==std::string::npos))return;
        auto* cp=static_cast<Em2CommandProcessor*>(graphics->command_processor());if(!cp || cp->IsGraphicsWorker() || queued.exchange(true))return;
        const auto attempt=attempts.fetch_add(1)+1;
        cp->CallInThread([cp,request,attempt,follow]{
            HostState restore;bool complete=false;try{complete=Capture(*cp,request);}catch(...){}
            if(complete){std::lock_guard lock(history.mutex);history.completed=request;}
            else REXLOG_WARN("Native shadow GPU readback attempt {} unavailable; requires stable native map and fully resolved 1x data; no image or rendering substitute",attempt);
            if(follow || !complete)queued.store(false);
        });
    }catch(...){queued.store(false);}
}
