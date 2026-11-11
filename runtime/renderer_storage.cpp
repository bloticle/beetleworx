#include "renderer_storage.h"
#include "gpu_sync.h"
#include "gpu_constants_internal.h"
#include <immintrin.h>
namespace {
using CP=rex::graphics::d3d12::D3D12CommandProcessor;
template<class Tag,auto Member>struct Access {friend auto MemberOf(Tag){return Member;}};
struct RenderTag{friend auto MemberOf(RenderTag);};template struct Access<RenderTag,&CP::render_target_cache_>;
struct TextureTag{friend auto MemberOf(TextureTag);};template struct Access<TextureTag,&CP::texture_cache_>;
struct SharedTag{friend auto MemberOf(SharedTag);};template struct Access<SharedTag,&CP::shared_memory_>;
}
bool em2::CaptureRendererStorage(Em2CommandProcessor& cp,RendererStorageSnapshot& output) {
    struct Fp {unsigned value=_mm_getcsr();~Fp(){_mm_setcsr(value);}}fp;
    GraphicsResourceCheckpoint checkpoint(cp);return CaptureRendererStorage(cp,checkpoint,output);
}
bool em2::CaptureRendererStorage(Em2CommandProcessor& cp,const GraphicsResourceCheckpoint& checkpoint,RendererStorageSnapshot& output) {
    struct Fp {unsigned value=_mm_getcsr();~Fp(){_mm_setcsr(value);}}fp;
    if(!cp.IsGraphicsWorker() || !checkpoint.owns(cp))return false;
    auto* render=(cp.*MemberOf(RenderTag{})).get();auto* texture=(cp.*MemberOf(TextureTag{})).get();
    auto* shared=(cp.*MemberOf(SharedTag{})).get();if(!render || !texture || !shared)return false;
    const auto sx=render->draw_resolution_scale_x(),sy=render->draw_resolution_scale_y();
    if(sx!=texture->draw_resolution_scale_x() || sy!=texture->draw_resolution_scale_y())return false;
    ResolveRangeLedger ledger;RendererStorageSnapshot snapshot;
    if(!cp.CopyTrackedResolveLedger(ledger) || !CapturePackedEdram(cp,*render,snapshot.packed) ||
       !CaptureAuxiliaryDepth(cp,*render,snapshot.auxiliary) ||
       !CaptureResolvedStorage(cp,*texture,*shared,ledger,snapshot.resolved))return false;
    if(snapshot.packed.scale_x!=sx || snapshot.packed.scale_y!=sy || snapshot.auxiliary.scale_x!=sx ||
       snapshot.auxiliary.scale_y!=sy || snapshot.resolved.scale_x!=sx || snapshot.resolved.scale_y!=sy)return false;
    output=std::move(snapshot);return true;
}
