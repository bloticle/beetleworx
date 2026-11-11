#include "stencil_consumer_survey.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <mutex>
#include <iterator>
#include <map>
#include <unordered_set>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <cstring>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <sstream>
#include <unordered_map>
#include <vector>

REXCVAR_DEFINE_BOOL(em2_stencil_consumer_survey,false,"Debug","Observe stencil consumers and exact pending S8 region lifetimes; bounded CPU-only survey, no rendering changes");
REXCVAR_DEFINE_INT32(em2_stencil_consumer_survey_frames,120,"Debug","Native frames to observe after a dense frame (at least 400 render-cache updates)").range(1,600);
REXCVAR_DEFINE_STRING(em2_stencil_clear_probe_directory,"","Debug","Optional absolute private directory for at most64 clear-like draw input snapshots and shader microcode dumps; observation only");

namespace em2 {
namespace {
constexpr size_t kReasons=size_t(StencilSurveyReason::Count),kRegionLimit=512,kResourceLimit=256;
constexpr const char* kReasonNames[]={"guest_stencil_draw","depth_transfer_read","resolve_dump_read","resolve_clear","native_bit_overwrite","proven_zero_overwrite","native_stencil_reference_overwrite"};
static_assert(std::size(kReasonNames)==kReasons);
struct Piece {D3D12_RECT rect;uint64_t frame,event;};
uint64_t Area(D3D12_RECT r){return r.right>r.left && r.bottom>r.top?uint64_t(r.right-r.left)*uint64_t(r.bottom-r.top):0;}
D3D12_RECT Intersect(D3D12_RECT a,D3D12_RECT b){return {std::max(a.left,b.left),std::max(a.top,b.top),std::min(a.right,b.right),std::min(a.bottom,b.bottom)};}
void Outside(const Piece& p,D3D12_RECT cut,std::vector<Piece>& out){
  const auto r=p.rect,i=Intersect(r,cut);
  if(!Area(i)){out.push_back(p);return;}
  const D3D12_RECT pieces[]={{r.left,r.top,r.right,i.top},{r.left,i.bottom,r.right,r.bottom},{r.left,i.top,i.left,i.bottom},{i.right,i.top,r.right,i.bottom}};
  for(const auto& rect:pieces)if(Area(rect))out.push_back({rect,p.frame,p.event});
}
struct Totals {
  uint64_t draw_updates=0,rasterization_updates=0,stencil_enabled_updates=0;
  uint64_t no_read_no_write_updates=0,empty_scissor_updates=0,histogram_overflow_updates=0;
  uint64_t copy_batches=0,copy_commands=0,issued_bytes=0,superseded_bytes=0,superseded_cross_frame_bytes=0;
  uint64_t consumed_bytes=0,canceled_bytes=0,forced_bytes=0;
  uint64_t unidentified_resource_events=0;
  std::array<uint64_t,kReasons> events{},demand_events{},reason_bytes{},cross_frame_bytes{};
};
struct StencilUse {bool comparison_independent=false,writes_possible=false;};
StencilUse ClassifyStencil(const StencilSurveyDrawState& s){
  const auto control=s.normalized_depth_control;
  const bool back_enabled=(control&128)!=0;
  // D3D12 exposes shared front/back masks. Consider the union when separate
  // guest masks differ, rather than assuming either native implementation.
  const auto read_mask=((s.front_refmask|(back_enabled?s.back_refmask:0))>>8)&255;
  const auto write_mask=((s.front_refmask|(back_enabled?s.back_refmask:0))>>16)&255;
  const auto depth_func=(control>>4)&7;
  const bool depth_enabled=(control&2)!=0;
  const bool z_pass=!depth_enabled || depth_func!=0;
  const bool z_fail=depth_enabled && depth_func!=7;
  StencilUse result{true,false};
  for(unsigned face=0;face<(back_enabled?2u:1u);++face){
    const unsigned shift=face?20:8;
    const auto func=(control>>shift)&7,fail=(control>>(shift+3))&7;
    const auto pass=(control>>(shift+6))&7,zfail=(control>>(shift+9))&7;
    const bool independent=func==0 || func==7 || !read_mask;
    bool stencil_pass=true,stencil_fail=true;
    if(func==0){stencil_pass=false;}
    else if(func==7){stencil_fail=false;}
    else if(!read_mask){stencil_pass=func==2 || func==3 || func==6;stencil_fail=!stencil_pass;}
    result.comparison_independent&=independent;
    result.writes_possible|=write_mask && ((stencil_fail && fail!=0) || (stencil_pass && z_pass && pass!=0) || (stencil_pass && z_fail && zfail!=0));
  }
  return result;
}
struct DrawStateHistogram {
  uint64_t draws=0,materialized_bytes=0,scissor_sample_area=0;
  uint32_t front_reference_min=255,front_reference_max=0,back_reference_min=255,back_reference_max=0;
  bool comparison_independent=false,writes_possible=false;
  bool pixel_analyzed=false,pixel_kills=false,pixel_writes_depth=false;
  D3D12_RECT scissor_union{LONG_MAX,LONG_MAX,0,0};
};
struct Resource {
  uint64_t id=0;uint32_t width=0,height=0,samples=0;
  uint64_t issued=0,superseded=0,consumed=0,canceled=0,forced=0;
  std::vector<Piece> pending;
  std::array<uint64_t,kReasons> reason_events{},reason_bytes{};
  std::map<std::array<uint64_t,6>,DrawStateHistogram> draw_states;
  uint64_t Bytes()const{uint64_t area=0;for(const auto& p:pending)area+=Area(p.rect);return area*samples;}
  D3D12_RECT Bounds()const{return {0,0,LONG(width),LONG(height)};}
};
struct Model {
  Totals totals;
  std::unordered_map<uint64_t,Resource> resources;
  uint64_t event=0;
  Resource& Get(uint64_t id,uint32_t width,uint32_t height,uint32_t samples){
    auto [it,inserted]=resources.try_emplace(id);auto& r=it->second;
    if(inserted){r.id=id;r.width=width;r.height=height;r.samples=samples;}
    return r;
  }
  void Bound(Resource& r){
    if(r.pending.size()<=kRegionLimit)return;
    // Fragmentation never becomes an optimistic saving. Conservatively count
    // a model materialization of all remaining data and start a fresh epoch.
    const auto bytes=r.Bytes();totals.forced_bytes+=bytes;r.forced+=bytes;r.pending.clear();
  }
  void Copy(Resource& r,std::span<const D3D12_RECT> rects,uint64_t frame){
    ++totals.copy_batches;++event;
    for(auto rect:rects){
      rect=Intersect(rect,r.Bounds());if(!Area(rect))continue;
      ++totals.copy_commands;const auto bytes=Area(rect)*r.samples;
      totals.issued_bytes+=bytes;r.issued+=bytes;
      std::vector<Piece> next;next.reserve(r.pending.size()+4);
      for(const auto& p:r.pending){
        const auto overlap=Area(Intersect(p.rect,rect))*r.samples;
        totals.superseded_bytes+=overlap;r.superseded+=overlap;
        if(p.frame!=frame)totals.superseded_cross_frame_bytes+=overlap;
        Outside(p,rect,next);
      }
      next.push_back({rect,frame,event});r.pending=std::move(next);Bound(r);
    }
  }
  uint64_t Remove(Resource& r,std::span<const D3D12_RECT> rects,uint64_t frame,StencilSurveyReason reason,bool consume){
    const auto index=size_t(reason);++totals.events[index];++r.reason_events[index];uint64_t bytes=0,cross_frame=0;
    for(auto rect:rects){
      rect=Intersect(rect,r.Bounds());if(!Area(rect))continue;
      std::vector<Piece> next;next.reserve(r.pending.size()+4);
      for(const auto& p:r.pending){
        const auto overlap=Area(Intersect(p.rect,rect))*r.samples;
        bytes+=overlap;if(p.frame!=frame)cross_frame+=overlap;
        Outside(p,rect,next);
      }
      r.pending=std::move(next);Bound(r);
    }
    totals.reason_bytes[index]+=bytes;r.reason_bytes[index]+=bytes;totals.cross_frame_bytes[index]+=cross_frame;
    if(consume){totals.consumed_bytes+=bytes;r.consumed+=bytes;if(bytes)++totals.demand_events[index];}
    else{totals.canceled_bytes+=bytes;r.canceled+=bytes;}
    return bytes;
  }
  uint64_t Pending()const{uint64_t bytes=0;for(const auto& [id,r]:resources)bytes+=r.Bytes();return bytes;}
  bool Balanced()const{return totals.issued_bytes==totals.superseded_bytes+totals.consumed_bytes+totals.canceled_bytes+totals.forced_bytes+Pending();}
};
struct Session {
  bool active=false;
  uint64_t id=0,frame=0,submission=0,frame_updates=0,start_frame=0,start_submission=0,last_report_frame=0;
  Model model;
  size_t clear_probe_count=0;
  bool clear_probe_failed=false;
  std::unordered_map<uint64_t,std::unordered_set<uint64_t>> clear_signatures;
  std::unordered_set<uint64_t> dumped_programs;
  std::ofstream clear_file;
  std::filesystem::path clear_path;
};
std::mutex survey_mutex;
std::unordered_map<const void*,Session> sessions;
std::atomic<bool> has_sessions=false;
std::atomic<uint64_t> serial=0,resource_serial=0;
const GUID kSurveyResourceIdentity={0x8c23b5e1,0xe72a,0x4ced,{0x83,0x91,0x39,0xb3,0xc8,0x42,0x8e,0x15}};
std::string Hex(uint64_t n,unsigned width=16){std::ostringstream out;out<<std::hex<<std::setfill('0')<<std::setw(width)<<n;return out.str();}
void Report(const void* owner,const Session& s,const char* boundary){
  const auto& m=s.model;const auto& t=m.totals;
  std::ostringstream out;
  out<<"{\"session\":"<<s.id<<",\"owner\":"<<uint64_t(reinterpret_cast<uintptr_t>(owner))
     <<",\"boundary\":\""<<boundary<<"\",\"start_frame\":"<<s.start_frame<<",\"frame\":"<<s.frame
     <<",\"start_submission\":"<<s.start_submission<<",\"submission\":"<<s.submission
     <<",\"draw_updates\":"<<t.draw_updates<<",\"rasterization_updates\":"<<t.rasterization_updates
     <<",\"stencil_enabled_updates\":"<<t.stencil_enabled_updates<<",\"copy_batches\":"<<t.copy_batches
     <<",\"no_read_no_write_updates\":"<<t.no_read_no_write_updates<<",\"empty_scissor_updates\":"<<t.empty_scissor_updates
     <<",\"histogram_overflow_updates\":"<<t.histogram_overflow_updates
     <<",\"copy_commands\":"<<t.copy_commands<<",\"issued_s8_payload_bytes\":"<<t.issued_bytes
     <<",\"superseded_before_consumer_bytes\":"<<t.superseded_bytes<<",\"superseded_cross_frame_bytes\":"<<t.superseded_cross_frame_bytes
     <<",\"clear_or_native_overwrite_before_consumer_bytes\":"<<t.canceled_bytes
     <<",\"required_materialization_payload_bytes\":"<<t.consumed_bytes<<",\"forced_model_materialization_bytes\":"<<t.forced_bytes
     <<",\"pending_unresolved_bytes\":"<<m.Pending()<<",\"byte_accounting_balanced\":"<<(m.Balanced()?"true":"false")
     <<",\"unidentified_resource_events\":"<<t.unidentified_resource_events
     <<",\"clear_probe_records\":"<<s.clear_probe_count<<",\"clear_probe_failed\":"<<(s.clear_probe_failed?"true":"false")
     <<",\"reasons\":{";
  for(size_t i=0;i<kReasons;++i){if(i)out<<',';out<<'"'<<kReasonNames[i]<<"\":{\"events\":"<<t.events[i]<<",\"materialization_events\":"<<t.demand_events[i]<<",\"payload_bytes\":"<<t.reason_bytes[i]<<",\"cross_frame_payload_bytes\":"<<t.cross_frame_bytes[i]<<'}';}
  out<<"},\"resources\":[";bool first=true;
  for(const auto& [id,r]:m.resources){if(!r.issued && r.pending.empty() && r.draw_states.empty() && std::all_of(r.reason_events.begin(),r.reason_events.end(),[](auto n){return !n;}))continue;if(!first)out<<',';first=false;
    out<<"{\"id\":"<<id<<",\"width\":"<<r.width<<",\"height\":"<<r.height<<",\"samples\":"<<r.samples
       <<",\"issued_bytes\":"<<r.issued<<",\"superseded_bytes\":"<<r.superseded<<",\"consumed_bytes\":"<<r.consumed
       <<",\"canceled_bytes\":"<<r.canceled<<",\"pending_bytes\":"<<r.Bytes()<<",\"pending_regions\":"<<r.pending.size()<<",\"consumer_reasons\":{";
    for(size_t i=0;i<kReasons;++i){if(i)out<<',';out<<'"'<<kReasonNames[i]<<"\":{\"events\":"<<r.reason_events[i]<<",\"payload_bytes\":"<<r.reason_bytes[i]<<'}';}
    out<<"},\"draw_states\":[";bool first_state=true;
    for(const auto& [key,h]:r.draw_states){if(!first_state)out<<',';first_state=false;
      const auto scissor=h.scissor_sample_area?h.scissor_union:D3D12_RECT{};
      out<<"{\"normalized_depth_control\":"<<key[0]<<",\"front_masks\":"<<key[1]<<",\"back_masks\":"<<key[2]
         <<",\"vertex_shader_hash\":\""<<Hex(key[3])<<"\",\"pixel_shader_hash\":\""<<Hex(key[4])<<"\",\"draw_initiator\":"<<key[5]
         <<",\"pixel_analyzed\":"<<(h.pixel_analyzed?"true":"false")<<",\"pixel_kills\":"<<(h.pixel_kills?"true":"false")<<",\"pixel_writes_depth\":"<<(h.pixel_writes_depth?"true":"false")
         <<",\"front_reference_min\":"<<h.front_reference_min<<",\"front_reference_max\":"<<h.front_reference_max
         <<",\"back_reference_min\":"<<h.back_reference_min<<",\"back_reference_max\":"<<h.back_reference_max
         <<",\"draws\":"<<h.draws<<",\"comparison_independent\":"<<(h.comparison_independent?"true":"false")
         <<",\"writes_possible\":"<<(h.writes_possible?"true":"false")<<",\"scissor_sample_area_sum\":"<<h.scissor_sample_area
         <<",\"materialized_payload_bytes\":"<<h.materialized_bytes<<",\"scissor_union\":["<<scissor.left<<','<<scissor.top<<','<<scissor.right<<','<<scissor.bottom<<"]}";
    }
    out<<"]}";}
  out<<"],\"scope\":\"CPU-only lifetime model; rendering unchanged. Bytes are logical R8/S8 sample payload, not measured VRAM bandwidth or FPS. Stencil consumers use the SDK-derived clipped scissor; only provably comparison-independent AND no-write states are excluded. Other draws conservatively materialize every pending pixel in their scissor, including unknown coverage/depth/discard and partial writes; ALWAYS+REPLACE is never assumed full coverage. Separate guest masks use their union for conservative D3D12 shared-mask classification. Source reads use complete source tiles. Deferred implementation requires distinct per-destination snapshots; none are allocated or pinned. End-window pending bytes have unknown future consumers.\"}";
  REXLOG_INFO("Stencil consumer survey {}",out.str());
}
Session* Enter(StencilSurveyContext c){
  auto& s=sessions[c.owner];has_sessions=true;
  if(s.frame && c.frame<s.frame){if(s.active)Report(c.owner,s,"frame_reset_unresolved");s={};}
  if(s.frame!=c.frame){
    if(!s.active && s.frame_updates>=400){
      s.active=true;s.id=++serial;s.start_frame=c.frame;s.start_submission=c.submission;s.last_report_frame=c.frame;
      REXLOG_INFO("Stencil consumer survey session {} begins at native frame {} submission {}, after dense frame {} had {} cache updates; observer only, rendering unchanged",s.id,c.frame,c.submission,s.frame,s.frame_updates);
    }
    s.frame_updates=0;s.frame=c.frame;s.submission=c.submission;
    if(s.active && c.frame-s.start_frame>=uint64_t(std::clamp(REXCVAR_GET(em2_stencil_consumer_survey_frames),1,600))){
      Report(c.owner,s,"window_end_unresolved");sessions.erase(c.owner);has_sessions=!sessions.empty();REXCVAR_SET(em2_stencil_consumer_survey,false);return nullptr;
    }
    if(s.active && c.frame-s.last_report_frame>=30){Report(c.owner,s,"frame_summary");s.last_report_frame=c.frame;}
  }
  s.submission=c.submission;return &s;
}
Resource* Identify(Session& s,ID3D12Resource* resource){
  if(!resource)return nullptr;
  uint64_t id=0;UINT size=sizeof(id);
  if(FAILED(resource->GetPrivateData(kSurveyResourceIdentity,&size,&id)) || !id){id=++resource_serial;if(FAILED(resource->SetPrivateData(kSurveyResourceIdentity,sizeof(id),&id))){++s.model.totals.unidentified_resource_events;return nullptr;}}
  if(!s.model.resources.contains(id) && s.model.resources.size()>=kResourceLimit){++s.model.totals.unidentified_resource_events;return nullptr;}
  const auto desc=resource->GetDesc();
  return &s.model.Get(id,uint32_t(desc.Width),desc.Height,desc.SampleDesc.Count);
}
// Same narrowly scoped member-access mechanism as the native renderer fixture;
// the installed SDK exposes shaders/registers but not its CPU physical backing.
template<class Tag,auto Member>struct ProbeAccess{friend auto ProbeMember(Tag){return Member;}};
struct ProbeMemoryTag{friend auto ProbeMember(ProbeMemoryTag);};
template struct ProbeAccess<ProbeMemoryTag,&rex::graphics::CommandProcessor::memory_>;
std::vector<uint8_t> ReadProbeBytes(const rex::memory::Memory& memory,uint32_t address,size_t bytes){
  bytes=std::min<size_t>(bytes,512);if(!bytes)return {};
  auto* source=memory.TranslatePhysical(address);std::vector<uint8_t> result(bytes);
  size_t copied=0;
  while(copied<bytes){
    MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(source+copied,&info,sizeof(info)) || info.State!=MEM_COMMIT || (info.Protect&(PAGE_NOACCESS|PAGE_GUARD)))return {};
    const auto end=reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize;
    const auto current=reinterpret_cast<uintptr_t>(source+copied);
    if(end<=current)return {};
    const auto count=std::min(bytes-copied,size_t(end-current));SIZE_T read=0;
    if(!ReadProcessMemory(GetCurrentProcess(),source+copied,result.data()+copied,count,&read) || read!=count)return {};
    copied+=count;
  }
  return result;
}
}
bool StencilSurveyEnabled(){
  if(REXCVAR_GET(em2_stencil_consumer_survey))return true;
  if(has_sessions.load(std::memory_order_relaxed)){
    std::lock_guard lock(survey_mutex);
    for(const auto& [owner,s]:sessions)if(s.active)Report(owner,s,"disabled_unresolved");
    sessions.clear();has_sessions=false;
  }
  return false;
}
void StencilSurveyDraw(StencilSurveyContext c,bool rasterization,bool stencil_enabled,ID3D12Resource* depth,const StencilSurveyDrawState* state){
  if(!StencilSurveyEnabled())return;
  std::lock_guard lock(survey_mutex);auto* s=Enter(c);if(!s)return;++s->frame_updates;
  if(!s->active)return;
  auto& t=s->model.totals;++t.draw_updates;t.rasterization_updates+=rasterization;
  if(!rasterization || !stencil_enabled || !depth)return;
  ++t.stencil_enabled_updates;auto* r=Identify(*s,depth);if(!r)return;
  const auto rect=state?Intersect(state->scissor,r->Bounds()):r->Bounds();
  const auto use=state?ClassifyStencil(*state):StencilUse{false,true};
  const bool no_read_no_write=use.comparison_independent && !use.writes_possible;
  t.no_read_no_write_updates+=no_read_no_write;t.empty_scissor_updates+=!Area(rect);
  uint64_t bytes=0;
  if(!no_read_no_write)bytes=s->model.Remove(*r,{&rect,1},c.frame,StencilSurveyReason::GuestStencilDraw,true);
  if(state){
    const std::array<uint64_t,6> key{state->normalized_depth_control,state->front_refmask&0x00FFFF00u,state->back_refmask&0x00FFFF00u,state->vertex_shader_hash,state->pixel_shader_hash,state->draw_initiator};
    auto it=r->draw_states.find(key);
    if(it==r->draw_states.end()){
      if(r->draw_states.size()>=128){++t.histogram_overflow_updates;return;}
      it=r->draw_states.try_emplace(key).first;
    }
    auto& h=it->second;++h.draws;h.materialized_bytes+=bytes;h.comparison_independent=use.comparison_independent;h.writes_possible=use.writes_possible;
    h.pixel_analyzed=state->pixel_analyzed;h.pixel_kills=state->pixel_kills;h.pixel_writes_depth=state->pixel_writes_depth;
    h.front_reference_min=std::min(h.front_reference_min,state->front_refmask&255);h.front_reference_max=std::max(h.front_reference_max,state->front_refmask&255);
    h.back_reference_min=std::min(h.back_reference_min,state->back_refmask&255);h.back_reference_max=std::max(h.back_reference_max,state->back_refmask&255);
    h.scissor_sample_area+=Area(rect)*r->samples;
    if(Area(rect)){h.scissor_union.left=std::min(h.scissor_union.left,rect.left);h.scissor_union.top=std::min(h.scissor_union.top,rect.top);h.scissor_union.right=std::max(h.scissor_union.right,rect.right);h.scissor_union.bottom=std::max(h.scissor_union.bottom,rect.bottom);}
  }
}
void StencilSurveyCopy(StencilSurveyContext c,ID3D12Resource* resource,std::span<const D3D12_RECT> rects){
  if(!StencilSurveyEnabled())return;
  std::lock_guard lock(survey_mutex);auto* s=Enter(c);if(!s || !s->active)return;auto* r=Identify(*s,resource);if(r)s->model.Copy(*r,rects,c.frame);
}
void StencilSurveyRead(StencilSurveyContext c,ID3D12Resource* resource,std::span<const D3D12_RECT> rects,StencilSurveyReason reason,bool whole){
  if(!StencilSurveyEnabled())return;
  std::lock_guard lock(survey_mutex);auto* s=Enter(c);if(!s || !s->active)return;auto* r=Identify(*s,resource);if(!r)return;
  const auto bounds=r->Bounds();s->model.Remove(*r,whole?std::span<const D3D12_RECT>(&bounds,1):rects,c.frame,reason,true);
}
void StencilSurveyOverwrite(StencilSurveyContext c,ID3D12Resource* resource,std::span<const D3D12_RECT> rects,StencilSurveyReason reason){
  if(!StencilSurveyEnabled())return;
  std::lock_guard lock(survey_mutex);auto* s=Enter(c);if(!s || !s->active)return;auto* r=Identify(*s,resource);if(r)s->model.Remove(*r,rects,c.frame,reason,false);
}
void StencilSurveyProbeClear(StencilSurveyContext c,rex::graphics::CommandProcessor& cp,
    const rex::graphics::RegisterFile& regs,const rex::graphics::Shader& vertex_shader,
    ID3D12Resource* depth,uint32_t control,uint32_t color_mask){
  namespace g=rex::graphics;
  if(control!=0x8777 || !StencilSurveyEnabled() || REXCVAR_GET(em2_stencil_clear_probe_directory).empty())return;
  std::lock_guard lock(survey_mutex);auto* s=Enter(c);if(!s || !s->active || s->clear_probe_failed || s->clear_probe_count>=64)return;
  auto* r=Identify(*s,depth);if(!r)return;auto& signatures=s->clear_signatures[r->id];if(signatures.size()>=8)return;
  const auto* memory=cp.*ProbeMember(ProbeMemoryTag{});if(!memory)return;
  try{
    if(!s->clear_file.is_open()){
      const std::filesystem::path directory=REXCVAR_GET(em2_stencil_clear_probe_directory);
      if(!directory.is_absolute()){s->clear_probe_failed=true;REXLOG_WARN("Stencil clear probe requires an absolute private directory; rendering unchanged");return;}
      std::error_code error;std::filesystem::create_directories(directory,error);if(error){s->clear_probe_failed=true;return;}
      s->clear_path=directory/("clear-inputs-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(s->id)+".jsonl");
      s->clear_file.open(s->clear_path,std::ios::app);if(!s->clear_file){s->clear_probe_failed=true;return;}
      REXLOG_INFO("Stencil clear probe session {}: private input snapshots at {} (at most64 total,8 per resource; no GPU reads or changes)",s->id,s->clear_path.string());
    }
    std::ostringstream data;
    data<<"{\"normalized_depth_control\":"<<control<<",\"normalized_color_mask\":"<<color_mask
        <<",\"resource_id\":"<<r->id<<",\"width\":"<<r->width<<",\"height\":"<<r->height<<",\"samples\":"<<r->samples;
    auto shader_info=[&](const char* name,const g::Shader* shader){
      data<<",\""<<name<<"\":";
      if(!shader){data<<"null";return;}
      const auto hash=shader->ucode_data_hash();
      data<<"{\"hash\":\""<<Hex(hash)<<"\",\"analyzed\":"<<(shader->is_ucode_analyzed()?"true":"false")
          <<",\"dwords\":"<<shader->ucode_dword_count()<<",\"kills_pixels\":"<<(shader->kills_pixels()?"true":"false")
          <<",\"writes_depth\":"<<(shader->writes_depth()?"true":"false")<<",\"memexport_mask\":"<<uint32_t(shader->memexport_eM_written())
          <<",\"vertex_kill_export_mask\":"<<shader->writes_point_size_edge_flag_kill_vertex()
          <<",\"uses_texture_results\":"<<(shader->uses_texture_fetch_instruction_results()?"true":"false")<<'}';
      if(s->dumped_programs.insert(hash).second)shader->DumpUcode(s->clear_path.parent_path());
    };
    shader_info("vertex_shader",&vertex_shader);shader_info("active_pixel_shader",cp.active_pixel_shader());
    const std::pair<const char*,uint32_t> raw_regs[]={
      {"VGT_DRAW_INITIATOR",g::XE_GPU_REG_VGT_DRAW_INITIATOR},{"VGT_INDX_OFFSET",g::XE_GPU_REG_VGT_INDX_OFFSET},
      {"VGT_MIN_VTX_INDX",g::XE_GPU_REG_VGT_MIN_VTX_INDX},{"VGT_MAX_VTX_INDX",g::XE_GPU_REG_VGT_MAX_VTX_INDX},
      {"VGT_DMA_BASE",g::XE_GPU_REG_VGT_DMA_BASE},{"VGT_DMA_SIZE",g::XE_GPU_REG_VGT_DMA_SIZE},
      {"PA_CL_VTE_CNTL",g::XE_GPU_REG_PA_CL_VTE_CNTL},{"PA_CL_CLIP_CNTL",g::XE_GPU_REG_PA_CL_CLIP_CNTL},
      {"PA_SU_VTX_CNTL",g::XE_GPU_REG_PA_SU_VTX_CNTL},{"PA_SU_SC_MODE_CNTL",g::XE_GPU_REG_PA_SU_SC_MODE_CNTL},
      {"PA_CL_VPORT_XSCALE",g::XE_GPU_REG_PA_CL_VPORT_XSCALE},{"PA_CL_VPORT_XOFFSET",g::XE_GPU_REG_PA_CL_VPORT_XOFFSET},
      {"PA_CL_VPORT_YSCALE",g::XE_GPU_REG_PA_CL_VPORT_YSCALE},{"PA_CL_VPORT_YOFFSET",g::XE_GPU_REG_PA_CL_VPORT_YOFFSET},
      {"PA_CL_VPORT_ZSCALE",g::XE_GPU_REG_PA_CL_VPORT_ZSCALE},{"PA_CL_VPORT_ZOFFSET",g::XE_GPU_REG_PA_CL_VPORT_ZOFFSET},
      {"PA_SC_WINDOW_OFFSET",g::XE_GPU_REG_PA_SC_WINDOW_OFFSET},{"PA_SC_WINDOW_SCISSOR_TL",g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL},
      {"PA_SC_WINDOW_SCISSOR_BR",g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR},{"PA_SC_SCREEN_SCISSOR_TL",g::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL},
      {"PA_SC_SCREEN_SCISSOR_BR",g::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR},{"RB_SURFACE_INFO",g::XE_GPU_REG_RB_SURFACE_INFO},
      {"PA_SC_AA_MASK",g::XE_GPU_REG_PA_SC_AA_MASK},{"SQ_CONTEXT_MISC",g::XE_GPU_REG_SQ_CONTEXT_MISC},
      {"RB_DEPTH_INFO",g::XE_GPU_REG_RB_DEPTH_INFO},{"RB_COLORCONTROL",g::XE_GPU_REG_RB_COLORCONTROL},
      {"RB_ALPHA_REF",g::XE_GPU_REG_RB_ALPHA_REF},{"RB_STENCILREFMASK",g::XE_GPU_REG_RB_STENCILREFMASK},
      {"RB_STENCILREFMASK_BF",g::XE_GPU_REG_RB_STENCILREFMASK_BF},{"SQ_PROGRAM_CNTL",g::XE_GPU_REG_SQ_PROGRAM_CNTL},
      {"SQ_VS_CONST",g::XE_GPU_REG_SQ_VS_CONST},{"SQ_PS_CONST",g::XE_GPU_REG_SQ_PS_CONST}};
    data<<",\"registers\":{";bool first=true;
    for(const auto& [name,index]:raw_regs){if(!first)data<<',';first=false;data<<'"'<<name<<"\":"<<regs[index];}
    data<<"},\"vertex_fetches\":[";first=true;
    for(const auto& binding:vertex_shader.vertex_bindings()){
      if(binding.fetch_constant>=96)continue;
      const auto fetch=regs.GetVertexFetch(binding.fetch_constant);
      const auto address=uint32_t(fetch.address)<<2;
      const auto bytes=ReadProbeBytes(*memory,address,std::min<uint64_t>(uint64_t(fetch.size)*4,512));
      if(!first)data<<',';first=false;
      data<<"{\"fetch_constant\":"<<binding.fetch_constant<<",\"stride_words\":"<<binding.stride_words
          <<",\"raw\":["<<fetch.dword_0<<','<<fetch.dword_1<<"],\"physical_address\":"<<address<<",\"size_words\":"<<uint32_t(fetch.size)
          <<",\"endian\":"<<uint32_t(fetch.endian)<<",\"cpu_bytes_hex\":\"";
      for(auto byte:bytes)data<<Hex(byte,2);
      data<<"\",\"cpu_readable\":"<<(!bytes.empty()?"true":"false")<<'}';
    }
    // Capture all constants so nondefault SQ_VS/PS_CONST bank bases or dynamic
    // addressing cannot silently select uncaptured inputs. This is bounded to
    //64 snapshots, not a per-draw log of the game.
    data<<"],\"float_constants_raw\":[";
    for(uint32_t i=0;i<512;++i){if(i)data<<',';data<<'[';for(uint32_t j=0;j<4;++j){if(j)data<<',';data<<regs[g::XE_GPU_REG_SHADER_CONSTANT_000_X+4*i+j];}data<<']';}
    data<<"]}";const auto payload=data.str();uint64_t signature=1469598103934665603ull;
    for(unsigned char byte:payload){signature^=byte;signature*=1099511628211ull;}
    if(!signatures.insert(signature).second)return;
    s->clear_file<<"{\"session\":"<<s->id<<",\"frame\":"<<c.frame<<",\"submission\":"<<c.submission
        <<",\"input_signature\":\""<<Hex(signature)<<"\",\"input\":"<<payload
        <<",\"scope\":\"Private CPU input observation, no rendering changes. Active PS may be stale/not bound; shader flags alone do not prove actual coverage. Physical vertex bytes are validated committed CPU backing, not GPU readback. Geometry, viewport, clipping, discard, depth and write-mask proof remain required.\"}\n";
    s->clear_file.flush();if(!s->clear_file){s->clear_probe_failed=true;return;}++s->clear_probe_count;
  }catch(const std::exception& error){s->clear_probe_failed=true;REXLOG_WARN("Stencil clear probe stopped after input observation error: {}; rendering unchanged",error.what());}
}
void ShutdownStencilSurvey(const void* owner){
  if(!has_sessions.load(std::memory_order_relaxed))return;
  std::lock_guard lock(survey_mutex);auto it=sessions.find(owner);if(it==sessions.end())return;
  if(it->second.active)Report(owner,it->second,"cache_shutdown_unresolved");sessions.erase(it);has_sessions=!sessions.empty();
}
bool TestStencilConsumerSurvey(){
  const auto classify=[](uint32_t control,uint32_t front,uint32_t back,bool independent,bool writes){
    const auto result=ClassifyStencil({control,front,back,{}});
    return result.comparison_independent==independent && result.writes_possible==writes;
  };
  constexpr uint32_t full=0x00FFFF00,always=1|(7<<8),never=1,equal=1|(2<<8);
  const bool classification_ok=
      classify(always,full,full,true,false) &&
      classify(equal,full,full,false,false) &&
      classify(never|(2<<14),full,full,true,false) &&
      classify(never|(2<<11),full,full,true,true) &&
      classify(always|(2<<14),full,full,true,true) &&
      classify(equal|(2<<11),0x00FF0000,0,true,false) &&
      classify(1|(1<<8)|(2<<14),0x00FF0000,0,true,false) &&
      classify(1|(4<<8)|(1<<11),0x00FF0000,0,true,true) &&
      classify(1|(1<<8),0x0000FF00,0,false,false) &&
      classify(always|(3<<14),0x0000FF00,0,true,false) &&
      classify(always|128|(2<<20),0x0000FF00,0,false,false) &&
      classify(always|(2<<20),full,full,true,false) &&
      classify(always|2|(2<<14),full,full,true,false) &&
      classify(always|2|(7<<4)|(2<<17),full,full,true,false) &&
      classify(always|2|(1<<4)|(2<<17),full,full,true,true) &&
      classify(always|(2<<14),0x00F0FF00,0,true,true);
  if(!classification_ok){REXLOG_ERROR("Stencil consumer survey reachable front/back/depth state classification failed");return false;}
  Model model;std::array<std::array<bool,64>,2> pending{};
  std::array<uint64_t,2> samples{1,4};uint64_t issued=0,superseded=0,consumed=0,canceled=0;uint32_t random=0x713d284b;
  auto next=[&](){random=random*1664525u+1013904223u;return random>>16;};
  for(unsigned i=0;i<20000;++i){
    const auto id=next()%2;auto& r=model.Get(id+1,8,8,uint32_t(samples[id]));
    const auto x=next()%8,y=next()%8,w=1+next()%(8-x),h=1+next()%(8-y),op=next()%3;
    const D3D12_RECT rect{LONG(x),LONG(y),LONG(x+w),LONG(y+h)};
    for(unsigned row=y;row<y+h;++row)for(unsigned col=x;col<x+w;++col){auto& value=pending[id][row*8+col];if(op==0){issued+=samples[id];superseded+=value?samples[id]:0;value=true;}else{if(value){if(op==1)consumed+=samples[id];else canceled+=samples[id];}value=false;}}
    if(op==0)model.Copy(r,{&rect,1},i/20);else model.Remove(r,{&rect,1},i/20,op==1?StencilSurveyReason::DepthTransferRead:StencilSurveyReason::ResolveClear,op==1);
    uint64_t bytes=0;for(unsigned k=0;k<2;++k)for(bool value:pending[k])bytes+=value?samples[k]:0;
    if(!model.Balanced() || model.Pending()!=bytes || model.totals.issued_bytes!=issued || model.totals.superseded_bytes!=superseded || model.totals.consumed_bytes!=consumed || model.totals.canceled_bytes!=canceled){REXLOG_ERROR("Stencil consumer survey independent bitmap/accounting mismatch at event {}",i);return false;}
  }
  REXLOG_INFO("Stencil consumer survey: 16 reachable front/back/depth/mask classification cases and 20000 independent bitmap lifetime events across two resources and 1x/4x samples, exact partial overwrites/read regions, cross-frame epochs and byte conservation passed; observer allocates no GPU resources");
  return true;
}
}
