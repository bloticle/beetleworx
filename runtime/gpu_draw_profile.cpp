#include "gpu_draw_profile.h"
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/d3d12_util.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <immintrin.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_set>
#include <vector>

REXCVAR_DEFINE_BOOL(em2_profile_gpu_draws,false,"Debug","Sample actual GPU draw/dispatch intervals by PSO shader identity; intrusive diagnostic, disabled in normal rendering");
REXCVAR_DEFINE_STRING(em2_gpu_draw_profile_directory,"","Debug","Optional private directory for GPU profile shader DXBC; never modifies game shaders");
REXCVAR_DEFINE_INT32(em2_gpu_draw_report_limit,16,"Debug","Maximum GPU work groups per diagnostic report (1..4096); increase to audit low-cost caster draws, default 16");
REXCVAR_DEFINE_BOOL(em2_trace_shadow_gpu_state,false,"Debug","Private-only: log actual R32 or square depth-only GPU draw state, requires GPU work profiling; no rendering changes");
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(std::string,em2_user_data_directory);
namespace {
constexpr GUID tag_guid{0x730521fd,0x924a,0x4f18,{0x82,0x06,0xb2,0xa8,0x61,0xcf,0x83,0x39}};
struct Fp {unsigned csr=_mm_getcsr();~Fp(){_mm_setcsr(csr);}};
bool StencilResourceFormat(DXGI_FORMAT format){return format==DXGI_FORMAT_R24G8_TYPELESS || format==DXGI_FORMAT_D24_UNORM_S8_UINT || format==DXGI_FORMAT_R32G8X24_TYPELESS || format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT;}
uint64_t TextureCopyKey(const D3D12_TEXTURE_COPY_LOCATION& dst,const D3D12_RESOURCE_DESC& d,const D3D12_TEXTURE_COPY_LOCATION& src,const D3D12_RESOURCE_DESC& s){
    if(d.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER && s.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER)return 0xc0010001ull;
    if(d.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER && s.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER)return 0xc0010002ull;
    if(d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D && s.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
       dst.Type==D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && src.Type==D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
       StencilResourceFormat(d.Format) && s.Format==DXGI_FORMAT_R8_UINT && d.MipLevels==1 && s.MipLevels==1 &&
       d.DepthOrArraySize==1 && s.DepthOrArraySize==1 && dst.SubresourceIndex==1 && src.SubresourceIndex==0 &&
       d.SampleDesc.Count==s.SampleDesc.Count && d.SampleDesc.Quality==s.SampleDesc.Quality)return 0xc0010003ull;
    if((d.Flags|s.Flags)&D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL || StencilResourceFormat(d.Format) || StencilResourceFormat(s.Format))return 0xc0010004ull;
    if((d.Flags|s.Flags)&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)return 0xc0010005ull;
    return 0xc0010006ull;
}
uint64_t Hash(const void* data,size_t size,uint64_t h=14695981039346656037ull){const auto* p=static_cast<const uint8_t*>(data);for(size_t i=0;i<size;++i)h=(h^p[i])*1099511628211ull;return h;}
void Dump(const std::string& directory,uint64_t hash,D3D12_SHADER_BYTECODE code){
    if(directory.empty() || !code.pShaderBytecode || !code.BytecodeLength)return;
    static std::mutex mutex;static std::unordered_set<std::string> done;std::lock_guard lock(mutex);
    const auto path=std::filesystem::path(directory)/fmt::format("shader-{:016X}.dxbc",hash);
    if(done.size()>=4096 || !done.insert(path.string()).second)return;
    std::filesystem::create_directories(path.parent_path());std::ofstream file(path,std::ios::binary);file.write(static_cast<const char*>(code.pShaderBytecode),std::streamsize(code.BytecodeLength));
}
}
void em2::TagGpuProfilePipeline(ID3D12PipelineState* state,const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc){
    if(!state || !REXCVAR_GET(em2_profile_gpu_draws))return;Fp fp;
    try{
        GpuProfilePipeline tag{};tag.vs=Hash(desc.VS.pShaderBytecode,desc.VS.BytecodeLength);tag.ps=Hash(desc.PS.pShaderBytecode,desc.PS.BytecodeLength);tag.samples=desc.SampleDesc.Count;tag.targets=desc.NumRenderTargets;
        tag.color_format=desc.NumRenderTargets?desc.RTVFormats[0]:0;tag.write_mask=desc.BlendState.RenderTarget[0].RenderTargetWriteMask;
        tag.depth_format=desc.DSVFormat;tag.depth_enable=desc.DepthStencilState.DepthEnable;tag.depth_write=desc.DepthStencilState.DepthWriteMask;tag.depth_func=desc.DepthStencilState.DepthFunc;
        tag.cull=desc.RasterizerState.CullMode;tag.depth_clip=desc.RasterizerState.DepthClipEnable;tag.front_ccw=desc.RasterizerState.FrontCounterClockwise;
        tag.depth_bias=desc.RasterizerState.DepthBias;tag.slope_bias=desc.RasterizerState.SlopeScaledDepthBias;tag.bias_clamp=desc.RasterizerState.DepthBiasClamp;
        tag.key=Hash(&tag.vs,sizeof(tag.vs));tag.key=Hash(&tag.ps,sizeof(tag.ps),tag.key);tag.key=Hash(&tag.samples,sizeof(tag.samples),tag.key);tag.key=Hash(&tag.targets,sizeof(tag.targets),tag.key);tag.key=Hash(desc.RTVFormats,std::min<UINT>(8,desc.NumRenderTargets)*sizeof(DXGI_FORMAT),tag.key);
        state->SetPrivateData(tag_guid,sizeof(tag),&tag);const auto directory=REXCVAR_GET(em2_gpu_draw_profile_directory);Dump(directory,tag.vs,desc.VS);Dump(directory,tag.ps,desc.PS);
    }catch(...){} // A diagnostic must not turn a successful native PSO into a failure.
}
void em2::LabelGpuProfileTransferPipeline(ID3D12PipelineState* state,uint32_t mode){
    if(!state || !REXCVAR_GET(em2_profile_gpu_draws))return;Fp fp;
    // Native SDK TransferMode order; do not change shader code or PSO identity.
    static constexpr const char* names[]={"ColorToDepth","ColorToColor","DepthToDepth","DepthToColor","ColorToStencilBit","DepthToStencilBit","ColorAndHostDepthToDepth","DepthAndHostDepthToDepth"};
    if(mode>=std::size(names))return;
    GpuProfilePipeline tag{};UINT bytes=sizeof(tag);
    if(FAILED(state->GetPrivateData(tag_guid,&bytes,&tag)) || bytes!=sizeof(tag))return;
    REXLOG_INFO("GPU transfer label key {:016X} mode {} name {}",tag.key,mode,names[mode]);
}
extern "C" ID3D12PipelineState* EM2OriginalCreateComputePipeline(ID3D12Device*,const void*,size_t,ID3D12RootSignature*);
void em2::TagGpuProfileComputePipeline(ID3D12PipelineState* state,const void* shader,size_t bytes){
    if(!state || !REXCVAR_GET(em2_profile_gpu_draws))return;Fp fp;
    try{GpuProfilePipeline tag{};tag.ps=Hash(shader,bytes);tag.key=tag.ps;state->SetPrivateData(tag_guid,sizeof(tag),&tag);Dump(REXCVAR_GET(em2_gpu_draw_profile_directory),tag.ps,{shader,bytes});}catch(...){}
}
ID3D12PipelineState* rex::ui::d3d12::util::CreateComputePipeline(ID3D12Device* device,const void* shader,size_t bytes,ID3D12RootSignature* root){
    auto* state=EM2OriginalCreateComputePipeline(device,shader,bytes,root);em2::TagGpuProfileComputePipeline(state,shader,bytes);return state;
}
bool em2::GpuDrawProfiler::Initialize(rex::ui::d3d12::D3D12Provider& provider){
    Fp fp;if(heap_)return true;if(unavailable_)return false;auto* device=provider.GetDevice();
    D3D12_QUERY_HEAP_DESC query{};query.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;query.Count=capacity*2;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC resource{};resource.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;resource.Width=capacity*16;resource.Height=resource.DepthOrArraySize=resource.MipLevels=resource.SampleDesc.Count=1;resource.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;D3D12_RANGE range{0,capacity*16};
    if(FAILED(provider.GetDirectQueue()->GetTimestampFrequency(&frequency_)) || !frequency_ || FAILED(device->CreateQueryHeap(&query,IID_PPV_ARGS(&heap_))) || FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&resource,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback_))) || FAILED(readback_->Map(0,&range,reinterpret_cast<void**>(&mapped_)))){ResetAfterIdle();unavailable_=true;return false;}return true;
}
em2::GpuProfilePipeline em2::GpuDrawProfiler::Identify(ID3D12PipelineState* state)noexcept{
    GpuProfilePipeline result{};if(!state)return result;UINT size=sizeof(result);
    if(FAILED(state->GetPrivateData(tag_guid,&size,&result)) || size!=sizeof(result)){result={};result.key=uint64_t(reinterpret_cast<uintptr_t>(state));}return result;
}
unsigned em2::GpuDrawProfiler::Begin(ID3D12GraphicsCommandList* list,uint64_t submission,GpuProfilePipeline pipeline,GpuProfileWork kind,uint64_t elements)noexcept{
    try{
        // Different work classes cannot share a statistics bucket, even if a
        // diagnostic deliberately reuses the same PSO identity.
        const auto key=pipeline.key^(0x9e3779b97f4a7c15ull*unsigned(kind));
        auto [it,inserted]=buckets_.try_emplace(key);auto& bucket=it->second;if(inserted){bucket.pipeline=pipeline;bucket.kind=kind;}++bucket.calls;++recorded_calls_;++work_calls_[unsigned(kind)];
        random_^=random_<<13;random_^=random_>>7;random_^=random_<<17;
        const bool short_draw=kind==GpuProfileWork::Draw && elements<=6 && bucket.last_small_submission!=submission;
        const bool selected=every_call_ || short_draw || !(random_&(kind==GpuProfileWork::Draw?63:15));if(!selected)return invalid_token;
        if(short_draw)bucket.last_small_submission=submission;
        if(!heap_ || !submission){++bucket.skipped;return invalid_token;}
        for(unsigned i=0;i<capacity;++i)if(!slots_[i].pending){slots_[i]={submission,key,true,false};list->EndQuery(heap_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,i*2);return i;}
        ++bucket.skipped;
    }catch(...){}return invalid_token;
}
void em2::GpuDrawProfiler::End(ID3D12GraphicsCommandList* list,unsigned token)noexcept{
    if(token>=capacity || !slots_[token].pending || slots_[token].ended)return;
    list->EndQuery(heap_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,token*2+1);list->ResolveQueryData(heap_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,token*2,2,readback_.Get(),token*16);slots_[token].ended=true;
}
void em2::GpuDrawProfiler::Poll(uint64_t completed){
    Fp fp;for(unsigned i=0;i<capacity;++i){auto& slot=slots_[i];if(!slot.pending || !slot.ended || slot.submission>completed)continue;uint64_t times[2];std::memcpy(times,mapped_+i*2,sizeof(times));auto it=buckets_.find(slot.key);slot={};if(it==buckets_.end() || times[1]<times[0])continue;auto& bucket=it->second;const auto ticks=times[1]-times[0];++bucket.samples;++completed_samples_;bucket.ticks+=ticks;bucket.max_ticks=std::max(bucket.max_ticks,ticks);}
    if(std::chrono::steady_clock::now()-last_report_>=std::chrono::seconds(5))FlushReport();
}
void em2::GpuDrawProfiler::FlushReport(){
    Fp fp;last_report_=std::chrono::steady_clock::now();if(!frequency_)return;std::vector<const Bucket*> rows;for(const auto& [key,b]:buckets_)if(b.calls)rows.push_back(&b);
    auto estimate=[](const Bucket* value){return value->samples?double(value->ticks)/value->samples*value->calls:0.;};
    std::sort(rows.begin(),rows.end(),[&](const auto* a,const auto* b){return estimate(a)>estimate(b);});
    REXLOG_INFO("GPU selected work profile: lifetime counts, top estimated shader groups; sparse sampling, query/serialization overhead and fence latency apply. Mean*recorded-call estimates are not frame GPU time. PS=0 means state-less work or an untagged SDK/external PSO; untagged draw/dispatch keys are process-local.");
    constexpr const char* names[]={"draw","dispatch","copy","clear"};
    const auto limit=size_t(std::clamp(REXCVAR_GET(em2_gpu_draw_report_limit),1,4096));
    REXLOG_INFO("GPU group coverage: reporting {} of {} groups with recorded calls; zero completed samples means unmeasured, not zero GPU cost",std::min(rows.size(),limit),rows.size());
    for(size_t i=0;i<std::min(rows.size(),limit);++i){const auto& b=*rows[i];const auto mean=b.samples?b.ticks*1000.0/frequency_/b.samples:0.;REXLOG_INFO("GPU work {} key {:016X} VS {:016X} PS {:016X} MSAA {} MRT {}: {} calls, {} completed samples, {} skips; selected {:.3f} ms, mean {:.4f} ms, max {:.3f} ms, rough lifetime estimate {:.1f} ms",names[unsigned(b.kind)],b.pipeline.key,b.pipeline.vs,b.pipeline.ps,b.pipeline.samples,b.pipeline.targets,b.calls,b.samples,b.skipped,b.ticks*1000.0/frequency_,mean,b.max_ticks*1000.0/frequency_,mean*b.calls);}
}
void em2::GpuDrawProfiler::ResetAfterIdle(){
    if(mapped_){D3D12_RANGE none{};readback_->Unmap(0,&none);mapped_=nullptr;}readback_.Reset();heap_.Reset();slots_={};buckets_.clear();work_calls_={};frequency_=completed_samples_=recorded_calls_=0;unavailable_=false;
}
void em2::ProfiledGpuList::ObserveShadowDraw(uint64_t elements)noexcept{
    if(!REXCVAR_GET(em2_trace_shadow_gpu_state) || !REXCVAR_GET(em2_private_profile))return;Fp fp;
    try{
        if(REXCVAR_GET(em2_user_data_directory).empty())return;
        const bool r32=pipeline_.color_format==DXGI_FORMAT_R32_FLOAT || pipeline_.color_format==DXGI_FORMAT_R32_UINT;
        const bool square_depth=!pipeline_.targets && pipeline_.depth_format && viewport_known_ && viewport_.Width==viewport_.Height && viewport_.Width>=128 && viewport_.Width<=2048;
        if(!r32 && !square_depth)return;
        static std::atomic<unsigned> reports{0};if(reports.fetch_add(1)>=64)return;
        REXLOG_INFO("Private shadow GPU draw: VS={:016X} PS={:016X} elements={} targets={} color_format={} write_mask={:02X} depth_format={} depth_enable={} depth_write={} depth_func={} cull={} front_ccw={} depth_clip={} bias={} slope={} clamp={} viewport_known={} viewport={},{},{},{} depth_range={},{} scissor_known={} scissor={},{},{},{}; actual forwarded draw, candidate identification only",pipeline_.vs,pipeline_.ps,elements,pipeline_.targets,pipeline_.color_format,pipeline_.write_mask,pipeline_.depth_format,pipeline_.depth_enable,pipeline_.depth_write,pipeline_.depth_func,pipeline_.cull,pipeline_.front_ccw,pipeline_.depth_clip,pipeline_.depth_bias,pipeline_.slope_bias,pipeline_.bias_clamp,viewport_known_,viewport_.TopLeftX,viewport_.TopLeftY,viewport_.Width,viewport_.Height,viewport_.MinDepth,viewport_.MaxDepth,scissor_known_,scissor_.left,scissor_.top,scissor_.right,scissor_.bottom);
    }catch(...){}
}
void em2::ProfiledGpuList::DrawInstanced(UINT count,UINT instances,UINT start,UINT first_instance){ObserveShadowDraw(uint64_t(count)*instances);const auto token=profile_.Begin(real_,submission_,pipeline_,GpuProfileWork::Draw,uint64_t(count)*instances);real_->DrawInstanced(count,instances,start,first_instance);profile_.End(real_,token);}
void em2::ProfiledGpuList::DrawIndexedInstanced(UINT count,UINT instances,UINT start,INT base,UINT first_instance){ObserveShadowDraw(uint64_t(count)*instances);const auto token=profile_.Begin(real_,submission_,pipeline_,GpuProfileWork::Draw,uint64_t(count)*instances);real_->DrawIndexedInstanced(count,instances,start,base,first_instance);profile_.End(real_,token);}
void em2::ProfiledGpuList::Dispatch(UINT x,UINT y,UINT z){const auto token=profile_.Begin(real_,submission_,pipeline_,GpuProfileWork::Dispatch,uint64_t(x)*y*z);real_->Dispatch(x,y,z);profile_.End(real_,token);}
void em2::ProfiledGpuList::CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* dst,UINT x,UINT y,UINT z,const D3D12_TEXTURE_COPY_LOCATION* src,const D3D12_BOX* box){
    uint64_t key=0xc0000002ull;
    {Fp fp;if(dst && src && dst->pResource && src->pResource)key=TextureCopyKey(*dst,dst->pResource->GetDesc(),*src,src->pResource->GetDesc());}
    const auto token=profile_.Begin(real_,submission_,GpuProfilePipeline{key},GpuProfileWork::Copy,0);
    real_->CopyTextureRegion(dst,x,y,z,src,box);profile_.End(real_,token);
}
bool em2::TestGpuCopyClassification(){
    auto check=[](bool ok,const char* text){if(!ok)REXLOG_ERROR("GPU copy classification: {}",text);return ok;};
    D3D12_TEXTURE_COPY_LOCATION dst{},src{};dst.Type=src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.SubresourceIndex=1;
    D3D12_RESOURCE_DESC d{},s{};d.Dimension=s.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.MipLevels=s.MipLevels=d.DepthOrArraySize=s.DepthOrArraySize=1;
    s.Format=DXGI_FORMAT_R8_UINT;s.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    for(auto format:{DXGI_FORMAT_R24G8_TYPELESS,DXGI_FORMAT_D24_UNORM_S8_UINT,DXGI_FORMAT_R32G8X24_TYPELESS,DXGI_FORMAT_D32_FLOAT_S8X24_UINT})for(unsigned samples:{1u,4u}){
        d.Format=format;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;d.SampleDesc.Count=s.SampleDesc.Count=samples;
        if(!check(TextureCopyKey(dst,d,src,s)==0xc0010003ull,"R8 to stencil plane"))return false;
        dst.SubresourceIndex=0;if(!check(TextureCopyKey(dst,d,src,s)==0xc0010004ull,"depth plane is not stencil"))return false;dst.SubresourceIndex=1;
        src.SubresourceIndex=1;if(!check(TextureCopyKey(dst,d,src,s)==0xc0010004ull,"source subresource guard"))return false;src.SubresourceIndex=0;
        d.MipLevels=2;if(!check(TextureCopyKey(dst,d,src,s)==0xc0010004ull,"mip-dependent plane indexing"))return false;d.MipLevels=1;
        d.DepthOrArraySize=2;if(!check(TextureCopyKey(dst,d,src,s)==0xc0010004ull,"array-dependent plane indexing"))return false;d.DepthOrArraySize=1;
        s.SampleDesc.Count=samples==1?4:1;if(!check(TextureCopyKey(dst,d,src,s)==0xc0010004ull,"sample mismatch"))return false;s.SampleDesc.Count=samples;
    }
    s.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    if(!check(TextureCopyKey(dst,d,src,s)==0xc0010001ull,"buffer to texture"))return false;
    std::swap(d,s);std::swap(dst,src);if(!check(TextureCopyKey(dst,d,src,s)==0xc0010002ull,"texture to buffer"))return false;
    d=s={};d.Dimension=s.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;if(!check(TextureCopyKey(dst,d,src,s)==0xc0010005ull,"color target"))return false;
    d.Flags=D3D12_RESOURCE_FLAG_NONE;if(!check(TextureCopyKey(dst,d,src,s)==0xc0010006ull,"generic texture"))return false;
    REXLOG_INFO("GPU copy classification passed: four stencil formats, 1x/4x MSAA, plane/source/mip/array/sample rejection, buffer/texture directions and color/generic copies");return true;
}
