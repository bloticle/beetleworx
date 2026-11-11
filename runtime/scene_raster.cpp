#include "scene_raster.h"
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/deferred_command_list.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/util/draw.h>
#include <rex/logging.h>
#include <bit>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>
namespace {
using CP=rex::graphics::d3d12::D3D12CommandProcessor;
using List=rex::graphics::d3d12::DeferredCommandList;
template<class Tag,auto Member>struct Access{friend auto MemberOf(Tag){return Member;}};
template<class Tag,class T>struct TypeAccess{friend auto TypeOf(Tag){return std::type_identity<T>{};}};
struct StreamTag{friend auto MemberOf(StreamTag);};template struct Access<StreamTag,&List::command_stream_>;
struct ViewportTag{friend auto MemberOf(ViewportTag);};template struct Access<ViewportTag,&CP::ff_viewport_>;
struct ScissorTag{friend auto MemberOf(ScissorTag);};template struct Access<ScissorTag,&CP::ff_scissor_>;
struct SystemTag{friend auto MemberOf(SystemTag);};template struct Access<SystemTag,&CP::system_constants_>;
struct CommandTag{friend auto TypeOf(CommandTag);};template struct TypeAccess<CommandTag,List::Command>;
struct HeaderTag{friend auto TypeOf(HeaderTag);};template struct TypeAccess<HeaderTag,List::CommandHeader>;
struct DrawTag{friend auto TypeOf(DrawTag);};template struct TypeAccess<DrawTag,List::D3DDrawInstancedArguments>;
struct IndexedTag{friend auto TypeOf(IndexedTag);};template struct TypeAccess<IndexedTag,List::D3DDrawIndexedInstancedArguments>;
using Command=decltype(TypeOf(CommandTag{}))::type;
using Header=decltype(TypeOf(HeaderTag{}))::type;
using Draw=decltype(TypeOf(DrawTag{}))::type;
using Indexed=decltype(TypeOf(IndexedTag{}))::type;
static_assert(sizeof(Header)==sizeof(uintmax_t));
struct DrawEvidence {uint32_t count=0;std::optional<em2::SceneRasterState::Draw> last;};
std::optional<DrawEvidence> Draws(std::span<const uintmax_t> stream){
    size_t cursor=0;DrawEvidence result;std::optional<D3D12_INDEX_BUFFER_VIEW> indices;
    while(cursor<stream.size()){
        Header header;std::memcpy(&header,&stream[cursor++],sizeof(header));
        if(header.command<Command::kD3DClearDepthStencilView || header.command>Command::kD3DSetSamplePositions)return {};
        if(header.arguments_size_elements>stream.size()-cursor)return {};
        const auto size=size_t(header.arguments_size_elements)*sizeof(uintmax_t);
        if(header.command==Command::kD3DIASetIndexBuffer){if(size<sizeof(D3D12_INDEX_BUFFER_VIEW))return {};D3D12_INDEX_BUFFER_VIEW view;std::memcpy(&view,&stream[cursor],sizeof(view));indices=view;}
        if(header.command==Command::kD3DDrawInstanced){if(size<sizeof(Draw))return {};Draw args;std::memcpy(&args,&stream[cursor],sizeof(args));result.last.reset();if(args.vertex_count_per_instance && args.instance_count){++result.count;result.last=em2::SceneRasterState::Draw{false,args.vertex_count_per_instance,args.instance_count,args.start_vertex_location,args.start_instance_location};}}
        else if(header.command==Command::kD3DDrawIndexedInstanced){if(size<sizeof(Indexed))return {};Indexed args;std::memcpy(&args,&stream[cursor],sizeof(args));result.last.reset();if(args.index_count_per_instance && args.instance_count){++result.count;
            if(indices && indices->BufferLocation && (indices->Format==DXGI_FORMAT_R16_UINT || indices->Format==DXGI_FORMAT_R32_UINT)){const uint32_t element=indices->Format==DXGI_FORMAT_R16_UINT?2:4;
                const uint64_t end=(uint64_t(args.start_index_location)+args.index_count_per_instance)*element;
                if(!(indices->BufferLocation%element) && end<=indices->SizeInBytes && uint64_t(indices->SizeInBytes)<=UINT64_MAX-indices->BufferLocation)result.last=em2::SceneRasterState::Draw{true,args.index_count_per_instance,args.instance_count,args.start_index_location,args.start_instance_location,args.base_vertex_location,indices};}
        }}
        cursor+=header.arguments_size_elements;
    }
    return result;
}
bool Finite(float v){return (std::bit_cast<uint32_t>(v)&0x7F800000u)!=0x7F800000u;}
}
em2::SceneDrawMark em2::MarkSceneDraw(CP& cp){return {cp.GetCurrentSubmission(),cp.GetCurrentFrame(),(cp.GetDeferredCommandList().*MemberOf(StreamTag{})).size()};}
bool em2::IsSceneColourRaster(const rex::graphics::RegisterFile& regs){
    using namespace rex::graphics;
    return regs.Get<reg::RB_MODECONTROL>().edram_mode==xenos::EdramMode::kColorDepth && regs.Get<reg::RB_SURFACE_INFO>().surface_pitch &&
        draw_util::IsRasterizationPotentiallyDone(regs,draw_util::IsPrimitivePolygonal(regs)) && (draw_util::GetNormalizedColorMask(regs,1)&15);
}
std::optional<em2::SceneRasterState> em2::InspectSceneDraw(CP& cp,SceneDrawMark mark){
    const auto& stream=cp.GetDeferredCommandList().*MemberOf(StreamTag{});
    if(cp.GetCurrentSubmission()!=mark.submission || cp.GetCurrentFrame()!=mark.frame || mark.stream_offset>stream.size())return {};
    const auto draws=Draws(std::span<const uintmax_t>(stream).subspan(mark.stream_offset));if(!draws || !draws->count || !draws->last)return {};
    SceneRasterState result;result.recorded_draws=draws->count;result.last_draw=*draws->last;result.viewport=cp.*MemberOf(ViewportTag{});result.scissor=cp.*MemberOf(ScissorTag{});
    const auto& system=cp.*MemberOf(SystemTag{});std::memcpy(result.ndc_scale.data(),system.ndc_scale,sizeof(system.ndc_scale));std::memcpy(result.ndc_offset.data(),system.ndc_offset,sizeof(system.ndc_offset));
    const auto& v=result.viewport;
    for(float n:{v.TopLeftX,v.TopLeftY,v.Width,v.Height,v.MinDepth,v.MaxDepth})if(!Finite(n))return {};
    for(unsigned i=0;i<3;++i)if(!Finite(result.ndc_scale[i]) || !Finite(result.ndc_offset[i]))return {};
    if(v.Width<=0 || v.Height<=0 || result.ndc_scale[0]==0 || result.ndc_scale[1]==0 || result.scissor.right<=result.scissor.left || result.scissor.bottom<=result.scissor.top)return {};
    return result;
}
bool em2::TestNativeSceneRaster(CP& cp){
    auto& list=cp.GetDeferredCommandList();auto& stream=list.*MemberOf(StreamTag{});auto saved=std::move(stream);
    const auto viewport=cp.*MemberOf(ViewportTag{});const auto scissor=cp.*MemberOf(ScissorTag{});const auto system=cp.*MemberOf(SystemTag{});
    struct Restore{CP& cp;decltype(stream)& commands;std::vector<uintmax_t> saved;D3D12_VIEWPORT viewport;D3D12_RECT scissor;decltype(system) system;
        ~Restore(){commands=std::move(saved);cp.*MemberOf(ViewportTag{})=viewport;cp.*MemberOf(ScissorTag{})=scissor;cp.*MemberOf(SystemTag{})=system;}} restore{cp,stream,std::move(saved),viewport,scissor,system};
    stream.clear();cp.*MemberOf(ViewportTag{})={1,2,640,360,0,1};cp.*MemberOf(ScissorTag{})={1,2,641,362};
    auto& constants=cp.*MemberOf(SystemTag{});for(unsigned i=0;i<3;++i){constants.ndc_scale[i]=i==1?-1.f:1.f;constants.ndc_offset[i]=float(i)*.125f;}
    auto mark=MarkSceneDraw(cp);list.D3DDispatch(1,1,1);if(InspectSceneDraw(cp,mark))return false;
    mark=MarkSceneDraw(cp);list.D3DDrawInstanced(0,1,0,0);if(InspectSceneDraw(cp,mark))return false;
    mark=MarkSceneDraw(cp);list.D3DDrawInstanced(3,1,0,0);auto result=InspectSceneDraw(cp,mark);
    if(!result || result->recorded_draws!=1 || result->viewport.TopLeftX!=1 || result->viewport.Width!=640 || result->scissor.bottom!=362 || result->ndc_scale[1]!=-1 || result->ndc_offset[2]!=.25f)return false;
    auto stale=mark;++stale.submission;if(InspectSceneDraw(cp,stale))return false;stale=mark;++stale.frame;if(InspectSceneDraw(cp,stale))return false;
    mark=MarkSceneDraw(cp);D3D12_INDEX_BUFFER_VIEW view{4096,32,DXGI_FORMAT_R16_UINT};list.D3DIASetIndexBuffer(&view);list.D3DDrawIndexedInstanced(6,2,3,-7,5);result=InspectSceneDraw(cp,mark);if(!result || !result->last_draw.indexed || result->last_draw.count!=6 || result->last_draw.instances!=2 || result->last_draw.start!=3 || result->last_draw.base_vertex!=-7 || result->last_draw.start_instance!=5 || result->last_draw.indices->BufferLocation!=4096)return false;
    (cp.*MemberOf(ViewportTag{})).Width=0;if(InspectSceneDraw(cp,mark))return false;(cp.*MemberOf(ViewportTag{})).Width=640;
    mark=MarkSceneDraw(cp);Header invalid{Command::kD3DDrawInstanced,UINT32_MAX};uintmax_t bits;std::memcpy(&bits,&invalid,sizeof(bits));stream.push_back(bits);if(InspectSceneDraw(cp,mark))return false;
    for(unsigned invalid=0;invalid<6;++invalid){mark=MarkSceneDraw(cp);auto bad=view;if(invalid==0)bad.BufferLocation=0;else if(invalid==1)bad.Format=DXGI_FORMAT_R32_FLOAT;else if(invalid==2)bad.SizeInBytes=8;else if(invalid==3)bad.BufferLocation=4097;else if(invalid==4)bad.BufferLocation=UINT64_MAX-3;
        if(invalid!=5)list.D3DIASetIndexBuffer(&bad);list.D3DDrawIndexedInstanced(6,1,0,0,0);if(InspectSceneDraw(cp,mark))return false;}
    mark=MarkSceneDraw(cp);list.D3DDrawInstanced(3,1,4,7);result=InspectSceneDraw(cp,mark);if(!result || result->last_draw.indexed || result->last_draw.start!=4 || result->last_draw.start_instance!=7)return false;
    list.D3DDrawInstanced(0,1,0,0);if(InspectSceneDraw(cp,mark))return false;
    REXLOG_INFO("Native scene raster tests passed: actual deferred draw records, indexed/nonindexed, new-command range, dispatch/zero count rejection, stale frame/submission, malformed extent and viewport rejection; value raster/NDC snapshot; no GPU execution or actual fog draw claimed");return true;
}
