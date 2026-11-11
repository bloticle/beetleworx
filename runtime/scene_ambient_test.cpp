#include "scene_ambient.h"
#include "scene_geometry_targets.h"
#include "scene_shader_audit.h"
#include "native_pixel_fixture.h"
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/graphics/d3d12/deferred_command_list.h>
#include <rex/logging.h>
#include <d3d12sdklayers.h>
#include <bit>
#include <algorithm>
#include <rex/thread/mutex.h>
#include <cmath>
#include <cstring>
#include <fstream>
namespace {
using namespace em2;using namespace em2::pixel_test;using CP=rex::graphics::d3d12::D3D12CommandProcessor;using List=rex::graphics::d3d12::DeferredCommandList;
template<class Tag,auto Member>struct Access{friend auto MemberOf(Tag){return Member;}};
#define FIELD(tag,type,field) struct tag{friend auto MemberOf(tag);};template struct Access<tag,&type::field>
FIELD(External,CP,current_external_pipeline_);FIELD(Stream,List,command_stream_);FIELD(Begin,CP,BeginSubmission);FIELD(Mode,CP,bindless_resources_used_);FIELD(ViewHeap,CP,view_bindful_heap_current_);FIELD(BindlessHeap,CP,view_bindless_heap_);
FIELD(View,HdrResolvedScene,view_);FIELD(Encoding,HdrResolvedScene,encoding_);FIELD(Geometry,SceneGeometryBindings,targets_);FIELD(Texture,HdrResolvedScene,texture_);FIELD(Layer,HdrResolvedScene,layer_);FIELD(Info,HdrResolvedScene,info_);FIELD(Valid,HdrResolvedScene,valid_);FIELD(Submission,HdrResolvedScene,submission_);FIELD(Frame,HdrResolvedScene,frame_);FIELD(Revision,HdrResolvedScene,revision_);
#undef FIELD
constexpr unsigned side=8,patterns=4;constexpr uint64_t image=2048;constexpr auto read=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
float Half(uint16_t bits){const unsigned e=(bits>>10)&31,m=bits&1023;return (bits&32768?-1.f:1.f)*(e?std::ldexp(float(1024+m),int(e)-25):std::ldexp(float(m),-24));}
uint16_t Bits(float v){const auto u=std::bit_cast<uint32_t>(v);if((u&0x7fffffff)==0)return uint16_t(u>>16);return uint16_t(((u>>16)&32768)|((((u>>23)&255)-112)<<10)|((u&0x7fffff)>>13));}
struct FixtureData:Fixture {
    SceneOcclusionPass ao;SceneAmbientPass composition;
    std::array<std::array<ComPtr<ID3D12Resource>,4>,patterns> inputs;
    std::array<std::shared_ptr<HdrResolvedScene>,patterns> colours,ambient;
    std::vector<std::shared_ptr<SceneGeometryBindings>> geometry;
    std::vector<std::shared_ptr<SceneOcclusionBindings>> occlusion;
    std::vector<std::shared_ptr<SceneAmbientBindings>> bindings;
};
std::array<float,4> Position(unsigned pattern,unsigned x,unsigned y){return {float(x)*.1f,float(y)*.1f,pattern && x<4?-.5f:0,pattern==2 && x==7?0.f:1.f};}
std::array<float,4> Normal(unsigned pattern,unsigned x){return {0,0,pattern==3 && x<4?1.f:-1.f,pattern==2 && x==4?0.f:1.f};}
std::array<float,4> Ambient(unsigned pattern,unsigned x,unsigned y){return {x && y?(pattern==2?-.125f:.5f):0,x && y?.25f:0,x && y?.125f:0,1};}
}
bool em2::TestSceneAmbientComposition(CP& cp,const std::filesystem::path& directory){
    if(!(cp.*MemberOf(Begin{}))(false))return false;
    rex::thread::global_critical_region region;auto memory_lock=region.Acquire();
    auto f=std::make_unique<FixtureData>();auto* device=cp.GetD3D12Provider().GetDevice();ComPtr<ID3D12InfoQueue> validation;device->QueryInterface(IID_PPV_ARGS(&validation));const auto first=validation?validation->GetNumStoredMessagesAllowedByRetrievalFilter():0;
    if(!f->event || !f->ao.Initialize(device) || !f->composition.Initialize(device) || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f->allocator))) || FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f->allocator.Get(),nullptr,IID_PPV_ARGS(&f->list))) || FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f->fence))) || !Buffer(device,patterns*4*image,D3D12_HEAP_TYPE_UPLOAD,&f->upload) || !Buffer(device,16*image,D3D12_HEAP_TYPE_READBACK,&f->readback))return false;
    auto& stream=cp.GetDeferredCommandList().*MemberOf(Stream{});auto saved=std::move(stream);stream.clear();struct Restore{decltype(stream) stream;decltype(saved)& saved;CP& cp;~Restore(){cp.SetExternalPipeline(nullptr);stream=std::move(saved);}}restore{stream,saved,cp};
    void* mapped=nullptr;D3D12_RANGE none{};if(FAILED(f->upload->Map(0,&none,&mapped)))return false;std::memset(mapped,0,patterns*4*image);
    for(unsigned pattern=0;pattern<patterns;++pattern){
        SceneViewIdentity metadata;metadata.frame=cp.GetCurrentFrame();metadata.submission=cp.GetCurrentSubmission();metadata.draw=pattern+1;metadata.width=metadata.height=side;auto view=std::make_shared<const SceneViewIdentity>(metadata);
        for(unsigned input=0;input<4;++input){D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=d.Height=side;d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=input==2?DXGI_FORMAT_R32G32B32A32_FLOAT:DXGI_FORMAT_R16G16B16A16_FLOAT;
            if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&f->inputs[pattern][input]))))return false;
            for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x){auto ambient=Ambient(pattern,x,y);const auto values=input==0?std::array<float,4>{4+ambient[0],2+ambient[1],1+ambient[2],.625f}:input==1?ambient:input==2?Position(pattern,x,y):Normal(pattern,x);auto* destination=static_cast<uint8_t*>(mapped)+(pattern*4+input)*image+y*256+x*(input==2?16:8);for(unsigned c=0;c<4;++c){if(input==2)std::memcpy(destination+c*4,&values[c],4);else{const auto bits=Bits(values[c]);std::memcpy(destination+c*2,&bits,2);}}}
        }
        for(unsigned layer=0;layer<2;++layer){auto source=std::make_shared<HdrResolvedScene>();source.get()->*MemberOf(Encoding{})=HdrSceneEncoding::kNativeLinear;source.get()->*MemberOf(Texture{})=f->inputs[pattern][layer];source.get()->*MemberOf(Layer{})=layer?SceneResolveLayer::kMaterialAmbient:SceneResolveLayer::kHdr;source.get()->*MemberOf(Valid{})=true;source.get()->*MemberOf(View{})=view;source.get()->*MemberOf(Submission{})=cp.GetCurrentSubmission();source.get()->*MemberOf(Frame{})=cp.GetCurrentFrame();source.get()->*MemberOf(Revision{})=1;auto& info=source.get()->*MemberOf(Info{});info.copy_dest_base=0x100000+pattern*65536;info.copy_dest_extent_start=info.copy_dest_base;info.copy_dest_extent_length=65536;info.coordinate_info.width_div_8=info.height_div_8=1;info.coordinate_info.draw_resolution_scale_x=info.coordinate_info.draw_resolution_scale_y=1;(layer?f->ambient:f->colours)[pattern]=std::move(source);}
        auto geometry=std::make_shared<SceneGeometryBindings>();auto targets=std::make_shared<SceneGeometryTargets>();targets->positions=f->inputs[pattern][2];targets->normals=f->inputs[pattern][3];targets->written=true;geometry.get()->*MemberOf(Geometry{})=std::move(targets);f->geometry.push_back(geometry);
    }
    f->upload->Unmap(0,nullptr);
    for(unsigned pattern=0;pattern<patterns;++pattern)for(unsigned input=0;input<4;++input){D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=f->upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint.Offset=(pattern*4+input)*image;from.PlacedFootprint.Footprint={f->inputs[pattern][input]->GetDesc().Format,side,side,1,256};to.pResource=f->inputs[pattern][input].Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),to.pResource,D3D12_RESOURCE_STATE_COPY_DEST,read);}
    auto execute=[&]{ID3D12DescriptorHeap* heaps[]={(cp.*MemberOf(Mode{}))?(cp.*MemberOf(BindlessHeap{})):(cp.*MemberOf(ViewHeap{}))};if(!heaps[0])return false;f->list->SetDescriptorHeaps(1,heaps);cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();cp.*MemberOf(External{})=nullptr;return true;};
    auto copy=[&](ID3D12Resource* source,unsigned index,unsigned width,unsigned height){Barrier(f->list.Get(),source,read,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=source;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Offset=index*image;to.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,width,height,1,256};f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),source,D3D12_RESOURCE_STATE_COPY_SOURCE,read);};
    unsigned guards=0,reuses=0;
    for(unsigned pattern=0;pattern<patterns;++pattern){
        auto ao=f->ao.CreateBindings(f->geometry[pattern],f->occlusion.empty()?nullptr:f->occlusion.back(),f->colours[pattern]->view());if(!ao || !f->ao.Record(cp,*ao,{2,4,.01f,2}) || !execute())return false;f->occlusion.push_back(ao);copy(ao->result(),12+pattern,4,4);
        if(pattern && f->composition.Record(cp,*f->bindings.back(),{}))return false;if(pattern)++guards;
        auto& colour=*f->colours[pattern];auto& ambient=*f->ambient[pattern];
        const auto view=colour.view();colour.*MemberOf(View{})=nullptr;if(f->composition.CreateBindings(f->colours[pattern],f->ambient[pattern],ao,SceneAmbientEncoding::kLinear))return false;++guards;
        colour.*MemberOf(View{})=std::make_shared<const SceneViewIdentity>(*view);if(f->composition.CreateBindings(f->colours[pattern],f->ambient[pattern],ao,SceneAmbientEncoding::kLinear))return false;colour.*MemberOf(View{})=view;++guards;
        ambient.*MemberOf(View{})=std::make_shared<const SceneViewIdentity>(*view);if(f->composition.CreateBindings(f->colours[pattern],f->ambient[pattern],ao,SceneAmbientEncoding::kLinear))return false;ambient.*MemberOf(View{})=view;++guards;
        colour.*MemberOf(Encoding{})=HdrSceneEncoding::kCompatibility;if(f->composition.CreateBindings(f->colours[pattern],f->ambient[pattern],ao,SceneAmbientEncoding::kLinear))return false;colour.*MemberOf(Encoding{})=HdrSceneEncoding::kNativeLinear;++guards;
        ++(colour.*MemberOf(Frame{}));if(f->composition.CreateBindings(f->colours[pattern],f->ambient[pattern],ao,SceneAmbientEncoding::kLinear))return false;--(colour.*MemberOf(Frame{}));++guards;
        ++(ambient.*MemberOf(Submission{}));if(f->composition.CreateBindings(f->colours[pattern],f->ambient[pattern],ao,SceneAmbientEncoding::kLinear))return false;--(ambient.*MemberOf(Submission{}));++guards;
        (ambient.*MemberOf(Info{})).copy_dest_base+=4096;if(f->composition.CreateBindings(f->colours[pattern],f->ambient[pattern],ao,SceneAmbientEncoding::kLinear))return false;(ambient.*MemberOf(Info{})).copy_dest_base-=4096;++guards;
        colour.*MemberOf(Valid{})=false;if(f->composition.CreateBindings(f->colours[pattern],f->ambient[pattern],ao,SceneAmbientEncoding::kLinear))return false;colour.*MemberOf(Valid{})=true;++guards;
        (colour.*MemberOf(Info{})).coordinate_info.width_div_8=2;(ambient.*MemberOf(Info{})).coordinate_info.width_div_8=2;if(f->composition.CreateBindings(f->colours[pattern],f->ambient[pattern],ao,SceneAmbientEncoding::kLinear))return false;(colour.*MemberOf(Info{})).coordinate_info.width_div_8=1;(ambient.*MemberOf(Info{})).coordinate_info.width_div_8=1;++guards;
        for(unsigned selection=0;selection<3;++selection){auto previous=f->bindings.empty()?nullptr:f->bindings.back();auto binding=f->composition.CreateBindings(f->colours[pattern],f->ambient[pattern],ao,SceneAmbientEncoding::kLinear,previous);if(!binding)return false;if(previous){if(binding->result()!=previous->result())return false;++reuses;}f->bindings.push_back(binding);
            if(f->composition.CreateBindings(f->colours[pattern],f->ambient[pattern],ao,SceneAmbientEncoding::kUnknown) || f->composition.CreateBindings(f->ambient[pattern],f->colours[pattern],ao,SceneAmbientEncoding::kLinear))return false;guards+=2;
            auto& revision=f->ambient[pattern].get()->*MemberOf(Revision{});++revision;if(f->composition.Record(cp,*binding,{}))return false;--revision;++guards;
            auto invalid=SceneAmbientSettings{};invalid.normal_cosine=1;if(f->composition.Record(cp,*binding,invalid))return false;++guards;
            if(!f->composition.Record(cp,*binding,{selection*.5f,.05f,.5f}) || !binding->current() || (previous && previous->current()) || !execute())return false;if(previous)++guards;copy(binding->result(),pattern*3+selection,8,8);
        }
    }
    if(FAILED(f->list->Close()))return false;ID3D12CommandList* lists[]={f->list.Get()};cp.GetD3D12Provider().GetDirectQueue()->ExecuteCommandLists(1,lists);if(FAILED(cp.GetD3D12Provider().GetDirectQueue()->Signal(f->fence.Get(),1)) || FAILED(f->fence->SetEventOnCompletion(1,f->event)) || WaitForSingleObject(f->event,15000)!=WAIT_OBJECT_0){f.release();return false;}
    D3D12_RANGE range{0,16*image};if(FAILED(f->readback->Map(0,&range,&mapped)))return false;const auto* bytes=static_cast<const uint8_t*>(mapped);unsigned bad=0,changed=0,preserved=0;double maximum=0;
    auto at=[&](unsigned index,unsigned x,unsigned y,unsigned c){uint16_t bits;std::memcpy(&bits,bytes+index*image+y*256+x*8+c*2,2);return Half(bits);};
    for(unsigned pattern=0;pattern<patterns;++pattern)for(unsigned selection=0;selection<3;++selection)for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x){const auto p=Position(pattern,x,y),n=Normal(pattern,x);double total=0,visibility=0;if(p[3]==1 && n[3]==1){const double fx=(double(x)-1)*.5,fy=(double(y)-1)*.5;const int bx=int(std::floor(fx)),by=int(std::floor(fy));const double tx=fx-bx,ty=fy-by;for(unsigned dy=0;dy<2;++dy)for(unsigned dx=0;dx<2;++dx){const int ax=std::clamp(bx+int(dx),0,3),ay=std::clamp(by+int(dy),0,3);const auto q=Position(pattern,ax*2+1,ay*2+1),qn=Normal(pattern,ax*2+1);if(q[3]!=1 || qn[3]!=1 || n[2]*qn[2]<.5 || std::abs(q[2]-p[2])>=.05)continue;const double w=(dx?tx:1-tx)*(dy?ty:1-ty);total+=w;visibility+=w*at(12+pattern,ax,ay,3);}}
        const double v=total>1e-6?visibility/total:1;const auto a=Ambient(pattern,x,y);const double direct[]={4,2,1};for(unsigned c=0;c<4;++c){const double colour=c==3?.625:direct[c]+a[c];const double expected=c==3?colour:colour-a[c]*(1-v)*selection*.5;const auto actual=at(pattern*3+selection,x,y,c);const double delta=std::abs(actual-expected);maximum=std::max(maximum,delta);bad+=!std::isfinite(actual) || delta>std::max(6e-8,std::abs(expected)/1024);if(c==3 || selection==0 || a[c]==0 || v==1){++preserved;bad+=actual!=colour;}else changed+=actual!=colour;}
    }
    std::filesystem::create_directories(directory);{std::ofstream pixels(directory/"ambient-composition-readback.bin",std::ios::binary);pixels.write(reinterpret_cast<const char*>(bytes),16*image);}
    f->readback->Unmap(0,&none);unsigned errors=0;if(validation)for(uint64_t i=first;i<validation->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;validation->GetMessage(i,nullptr,&size);std::vector<uint8_t> data(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());if(SUCCEEDED(validation->GetMessage(i,message,&size)) && message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;REXLOG_ERROR("Ambient composition validation: {}",message->pDescription);}}
    std::filesystem::create_directories(directory);std::ofstream report(directory/"ambient-composition-results.json");report<<"{\"dispatches\":12,\"components\":3072,\"invalid\":"<<bad<<",\"max_error\":"<<maximum<<",\"changed_components\":"<<changed<<",\"preserved_components\":"<<preserved<<",\"guards\":"<<guards<<",\"output_reuses\":"<<reuses<<",\"device_errors\":"<<errors<<",\"scope\":\"Actual deferred geometry AO plus ambient residual composition; seeded linear HDR/ambient and geometry, native-shaped metadata and camera associations via test access. Missing and distinct camera tokens reject even with identical metadata. Flat/step/invalid/flipped normals, signed ambient, zero ambient, three strengths, alpha, edge gating and resource reuse. CPU reference uses GPU AO as input. Not native scene association, original material draw through IssueDraw or displayed lighting.\"}\n";REXLOG_INFO("Ambient composition:3072 components, invalid {}, error {}, changed {}, device errors {}",bad,maximum,changed,errors);return !bad && changed && !errors && guards==98 && reuses==11 && report.good();
}
