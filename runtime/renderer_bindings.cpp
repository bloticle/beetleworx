#include "renderer_bindings.h"
#include "gpu_sync.h"
#include "gpu_constants_internal.h"
#include <immintrin.h>
namespace {
using CP=rex::graphics::d3d12::D3D12CommandProcessor;
using RT=rex::graphics::d3d12::D3D12RenderTargetCache;
template<class Tag,auto Member>struct Access {friend auto MemberOf(Tag){return Member;}};
struct PrimaryTag{friend auto MemberOf(PrimaryTag);};template struct Access<PrimaryTag,&CP::render_target_cache_>;
struct ModeTag{friend auto MemberOf(ModeTag);};template struct Access<ModeTag,&CP::bindless_resources_used_>;
struct TargetModeTag{friend auto MemberOf(TargetModeTag);};template struct Access<TargetModeTag,&RT::bindless_resources_used_>;
struct BufferTag{friend auto MemberOf(BufferTag);};template struct Access<BufferTag,&RT::edram_buffer_>;
struct Fp {unsigned value=_mm_getcsr();~Fp(){_mm_setcsr(value);}};
void Bind(CP& cp,RT& cache) {
    using View=CP::SystemBindlessView;
    cache.WriteEdramRawSRVDescriptor(cp.GetSystemBindlessViewHandlePair(View::kEdramRawSRV).first);
    cache.WriteEdramRawUAVDescriptor(cp.GetSystemBindlessViewHandlePair(View::kEdramRawUAV).first);
    for(uint32_t power=2;power<=4;++power) {
        cache.WriteEdramUintPow2SRVDescriptor(cp.GetEdramUintPow2BindlessSRVHandlePair(power).first,power);
        cache.WriteEdramUintPow2UAVDescriptor(cp.GetEdramUintPow2BindlessUAVHandlePair(power).first,power);
    }
}
}
em2::EdramSystemBindings::EdramSystemBindings(Em2CommandProcessor& cp,const GraphicsResourceCheckpoint& checkpoint,RT& target) {
    Fp fp;
    if(!cp.IsGraphicsWorker() || !checkpoint.owns(cp) || cp.edram_bindings_overridden_)return;
    auto* previous=(cp.*MemberOf(PrimaryTag{})).get();const bool mode=cp.*MemberOf(ModeTag{});
    if(!previous || !(previous->*MemberOf(BufferTag{})) || !(target.*MemberOf(BufferTag{})) ||
       previous->GetPath()!=target.GetPath() || (previous->*MemberOf(TargetModeTag{}))!=mode || (target.*MemberOf(TargetModeTag{}))!=mode || !cp.DrainForResourceChange())return;
    if(mode)Bind(cp,target);
    previous_=previous;target_=&target;bindless_=mode;processor_=&cp;cp.edram_bindings_overridden_=true;
}
em2::EdramSystemBindings::~EdramSystemBindings(){try{Restore();}catch(...){}}
bool em2::EdramSystemBindings::Restore() {
    Fp fp;if(!processor_)return true;auto& cp=*processor_;
    if(!cp.IsGraphicsWorker() || !cp.resource_checkpoint_ || (cp.*MemberOf(PrimaryTag{})).get()!=previous_ ||
       !cp.DrainForResourceChange())return false;
    if(bindless_)Bind(cp,*previous_);
    cp.edram_bindings_overridden_=false;processor_=nullptr;return true;
}
bool em2::EdramSystemBindings::Commit() {
    Fp fp;if(!processor_)return false;auto& cp=*processor_;
    if(!cp.IsGraphicsWorker() || !cp.resource_checkpoint_ || (cp.*MemberOf(PrimaryTag{})).get()!=target_ ||
       !cp.DrainForResourceChange())return false;
    cp.edram_bindings_overridden_=false;processor_=nullptr;return true;
}
