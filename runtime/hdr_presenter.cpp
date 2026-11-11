#include "built_shaders.h"
#include "hdr_presenter.h"
#include "hdr_output.h"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <rex/logging.h>
using Microsoft::WRL::ComPtr;
namespace {
using Presenter=rex::ui::d3d12::D3D12Presenter;
thread_local bool creating_hdr=false;
thread_local bool legacy_refresh=false;
struct State{
 std::atomic<float> white{200},peak{1000};
 ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pipeline;
 ComPtr<ID3D12PipelineState> sdr_paint;
 ComPtr<ID3D12Resource> ui;ComPtr<ID3D12DescriptorHeap> rtv,srv;
 bool Initialize(ID3D12Device* device){
  if(pipeline)return true;
  
  ComPtr<ID3DBlob> error,serialized;
  const auto* vs=em2::GetBuiltShader(em2::BuiltShader::HdrUIVS);if(!vs)return false;const auto* ps=em2::GetBuiltShader(em2::BuiltShader::HdrUIPS);if(!ps)return false;
  const D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};D3D12_ROOT_PARAMETER params[2]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable={1,&range};params[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[1].Constants={0,0,1};params[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;const D3D12_ROOT_SIGNATURE_DESC desc{2,params};
  if(FAILED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error)) || FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root))))return false;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=root.Get();p.VS={vs->GetBufferPointer(),vs->GetBufferSize()};p.PS={ps->GetBufferPointer(),ps->GetBufferSize()};p.SampleMask=UINT_MAX;p.SampleDesc.Count=1;p.NumRenderTargets=1;p.RTVFormats[0]=em2::kHdrOutputFormat;p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;p.RasterizerState.DepthClipEnable=TRUE;p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
  auto& stencil=p.DepthStencilState.FrontFace;stencil.StencilFunc=D3D12_COMPARISON_FUNC_ALWAYS;stencil.StencilFailOp=stencil.StencilDepthFailOp=stencil.StencilPassOp=D3D12_STENCIL_OP_KEEP;p.DepthStencilState.BackFace=stencil;
  auto& b=p.BlendState.RenderTarget[0];b.BlendEnable=TRUE;b.SrcBlend=b.SrcBlendAlpha=D3D12_BLEND_ONE;b.DestBlend=b.DestBlendAlpha=D3D12_BLEND_INV_SRC_ALPHA;b.BlendOp=b.BlendOpAlpha=D3D12_BLEND_OP_ADD;b.RenderTargetWriteMask=15;
  return SUCCEEDED(device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&pipeline)));
 }
};
std::mutex mutex;std::map<const Presenter*,std::shared_ptr<State>> states;std::atomic<size_t> count{0};
std::shared_ptr<State> Find(const Presenter* p){if(!count.load(std::memory_order_acquire))return {};std::lock_guard lock(mutex);auto it=states.find(p);return it==states.end()?nullptr:it->second;}
}
std::unique_ptr<rex::ui::d3d12::D3D12Presenter> em2::CreateHdrPresenter(rex::ui::Presenter::HostGpuLossCallback callback,const rex::ui::d3d12::D3D12Provider& provider){
 struct Scope{bool previous=creating_hdr;Scope(){creating_hdr=true;}~Scope(){creating_hdr=previous;}}scope;
 return Presenter::Create(std::move(callback),provider);
}
void em2::RegisterHdrPresenter(const Presenter* p){if(!creating_hdr)return;std::lock_guard lock(mutex);states.emplace(p,std::make_shared<State>());count.store(states.size(),std::memory_order_release);}
void em2::ForgetHdrPresenter(const Presenter* p){std::lock_guard lock(mutex);states.erase(p);count.store(states.size(),std::memory_order_release);}
bool em2::IsHdrPresenter(const Presenter* p){return bool(Find(p));}
bool em2::SetHdrPresenterPaperWhite(const Presenter* p,float nits){auto s=Find(p);if(!s || !std::isfinite(nits) || nits<=0 || nits>1000)return false;s->white.store(nits,std::memory_order_relaxed);return true;}
float em2::HdrPresenterPaperWhite(const Presenter* p){auto s=Find(p);return s?s->white.load(std::memory_order_relaxed):200.f;}
bool em2::SetHdrPresenterPeak(const Presenter* p,float nits){auto s=Find(p);if(!s || !std::isfinite(nits) || nits<=0 || nits>10000)return false;s->peak.store(nits,std::memory_order_relaxed);return true;}
float em2::HdrPresenterPeak(const Presenter* p){auto s=Find(p);return s?std::max(s->white.load(std::memory_order_relaxed),s->peak.load(std::memory_order_relaxed)):1000.f;}
em2::HdrLegacyRefreshScope::HdrLegacyRefreshScope():previous_(legacy_refresh){legacy_refresh=true;}
em2::HdrLegacyRefreshScope::~HdrLegacyRefreshScope(){legacy_refresh=previous_;}
bool em2::IsHdrLegacyRefresh(){return legacy_refresh;}
ID3D12PipelineState* em2::HdrSdrPaintPipeline(const Presenter* p){auto s=Find(p);return s?s->sdr_paint.Get():nullptr;}
bool em2::CreateHdrSdrPaintPipeline(const Presenter* p,ID3D12Device* device,const D3D12_GRAPHICS_PIPELINE_STATE_DESC& original){
 auto s=Find(p);if(!s || !device)return false;if(s->sdr_paint)return true;
 
 
 ComPtr<ID3DBlob> error;const auto* shader=em2::GetBuiltShader(em2::BuiltShader::HdrSDRPaint);if(!shader)return false;
 auto desc=original;desc.PS={shader->GetBufferPointer(),shader->GetBufferSize()};desc.CachedPSO={};return SUCCEEDED(device->CreateGraphicsPipelineState(&desc,IID_PPV_ARGS(&s->sdr_paint)));
}
ID3D12Resource* em2::PrepareHdrPresenterUi(const Presenter* p,ID3D12Device* device,uint32_t w,uint32_t h,D3D12_CPU_DESCRIPTOR_HANDLE& rtv){
 auto s=Find(p);if(!s || !device || device!=p->provider().GetDevice() || !w || !h || w>16384 || h>16384 || !s->Initialize(device))return nullptr;
 if(s->ui && s->ui->GetDesc().Width==w && s->ui->GetDesc().Height==h){rtv=s->rtv->GetCPUDescriptorHandleForHeapStart();return s->ui.Get();}
 ComPtr<ID3D12Resource> image;ComPtr<ID3D12DescriptorHeap> rtvs,srvs;D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=Presenter::kSwapChainFormat;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
 if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&image))))return nullptr;
 const D3D12_DESCRIPTOR_HEAP_DESC a{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0},b{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
 if(FAILED(device->CreateDescriptorHeap(&a,IID_PPV_ARGS(&rtvs))) || FAILED(device->CreateDescriptorHeap(&b,IID_PPV_ARGS(&srvs))))return nullptr;
 device->CreateRenderTargetView(image.Get(),nullptr,rtvs->GetCPUDescriptorHandleForHeapStart());D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=d.Format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;device->CreateShaderResourceView(image.Get(),&view,srvs->GetCPUDescriptorHandleForHeapStart());
 s->ui=std::move(image);s->rtv=std::move(rtvs);s->srv=std::move(srvs);rtv=s->rtv->GetCPUDescriptorHandleForHeapStart();return s->ui.Get();
}
bool em2::DrawHdrPresenterUi(const Presenter* p,ID3D12GraphicsCommandList* list){
 auto s=Find(p);if(!s || !s->ui || !s->pipeline || !list)return false;
 const auto d=s->ui->GetDesc();D3D12_VIEWPORT vp{0,0,float(d.Width),float(d.Height),0,1};D3D12_RECT scissor{0,0,LONG(d.Width),LONG(d.Height)};list->RSSetViewports(1,&vp);list->RSSetScissorRects(1,&scissor);ID3D12DescriptorHeap* heaps[]={s->srv.Get()};list->SetDescriptorHeaps(1,heaps);list->SetGraphicsRootSignature(s->root.Get());list->SetPipelineState(s->pipeline.Get());list->SetGraphicsRootDescriptorTable(0,s->srv->GetGPUDescriptorHandleForHeapStart());const float white=s->white.load(std::memory_order_relaxed);list->SetGraphicsRoot32BitConstants(1,1,&white,0);list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);list->DrawInstanced(3,1,0,0);return true;
}
bool em2::HdrPresenterUiExtentMatches(const Presenter* p,uint32_t w,uint32_t h){auto s=Find(p);return s && s->ui && s->ui->GetDesc().Width==w && s->ui->GetDesc().Height==h;}
