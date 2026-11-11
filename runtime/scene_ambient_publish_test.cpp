#include "scene_lighting.h"
#include "gpu_constants_internal.h"
#include "hdr_scene_cache.h"
#include <rex/cvar.h>
#include <rex/graphics/d3d12/render_target_cache.h>
#include "scene_geometry_targets.h"
#include "native_pixel_fixture.h"
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/texture_cache.h>
#include <rex/graphics/d3d12/deferred_command_list.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/thread/mutex.h>
#include <rex/logging.h>
#include <d3d12sdklayers.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
REXCVAR_DECLARE(bool,em2_apply_scene_lighting);
REXCVAR_DECLARE(bool,em2_apply_scene_indirect);
#include "material_capture.h"
namespace {
using namespace em2;using namespace em2::pixel_test;
using CP=rex::graphics::d3d12::D3D12CommandProcessor;using Cache=rex::graphics::d3d12::D3D12RenderTargetCache;using List=rex::graphics::d3d12::DeferredCommandList;
template<class Tag,auto Member>struct Access{friend auto MemberOf(Tag){return Member;}};
#define FIELD(tag,type,field) struct tag{friend auto MemberOf(tag);};template struct Access<tag,&type::field>
FIELD(Stream,List,command_stream_);FIELD(External,CP,current_external_pipeline_);FIELD(Mode,CP,bindless_resources_used_);FIELD(ViewHeap,CP,view_bindful_heap_current_);FIELD(BindlessHeap,CP,view_bindless_heap_);
FIELD(CacheOwner,CP,render_target_cache_);FIELD(Memory,rex::graphics::CommandProcessor,memory_);FIELD(Shared,CP,shared_memory_);FIELD(Lighting,Em2CommandProcessor,scene_lighting_);FIELD(Failed,Em2CommandProcessor,scene_lighting_failed_);
FIELD(Targets,SceneGeometryBindings,targets_);FIELD(Latest,SceneLightingState,latest_);FIELD(Pending,SceneLightingState,pending_);
#undef FIELD
constexpr unsigned stride=2048,side=8;constexpr auto read=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
struct PublicationFixture {
    ComPtr<ID3D12Resource> upload,readback,positions,normals;
    ComPtr<ID3D12InfoQueue> validation;uint64_t first_message=0;
    SceneOcclusionPass ao;SceneAmbientPass preliminary;SceneLightingState state;
    std::shared_ptr<SceneGeometryBindings> geometry;std::shared_ptr<SceneOcclusionBindings> occlusion;
    std::shared_ptr<SceneAmbientBindings> probe;std::shared_ptr<SceneLightingFrame> frame;
    unsigned guards=0;HdrGuestTransfer transfer=HdrGuestTransfer::kRaw;int32_t bias=0;
};
double Half(uint16_t bits){const unsigned e=(bits>>10)&31,m=bits&1023;double v=e?std::ldexp(1.+m/1024.,int(e)-15):std::ldexp(double(m),-24);return bits&32768?-v:v;}
}
namespace em2 {
// The parent native-Resolve fixture retains this owner through its own GPU
// fence. All commands execute in its existing list, after its material seeds.
std::shared_ptr<void> RecordSceneAmbientPublicationTest(CP& cp,Cache& cache,const std::shared_ptr<const HdrResolvedScene>& colour,const std::shared_ptr<const HdrResolvedScene>& ambient,rex::graphics::d3d12::D3D12TextureCache& textures,ID3D12Resource* legacy,ID3D12GraphicsCommandList* list){
    if(!colour || !ambient || !list || !colour->view() || colour->view()!=ambient->view())return {};
    auto f=std::make_shared<PublicationFixture>();f->transfer=colour->guest_transfer();f->bias=int32_t(colour->info().copy_dest_info.copy_dest_exp_bias);auto* device=cp.GetD3D12Provider().GetDevice();device->QueryInterface(IID_PPV_ARGS(&f->validation));if(f->validation)f->first_message=f->validation->GetNumStoredMessagesAllowedByRetrievalFilter();
    if(!f->ao.Initialize(device) || !f->preliminary.Initialize(device) || !Buffer(device,2*stride,D3D12_HEAP_TYPE_UPLOAD,&f->upload) || !Buffer(device,8*stride,D3D12_HEAP_TYPE_READBACK,&f->readback))return {};
    for(unsigned i=0;i<2;++i){D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=d.Height=side;d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=i?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R32G32B32A32_FLOAT;
        if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(i?&f->normals:&f->positions))))return {};}
    void* upload=nullptr;D3D12_RANGE none{};if(FAILED(f->upload->Map(0,&none,&upload)))return {};std::memset(upload,0,2*stride);
    for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x){const float position[]={x<4?x*.1f:.4f,y*.1f,x<4?0.f:(x-3)*.1f,1};std::memcpy(static_cast<uint8_t*>(upload)+y*256+x*16,position,16);const uint16_t normal[]={uint16_t(x<4?0:0xbc00),0,uint16_t(x<4?0x3c00:0),0x3c00};std::memcpy(static_cast<uint8_t*>(upload)+stride+y*256+x*8,normal,8);}f->upload->Unmap(0,nullptr);
    for(unsigned i=0;i<2;++i){D3D12_TEXTURE_COPY_LOCATION source{},destination{};source.pResource=f->upload.Get();source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;source.PlacedFootprint.Offset=i*stride;source.PlacedFootprint.Footprint={i?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R32G32B32A32_FLOAT,side,side,1,256};destination.pResource=i?f->normals.Get():f->positions.Get();destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);Barrier(list,destination.pResource,D3D12_RESOURCE_STATE_COPY_DEST,read);}
    auto& stream=cp.GetDeferredCommandList().*MemberOf(Stream{});
    auto execute=[&]{ID3D12DescriptorHeap* heap=(cp.*MemberOf(Mode{}))?(cp.*MemberOf(BindlessHeap{})):(cp.*MemberOf(ViewHeap{}));if(!heap)return false;list->SetDescriptorHeaps(1,&heap);cp.GetDeferredCommandList().Execute(list,nullptr);stream.clear();cp.*MemberOf(External{})=nullptr;return true;};
    auto copy=[&](ID3D12Resource* source,unsigned index,unsigned width=side,unsigned height=side){Barrier(list,source,read,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=source;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Offset=index*stride;to.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,width,height,1,256};list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(list,source,D3D12_RESOURCE_STATE_COPY_SOURCE,read);};
    if(!execute())return {};copy(colour->texture(),0);copy(ambient->texture(),3);
    f->geometry=std::make_shared<SceneGeometryBindings>();auto targets=std::make_shared<SceneGeometryTargets>();targets->positions=f->positions;targets->normals=f->normals;targets->written=true;f->geometry.get()->*MemberOf(Targets{})=std::move(targets);
    f->occlusion=f->ao.CreateBindings(f->geometry,{},colour->view());if(!f->occlusion || !f->ao.Record(cp,*f->occlusion,{2,4,.01f,2}))return {};
    const SceneAmbientSettings settings{1,.05f,.5f};f->probe=f->preliminary.CreateBindings(colour,ambient,f->occlusion,SceneAmbientEncoding::kLinear);if(!f->probe || !f->preliminary.Record(cp,*f->probe,settings))return {};
    const auto before=stream.size();if(PublishSceneAmbientResult(nullptr,cp,colour,f->probe) || PublishSceneAmbientResult(&cache,cp,ambient,f->probe))return {};f->guards+=2;
    CommitHdrSceneResolve(&cache,std::const_pointer_cast<HdrResolvedScene>(colour),false);if(PublishSceneAmbientResult(&cache,cp,colour,f->probe))return {};++f->guards;CommitHdrSceneResolve(&cache,std::const_pointer_cast<HdrResolvedScene>(colour),true);if(stream.size()!=before)return {};++f->guards;
    uint64_t keys=0;RememberHdrTextureKeyBatch(&textures,&keys,1);if(!HdrTextureKeyBatchCurrent(&textures,&keys,1))return {};++f->guards;
    ID3D12Resource* original_guest=nullptr;uint32_t original_guest_identity=0;
    if(f->transfer!=HdrGuestTransfer::kRaw){original_guest=colour->guest_texture();original_guest_identity=colour->guest_srv_identity();if(!original_guest || !original_guest_identity || original_guest==colour->texture())return {};++f->guards;}
    const auto original_resource=colour->texture();const auto revision=colour->revision();const auto view=colour->view();
    f->frame=std::make_shared<SceneLightingFrame>();f->frame->submission=cp.GetCurrentSubmission();f->frame->view=view;f->frame->occlusion=f->occlusion;f->frame->geometry_recorded=f->frame->occlusion_recorded=true;f->state.*MemberOf(Latest{})=f->frame;(f->state.*MemberOf(Pending{})).push_back(f->frame);
    auto* game=dynamic_cast<Em2CommandProcessor*>(&cp);if(!game || !legacy || !BindHdrSceneView(&cache,legacy,view))return {};
    {
        // Install the fixture's native cache/frame only for this actual native
        // Resolve callback. Restore all borrowed ownership and debug flags.
        auto& native_owner=cp.*MemberOf(CacheOwner{});auto saved_owner=std::move(native_owner);auto& lighting=game->*MemberOf(Lighting{});auto saved_lighting=std::move(lighting);const auto saved_failed=game->*MemberOf(Failed{});const bool saved_apply=REXCVAR_GET(em2_apply_scene_lighting);const bool saved_indirect=REXCVAR_GET(em2_apply_scene_indirect);
        struct Restore {decltype(native_owner) owner;decltype(saved_owner)& saved;decltype(lighting) lighting;decltype(saved_lighting)& saved_lighting;Em2CommandProcessor& game;bool failed,apply,indirect;
            ~Restore(){owner.release();owner=std::move(saved);lighting=std::move(saved_lighting);game.*MemberOf(Failed{})=failed;REXCVAR_SET(em2_apply_scene_lighting,apply);REXCVAR_SET(em2_apply_scene_indirect,indirect);}}restore{native_owner,saved_owner,lighting,saved_lighting,*game,saved_failed,saved_apply,saved_indirect};
        native_owner.reset(&cache);lighting=std::shared_ptr<SceneLightingState>(f,&f->state);game->*MemberOf(Failed{})=false;REXCVAR_SET(em2_apply_scene_lighting,true);
        if(!HasLinearMaterialSurfaceCapture(&cache))return {};
        auto surface=FindMaterialSurfaceResolve(&cache,colour->info());if(!surface)return {};
        CommitMaterialSurfaceResolve(&cache,std::const_pointer_cast<HdrResolvedScene>(surface),false);REXCVAR_SET(em2_apply_scene_indirect,true);
        if(!f->state.ComposeResolved(&cache,cp,colour->info(),settings) || f->state.indirect_fallbacks()!=1 || !f->frame->indirect.empty())return {};
        CommitMaterialSurfaceResolve(&cache,std::const_pointer_cast<HdrResolvedScene>(surface),true);
        for(bool indirect:{false,true}){
            REXCVAR_SET(em2_apply_scene_indirect,indirect);
            uint32_t written=0,length=0;auto* memory=cp.*MemberOf(Memory{});auto* shared=(cp.*MemberOf(Shared{})).get();if(!memory || !shared || !cache.Resolve(*memory,*shared,textures,written,length) || !length || game->*MemberOf(Failed{}))return {};
        }
    }
    if(f->state.publications()!=3 || f->state.indirect_publications()!=1 || f->state.indirect_fallbacks()!=1 || f->frame->compositions.size()!=3 || f->frame->indirect.size()!=1 || !colour->lighting_applied() || colour->revision()!=revision+5 || colour->texture()!=original_resource || colour->view()!=view)return {};++f->guards;
    if(f->transfer!=HdrGuestTransfer::kRaw){if(colour->guest_texture()!=original_guest || colour->guest_srv_identity()!=original_guest_identity)return {};++f->guards;}
    if(HdrTextureKeyBatchCurrent(&textures,&keys,1))return {};++f->guards;
    const auto& i=colour->info();HdrTextureQuery query{i.copy_dest_base,8,8,uint32_t(i.copy_dest_coordinate_info.pitch_aligned_div_32),uint32_t(i.copy_dest_info.copy_dest_endian),1,1};if(FindHdrSceneTexture(&textures,query)!=colour)return {};++f->guards;
    const auto published_size=stream.size();auto published=f->frame->compositions.back();
    if(PublishSceneAmbientResult(&cache,cp,colour,published) || f->state.ComposeResolved(&cache,cp,colour->info(),settings) || f->preliminary.Record(cp,*f->probe,settings) || f->preliminary.CreateBindings(colour,ambient,f->occlusion,SceneAmbientEncoding::kLinear) || stream.size()!=published_size)return {};f->guards+=4;
    if(!execute())return {};copy(colour->texture(),1);copy(published->result(),2);copy(f->occlusion->result(),4,4,4);if(f->transfer!=HdrGuestTransfer::kRaw)copy(colour->guest_texture(),5);copy(f->frame->indirect.back()->result(),6,4,4);copy(f->probe->result(),7);return f;
}
bool VerifySceneAmbientPublicationTest(const std::shared_ptr<void>& owner,const std::filesystem::path& directory){
    auto f=std::static_pointer_cast<PublicationFixture>(owner);if(!f)return false;const uint8_t* bytes=nullptr;D3D12_RANGE range{0,8*stride};if(FAILED(f->readback->Map(0,&range,reinterpret_cast<void**>(const_cast<uint8_t**>(&bytes)))))return false;
    auto bits=[&](unsigned image,unsigned x,unsigned y,unsigned c){uint16_t value;std::memcpy(&value,bytes+image*stride+y*256+x*8+c*2,2);return value;};unsigned bad=0,changed=0,alpha_differences=0;
    for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x)for(unsigned c=0;c<4;++c){const auto original=bits(0,x,y,c),actual=bits(1,x,y,c),expected=bits(2,x,y,c);bad+=actual!=expected || !std::isfinite(Half(actual));if(c==3)alpha_differences+=actual!=original;else changed+=actual!=original;}
    unsigned bounce_positive=0,bounce_invalid=0,upgraded_rgb=0;
    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)for(unsigned c=0;c<4;++c){const auto value=Half(bits(6,x,y,c));bounce_invalid+=!std::isfinite(value) || value<0;if(c<3)bounce_positive+=value>0;}
    for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x)for(unsigned c=0;c<3;++c)upgraded_rgb+=Half(bits(1,x,y,c))>Half(bits(7,x,y,c));
    unsigned encoded_bad=0;double encoded_error=0;if(f->transfer!=HdrGuestTransfer::kRaw)for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x)for(unsigned c=0;c<4;++c){const float value=float(Half(bits(1,x,y,c)));const float scale=std::exp2(float(f->bias));const float input=std::max(0.f,value/scale);float encoded=input;
        if(f->transfer==HdrGuestTransfer::kSrgb)encoded=input<=.0031308f?12.92f*input:1.055f*std::pow(input,1.f/2.4f)-.055f;
        else encoded=input<=1?rex::graphics::xenos::LinearToPWLGamma(input):std::trunc(input*(1023.f/8))*(1.f/255)+128.f/255;
        const double expected=c==3?std::clamp(value,0.f,1.f):encoded*scale,actual=Half(bits(5,x,y,c)),error=std::abs(actual-expected);encoded_error=std::max(encoded_error,error);encoded_bad+=!std::isfinite(actual) || error>std::max(6e-8,std::abs(expected)/1024);}
    unsigned errors=0;if(f->validation)for(uint64_t i=f->first_message;i<f->validation->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;f->validation->GetMessage(i,nullptr,&size);std::vector<uint8_t> data(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());if(SUCCEEDED(f->validation->GetMessage(i,message,&size)) && message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;REXLOG_ERROR("Scene publication validation: {}",message->pDescription);}}
    std::filesystem::create_directories(directory);std::ofstream pixels(directory/"scene-publication-readback.bin",std::ios::binary);pixels.write(reinterpret_cast<const char*>(bytes),8*stride);D3D12_RANGE none{};f->readback->Unmap(0,&none);
    std::ofstream report(directory/"scene-publication-results.json");report<<"{\"indirect_publications\":1,\"ao_fallbacks\":1,\"readback_images\":8,\"bounce_positive_components\":"<<bounce_positive<<",\"bounce_invalid\":"<<bounce_invalid<<",\"upgraded_rgb\":"<<upgraded_rgb<<",\"encoded_transfer\":"<<uint32_t(f->transfer)<<",\"encoded_components\":"<<(f->transfer==HdrGuestTransfer::kRaw?0:256)<<",\"encoded_invalid\":"<<encoded_bad<<",\"encoded_max_error\":"<<encoded_error<<",\"callback_native_copies\":2,\"components\":256,\"invalid\":"<<bad<<",\"changed_rgb\":"<<changed<<",\"alpha_differences\":"<<alpha_differences<<",\"guards\":"<<f->guards<<",\"device_errors\":"<<errors<<",\"scope\":\"Actual native HDR/ambient/linear-surface resolve images; seeded corner geometry; missing-surface AO fallback then native AO and GI resolve callbacks, retained indirect pass and owner SceneLightingState composition/publication into existing FP16 image, readback bit-exact to composed output. Resource/view preserved, revision/descriptor epoch updated, full native texture lookup, duplicate/recompose/invalid/missing-cache/layer guards. Parent command list/fence owns all work. Camera metadata and geometry are fixture data; no live camera or displayed gameplay proof.\"}\n";
    REXLOG_INFO("Scene lighting publication:256 components, invalid{}, changed RGB{}, alpha differences{}, guards{}, device errors{}",bad,changed,alpha_differences,f->guards,errors);
    REXLOG_INFO("Native indirect publication: positive bounce{}, invalid{}, upgraded RGB{}",bounce_positive,bounce_invalid,upgraded_rgb);
    return !bad && bounce_positive && !bounce_invalid && upgraded_rgb && changed && !alpha_differences && !errors && f->guards==(f->transfer==HdrGuestTransfer::kRaw?12:14) && !encoded_bad && report.good() && pixels.good();
}
}
