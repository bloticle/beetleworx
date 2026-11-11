#include "direct_color_resolve.h"
#include "built_shaders.h"
#include "gpu_draw_profile.h"
#include <rex/cvar.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <array>
#include <algorithm>
#include <sstream>
#include <rex/logging.h>
REXCVAR_DEFINE_BOOL(em2_direct_color_resolve,false,"Graphics","Experimental exact native RGBA8 1x-MSAA direct scaled resolves; unsupported ownership/layouts retain the original path");
REXCVAR_DEFINE_BOOL(em2_direct_color_resolve_survey,false,"Debug","Bounded CPU-only direct-resolve eligibility survey; preserves every rendering operation");
REXCVAR_DEFINE_INT32(em2_direct_color_resolve_survey_frames,120,"Debug","Eligibility survey native frames after at least 400 native draws in the prior frame").range(1,600);
namespace {
using Cache=rex::graphics::d3d12::D3D12RenderTargetCache;
std::mutex mutex;
std::unordered_map<const Cache*,std::shared_ptr<em2::DirectColorResolveProgram>> programs;
std::atomic<uint64_t> dispatches=0;
struct Survey {uint64_t frame=0,previous_frame_draws=0,start=0,last_report=0,total=0,fast=0,eligible=0,bytes=0,eligible_bytes=0;bool active=false;std::unordered_map<std::string,uint64_t> reasons;};
std::unordered_map<const Cache*,Survey> surveys;
void Report(const Cache* cache,const Survey& s,const char* boundary){
 std::ostringstream out;out<<"{\"owner\":"<<reinterpret_cast<uintptr_t>(cache)<<",\"boundary\":\""<<boundary<<"\",\"start_frame\":"<<s.start<<",\"frame\":"<<s.frame<<",\"resolves\":"<<s.total<<",\"fast32\":"<<s.fast<<",\"eligible\":"<<s.eligible<<",\"rgba32_scaled_rect_bytes\":"<<s.bytes<<",\"eligible_rgba32_scaled_rect_bytes\":"<<s.eligible_bytes<<",\"reasons\":{";bool first=true;for(const auto& [reason,count]:s.reasons){out<<(first?"":",")<<'\"'<<reason<<"\":"<<count;first=false;}out<<"},\"scope\":\"CPU ownership/representation observer only; rectangle bytes exclude destination padding and are not GPU time\"}";REXLOG_INFO("Direct color resolve survey {}",out.str());
}
}
bool em2::DirectColorResolveEnabled(){return REXCVAR_GET(em2_direct_color_resolve);}
bool em2::DirectColorResolveSurveyEnabled(){return REXCVAR_GET(em2_direct_color_resolve_survey);}
bool em2::DirectColorResolveCoverage(std::span<const DirectColorResolveRegion> regions,uint32_t width,uint32_t rows,uint32_t pitch){
 if(!width||!rows||!pitch||width>pitch||rows>2048||regions.size()>4096)return false;
 std::vector<std::vector<std::pair<uint32_t,uint32_t>>> spans(rows);
 for(const auto& r:regions){
  const uint32_t x=r.offset%pitch,y=r.offset/pitch;
  if(!r.width||!r.rows||uint64_t(x)+r.width>width||uint64_t(y)+r.rows>rows)return false;
  for(uint32_t i=0;i<r.rows;++i)spans[y+i].push_back({x,x+r.width});
 }
 for(auto& row:spans){std::sort(row.begin(),row.end());uint32_t next=0;for(auto [begin,end]:row){if(begin!=next)return false;next=end;}if(next!=width)return false;}
 return true;
}
void em2::ObserveDirectColorResolve(const Cache* cache,uint64_t frame,const rex::graphics::draw_util::ResolveInfo& info,bool fast32,bool eligible,const char* reason,uint32_t sx,uint32_t sy,uint32_t native_draws){
 if(!DirectColorResolveSurveyEnabled())return;
 std::lock_guard lock(mutex);auto& s=surveys[cache];
 if(s.frame&&frame<s.frame)s={};
 if(s.frame!=frame){
  if(!s.active&&s.previous_frame_draws>=400){s.active=true;s.start=frame;s.last_report=frame;}
  s.previous_frame_draws=0;s.frame=frame;
  if(s.active&&frame-s.start>=uint64_t(std::clamp(REXCVAR_GET(em2_direct_color_resolve_survey_frames),1,600))){Report(cache,s,"window_end");surveys.erase(cache);REXCVAR_SET(em2_direct_color_resolve_survey,false);return;}
  if(s.active&&frame-s.last_report>=30){Report(cache,s,"frame_summary");s.last_report=frame;}
 }
 s.previous_frame_draws=std::max(s.previous_frame_draws,uint64_t(native_draws));if(!s.active)return;
 ++s.total;s.fast+=fast32;s.eligible+=eligible;++s.reasons[reason];
 const uint64_t bytes=uint64_t(info.coordinate_info.width_div_8)*8*info.height_div_8*8*4*sx*sy;
 if(fast32)s.bytes+=bytes;if(eligible)s.eligible_bytes+=bytes;
}
bool em2::ValidateDirectColorResolve(const rex::graphics::draw_util::ResolveInfo& info,
 const DirectColorResolveSource& source,uint32_t sx,uint32_t sy,
 uint32_t base,uint32_t width,uint32_t rows,uint32_t pitch){
 using namespace rex::graphics;
 if(sx!=2||sy!=2||info.coordinate_info.draw_resolution_scale_x!=sx||info.coordinate_info.draw_resolution_scale_y!=sy||
    info.IsCopyingDepth()||info.color_edram_info.is_depth||
    info.color_edram_info.format!=0||info.color_edram_info.format_is_64bpp||
    info.color_edram_info.msaa_samples!=xenos::MsaaSamples::k1X||
    source.samples!=0||source.format!=0||!source.pitch||!width||!rows||!pitch||
    info.copy_dest_info.copy_dest_array)return false;
 draw_util::ResolveCopyShaderConstants constants;uint32_t gx,gy;
 if(info.GetCopyShader(sx,sy,constants,gx,gy)!=draw_util::ResolveCopyShaderIndex::kFast32bpp1x2xMSAA||!gx||!gy)return false;
 const auto& d=source.description;
 if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.DepthOrArraySize!=1||d.MipLevels!=1||d.SampleDesc.Count!=1||
    (d.Format!=DXGI_FORMAT_R8G8B8A8_TYPELESS&&d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM))return false;
 const uint64_t last=uint64_t(base)+uint64_t(rows-1)*pitch+width-1;
 if(base<source.base||last>=2048||width>pitch||source.base>=2048)return false;
 if(d.Width!=uint64_t(source.pitch)*80*sx)return false;
 const uint64_t native_bottom=((last-source.base)/source.pitch+1)*16*sy;
 return native_bottom<=d.Height;
}
std::shared_ptr<em2::DirectColorResolveProgram> em2::PrepareDirectColorResolve(const Cache* cache,ID3D12Device* device){
 std::lock_guard lock(mutex);auto found=programs.find(cache);if(found!=programs.end())return found->second;
 auto result=std::make_shared<DirectColorResolveProgram>();
 D3D12_DESCRIPTOR_RANGE ranges[2]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,1,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0}};
 D3D12_ROOT_PARAMETER parameters[4]{};
 for(unsigned i=0;i<2;++i){parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[i].Constants={i,0,4};}
 for(unsigned i=0;i<2;++i){parameters[i+2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[i+2].DescriptorTable={1,&ranges[i]};}
 D3D12_ROOT_SIGNATURE_DESC desc{4,parameters};Microsoft::WRL::ComPtr<ID3DBlob> serialized,error;
 if(FAILED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error))||
    FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&result->root))))return {};
 const auto* shader=GetBuiltShader(BuiltShader::DirectColorResolve);if(!shader)return {};
 D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=result->root.Get();pipeline.CS={shader->bytes,shader->size};
 if(FAILED(device->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&result->pipeline))))return {};
 TagGpuProfileComputePipeline(result->pipeline.Get(),shader->bytes,shader->size);
 programs.emplace(cache,result);return result;
}
void em2::ShutdownDirectColorResolve(const Cache* cache){std::lock_guard lock(mutex);programs.erase(cache);auto s=surveys.find(cache);if(s!=surveys.end()){if(s->second.active)Report(cache,s->second,"shutdown");surveys.erase(s);}}
void em2::RecordDirectColorResolve(){dispatches.fetch_add(1,std::memory_order_relaxed);}
uint64_t em2::DirectColorResolveDispatches(){return dispatches.load(std::memory_order_relaxed);}
