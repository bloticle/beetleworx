#include "scene_replay.h"
#include "scene_raster_pass.h"
#include "../private/scene_shader_data.h"
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/pipeline_cache.h>
#include <rex/graphics/register_file.h>
#include <rex/logging.h>
#include <bit>
#include <cstring>
#include <memory>
#include <type_traits>
namespace {
using CP=rex::graphics::d3d12::D3D12CommandProcessor;
using Cache=rex::graphics::d3d12::PipelineCache;
using List=rex::graphics::d3d12::DeferredCommandList;
template<class Tag,auto Member>struct Access{friend auto MemberOf(Tag){return Member;}};
template<class Tag,class T>struct TypeAccess{friend auto TypeOf(Tag){return std::type_identity<T>{};}};
#define FIELD(tag,field) struct tag{friend auto MemberOf(tag);};template struct Access<tag,&CP::field>
FIELD(Pipeline,current_guest_pipeline_);FIELD(External,current_external_pipeline_);FIELD(System,cbuffer_binding_system_);FIELD(Float,cbuffer_binding_float_vertex_);FIELD(Bool,cbuffer_binding_bool_loop_);FIELD(Fetch,cbuffer_binding_fetch_);FIELD(Indices,cbuffer_binding_descriptor_indices_vertex_);FIELD(Shared,shared_memory_);FIELD(SharedUav,current_shared_memory_binding_is_uav_);FIELD(Constants,system_constants_);FIELD(Topology,primitive_topology_);FIELD(Registers,register_file_);FIELD(Vertex,active_vertex_shader_);FIELD(Pixel,active_pixel_shader_);
FIELD(Viewport,ff_viewport_);FIELD(Scissor,ff_scissor_);
#undef FIELD
struct PipelineType{friend auto TypeOf(PipelineType);};template struct TypeAccess<PipelineType,Cache::Pipeline>;
using NativePipeline=decltype(TypeOf(PipelineType{}))::type;
struct Stream{friend auto MemberOf(Stream);};template struct Access<Stream,&List::command_stream_>;
bool Address(D3D12_GPU_VIRTUAL_ADDRESS address){return address && !(address&255);}
std::array<D3D12_GPU_VIRTUAL_ADDRESS,5> ConstantAddresses(CP& cp){return {(cp.*MemberOf(System{})).address,(cp.*MemberOf(Float{})).address,(cp.*MemberOf(Bool{})).address,(cp.*MemberOf(Fetch{})).address,(cp.*MemberOf(Indices{})).up_to_date?(cp.*MemberOf(Indices{})).address:0};}
bool Bindings(CP& cp){return (cp.*MemberOf(System{})).up_to_date && (cp.*MemberOf(Float{})).up_to_date && (cp.*MemberOf(Bool{})).up_to_date && (cp.*MemberOf(Fetch{})).up_to_date && cp.*MemberOf(SharedUav{})==std::optional<bool>(false) && !((cp.*MemberOf(Constants{})).flags&rex::graphics::DxbcShaderTranslator::kSysFlag_SharedMemoryIsUAV);}
}
std::optional<em2::SceneReplayInputs> em2::CaptureSceneReplay(CP& cp,SceneDrawMark mark){
    const auto* regs=cp.*MemberOf(Registers{});const auto* pipeline=static_cast<const NativePipeline*>(cp.*MemberOf(Pipeline{}));
    if(!regs || !pipeline || cp.*MemberOf(External{}) || !Bindings(cp))return {};
    const auto& native=pipeline->description;auto* vertex=native.vertex_shader;auto* pixel=native.pixel_shader;
    if(!vertex || !pixel || !vertex->is_valid() || !pixel->is_valid() || &vertex->shader()!=cp.active_vertex_shader() || &pixel->shader()!=cp.active_pixel_shader())return {};
    auto scene=CaptureSceneDrawInputs(&vertex->shader(),&pixel->shader(),*regs,cp.GetCurrentFrame(),1);
    if(!scene || !IsSceneColourRaster(*regs))return {};scene->raster=InspectSceneDraw(cp,mark);if(!scene->raster)return {};
    const auto topology=cp.*MemberOf(Topology{});if(topology!=D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST && topology!=D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)return {};
    SceneReplayInputs result;result.scene=std::move(*scene);result.submission=cp.GetCurrentSubmission();result.frame=cp.GetCurrentFrame();result.command_end=(cp.GetDeferredCommandList().*MemberOf(Stream{})).size();result.pipeline_identity=pipeline;result.constants=ConstantAddresses(cp);result.topology=topology;
    for(unsigned i=0;i<4;++i)if(!Address(result.constants[i]))return {};if(result.constants[4] && !Address(result.constants[4]))return {};
    auto* shared=(cp.*MemberOf(Shared{})).get();if(!shared || !shared->GetBuffer() || !shared->GetGPUAddress())return {};
    result.shared_memory=shared->GetBuffer();result.shared_address=shared->GetGPUAddress();result.vertex_shader=vertex->translated_binary();
    if(result.vertex_shader.empty())return {};if(native.geometry_shader)result.geometry_shader=*native.geometry_shader;
    return result;
}
bool em2::IsSceneReplayCurrent(CP& cp,const SceneReplayInputs& replay){
    if(replay.submission!=cp.GetCurrentSubmission() || replay.frame!=cp.GetCurrentFrame() || replay.command_end!=(cp.GetDeferredCommandList().*MemberOf(Stream{})).size() || replay.pipeline_identity!=cp.*MemberOf(Pipeline{}) || cp.*MemberOf(External{}) || replay.topology!=cp.*MemberOf(Topology{}) || !Bindings(cp) || replay.constants!=ConstantAddresses(cp))return false;
    auto* shared=(cp.*MemberOf(Shared{})).get();if(!shared || replay.shared_memory!=shared->GetBuffer() || replay.shared_address!=shared->GetGPUAddress())return false;
    const auto* pipeline=static_cast<const NativePipeline*>(cp.*MemberOf(Pipeline{}));if(!pipeline)return false;const auto& native=pipeline->description;
    return native.vertex_shader && native.pixel_shader && &native.vertex_shader->shader()==cp.active_vertex_shader() && &native.pixel_shader->shader()==cp.active_pixel_shader() && replay.vertex_shader.data()==native.vertex_shader->translated_binary().data() && replay.vertex_shader.size()==native.vertex_shader->translated_binary().size() && (native.geometry_shader?replay.geometry_shader.data()==native.geometry_shader->data() && replay.geometry_shader.size()==native.geometry_shader->size():replay.geometry_shader.empty());
}
bool em2::TestNativeSceneReplay(CP& cp){
    using namespace rex::graphics;
    auto& stream=cp.GetDeferredCommandList().*MemberOf(Stream{});auto saved=std::move(stream);
    const auto pipeline=cp.*MemberOf(Pipeline{});const auto external=cp.*MemberOf(External{});const auto system=cp.*MemberOf(System{}),floats=cp.*MemberOf(Float{}),boolean=cp.*MemberOf(Bool{}),fetch=cp.*MemberOf(Fetch{}),indices=cp.*MemberOf(Indices{});
    const auto topology=cp.*MemberOf(Topology{});const auto uav=cp.*MemberOf(SharedUav{});const auto values=cp.*MemberOf(Constants{});const auto viewport=cp.*MemberOf(Viewport{});const auto scissor=cp.*MemberOf(Scissor{});auto* regs=cp.*MemberOf(Registers{});auto* vs=cp.active_vertex_shader();auto* ps=cp.active_pixel_shader();
    struct Restore{CP& cp;decltype(stream)& commands;std::vector<uintmax_t> saved;decltype(pipeline) pipeline;decltype(external) external;decltype(system) system,floats,boolean,fetch,indices;decltype(topology) topology;decltype(uav) uav;decltype(values) values;D3D12_VIEWPORT viewport;D3D12_RECT scissor;RegisterFile* regs;Shader* vs;Shader* ps;
        ~Restore(){commands=std::move(saved);cp.*MemberOf(Pipeline{})=pipeline;cp.*MemberOf(External{})=external;cp.*MemberOf(System{})=system;cp.*MemberOf(Float{})=floats;cp.*MemberOf(Bool{})=boolean;cp.*MemberOf(Fetch{})=fetch;cp.*MemberOf(Indices{})=indices;cp.*MemberOf(Topology{})=topology;cp.*MemberOf(SharedUav{})=uav;cp.*MemberOf(Constants{})=values;cp.*MemberOf(Viewport{})=viewport;cp.*MemberOf(Scissor{})=scissor;cp.*MemberOf(Registers{})=regs;cp.*MemberOf(Vertex{})=vs;cp.*MemberOf(Pixel{})=ps;}} restore{cp,stream,std::move(saved),pipeline,external,system,floats,boolean,fetch,indices,topology,uav,values,viewport,scissor,regs,vs,ps};
    stream.clear();auto registers=std::make_unique<RegisterFile>();std::memset(registers->values,0,sizeof(registers->values));
    // Reuse the tested native raster's viewport, but supply real register-layout metadata.
    registers->values[reg::RB_MODECONTROL::register_index]=uint32_t(xenos::EdramMode::kColorDepth);reg::RB_SURFACE_INFO surface{};surface.surface_pitch=32;registers->values[reg::RB_SURFACE_INFO::register_index]=surface.value;
    reg::VGT_DRAW_INITIATOR draw{};draw.prim_type=xenos::PrimitiveType::kTriangleList;draw.num_indices=3;registers->values[reg::VGT_DRAW_INITIATOR::register_index]=draw.value;registers->values[reg::RB_COLOR_MASK::register_index]=15;
    for(unsigned i=0;i<4;++i){registers->values[XE_GPU_REG_SHADER_CONSTANT_256_X+(13+i)*4+i]=std::bit_cast<uint32_t>(1.f);registers->values[XE_GPU_REG_SHADER_CONSTANT_000_X+255*4+i]=scene_shader_data::depth_fog_vertex_literals[i];registers->values[XE_GPU_REG_SHADER_CONSTANT_256_X+255*4+i]=std::bit_cast<uint32_t>(SceneDrawInputs::Row{0,1,3,2}[i]);}
    xenos::xe_gpu_texture_fetch_t texture{};texture.type=xenos::FetchConstantType::kTexture;texture.dimension=xenos::DataDimension::k2DOrStacked;for(unsigned i=0;i<2;++i)std::memcpy(&registers->values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0+i*6],&texture,sizeof(texture));
    xenos::xe_gpu_vertex_fetch_t buffer{};buffer.type=xenos::FetchConstantType::kVertex;buffer.address=1024;buffer.size=12;std::memcpy(&registers->values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0+95*2],&buffer,sizeof(buffer));
    const auto& vc=scene_shader_data::depth_fog_vertex_linked_example;const auto& pc=scene_shader_data::ScreenSpace_DepthFog_0;
    rex::graphics::d3d12::D3D12Shader vertex(xenos::ShaderType::kVertex,1,vc.data(),vc.size(),std::endian::native),pixel(xenos::ShaderType::kPixel,2,pc.data(),pc.size(),std::endian::native);rex::string::StringBuffer disasm;vertex.AnalyzeUcode(disasm);pixel.AnalyzeUcode(disasm);
    DxbcShaderTranslator translator(cp.GetD3D12Provider().GetAdapterVendorID(),false,false,false,true,1,1);auto* vt=static_cast<rex::graphics::d3d12::D3D12Shader::D3D12Translation*>(vertex.GetOrCreateTranslation(translator.GetDefaultVertexShaderModification(0)));auto* pt=static_cast<rex::graphics::d3d12::D3D12Shader::D3D12Translation*>(pixel.GetOrCreateTranslation(translator.GetDefaultPixelShaderModification(0)));if(!translator.TranslateAnalyzedShader(*vt) || !translator.TranslateAnalyzedShader(*pt))return false;
    NativePipeline native{};native.description.vertex_shader=vt;native.description.pixel_shader=pt;
    cp.*MemberOf(Pipeline{})=&native;cp.*MemberOf(External{})=nullptr;cp.*MemberOf(Registers{})=registers.get();cp.*MemberOf(Vertex{})=&vertex;cp.*MemberOf(Pixel{})=&pixel;cp.*MemberOf(Topology{})=D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;cp.*MemberOf(SharedUav{})=false;(cp.*MemberOf(Constants{})).flags&=~DxbcShaderTranslator::kSysFlag_SharedMemoryIsUAV;
    cp.*MemberOf(System{})={4096,true};cp.*MemberOf(Float{})={8192,true};cp.*MemberOf(Bool{})={12288,true};cp.*MemberOf(Fetch{})={16384,true};cp.*MemberOf(Indices{})={0,false};
    cp.*MemberOf(Viewport{})={0,0,16,16,0,1};cp.*MemberOf(Scissor{})={0,0,16,16};for(unsigned i=0;i<3;++i){(cp.*MemberOf(Constants{})).ndc_scale[i]=1;(cp.*MemberOf(Constants{})).ndc_offset[i]=0;}
    const auto mark=MarkSceneDraw(cp);cp.GetDeferredCommandList().D3DDrawInstanced(3,1,0,0);
    auto replay=CaptureSceneReplay(cp,mark);
    if(!replay){REXLOG_ERROR("Native scene replay fixture capture rejected; raster must be initialized");return false;}
    if(!IsSceneReplayCurrent(cp,*replay) || replay->constants[1]!=8192 || replay->vertex_shader.data()!=vt->translated_binary().data() || replay->scene.raster->last_draw.count!=3)return false;
    if(!TestNativeSceneSampler(cp,*replay))return false;
    cp.*MemberOf(Float{})={8192,false};if(IsSceneReplayCurrent(cp,*replay) || CaptureSceneReplay(cp,mark))return false;cp.*MemberOf(Float{})={8192,true};
    (cp.*MemberOf(System{})).address=4097;if(IsSceneReplayCurrent(cp,*replay) || CaptureSceneReplay(cp,mark))return false;(cp.*MemberOf(System{})).address=4096;
    cp.*MemberOf(SharedUav{})=true;if(IsSceneReplayCurrent(cp,*replay) || CaptureSceneReplay(cp,mark))return false;cp.*MemberOf(SharedUav{})=false;
    auto stale=*replay;++stale.submission;if(IsSceneReplayCurrent(cp,stale))return false;stale=*replay;++stale.frame;if(IsSceneReplayCurrent(cp,stale))return false;
    (cp.*MemberOf(Constants{})).flags|=DxbcShaderTranslator::kSysFlag_SharedMemoryIsUAV;if(IsSceneReplayCurrent(cp,*replay) || CaptureSceneReplay(cp,mark))return false;(cp.*MemberOf(Constants{})).flags&=~DxbcShaderTranslator::kSysFlag_SharedMemoryIsUAV;
    cp.*MemberOf(Topology{})=D3D_PRIMITIVE_TOPOLOGY_POINTLIST;if(IsSceneReplayCurrent(cp,*replay) || CaptureSceneReplay(cp,mark))return false;cp.*MemberOf(Topology{})=D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    cp.*MemberOf(Pixel{})=&vertex;if(IsSceneReplayCurrent(cp,*replay) || CaptureSceneReplay(cp,mark))return false;cp.*MemberOf(Pixel{})=&pixel;
    // GS bytecode is borrowed from the native cache, not decoded or executed by
    // this metadata test. Check identity/lifetime detection with a sentinel span.
    std::vector<uint32_t> geometry{1,2,3};native.description.geometry_shader=&geometry;auto with_geometry=CaptureSceneReplay(cp,mark);if(!with_geometry || with_geometry->geometry_shader.data()!=geometry.data() || !IsSceneReplayCurrent(cp,*with_geometry) || IsSceneReplayCurrent(cp,*replay))return false;native.description.geometry_shader=nullptr;if(IsSceneReplayCurrent(cp,*with_geometry))return false;
    cp.GetDeferredCommandList().D3DDispatch(1,1,1);if(IsSceneReplayCurrent(cp,*replay))return false;
    REXLOG_INFO("Native scene replay capture passed: actual translated native VS/PS, mock bound CBV values and real shared-memory identity, original shader gates, stale/incomplete binding rejection and command/submission/frame scope; commands not GPU-executed, no game draw or CBV memory authority claimed");return true;
}
