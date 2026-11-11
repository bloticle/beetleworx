#include "scene_indirect.h"
#include "scene_ambient.h"
#include "scene_lighting.h"
#include <limits>
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
FIELD(ManagedLatest,SceneLightingState,latest_);FIELD(ManagedPending,SceneLightingState,pending_);FIELD(External,CP,current_external_pipeline_);FIELD(Stream,List,command_stream_);FIELD(Begin,CP,BeginSubmission);FIELD(Mode,CP,bindless_resources_used_);FIELD(ViewHeap,CP,view_bindful_heap_current_);FIELD(BindlessHeap,CP,view_bindless_heap_);
FIELD(View,HdrResolvedScene,view_);FIELD(Encoding,HdrResolvedScene,encoding_);FIELD(Geometry,SceneGeometryBindings,targets_);FIELD(Texture,HdrResolvedScene,texture_);FIELD(Layer,HdrResolvedScene,layer_);FIELD(Info,HdrResolvedScene,info_);FIELD(Valid,HdrResolvedScene,valid_);FIELD(Submission,HdrResolvedScene,submission_);FIELD(Frame,HdrResolvedScene,frame_);FIELD(Revision,HdrResolvedScene,revision_);
#undef FIELD
constexpr unsigned side=8,patterns=7;constexpr unsigned sourceBase=patterns*3,composeBase=patterns*4,aoBase=patterns*7,images=patterns*8;constexpr uint64_t image=2048;constexpr auto read=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
float Half(uint16_t bits){const unsigned e=(bits>>10)&31,m=bits&1023;return (bits&32768?-1.f:1.f)*(e?std::ldexp(float(1024+m),int(e)-25):std::ldexp(float(m),-24));}
uint16_t Bits(float v){const auto u=std::bit_cast<uint32_t>(v);if((u&0x7fffffff)==0)return uint16_t(u>>16);return uint16_t(((u>>16)&32768)|((((u>>23)&255)-112)<<10)|((u&0x7fffff)>>13));}
struct FixtureData:Fixture {
    SceneOcclusionPass ao;SceneIndirectPass composition;SceneLightingState managed;
    std::vector<std::shared_ptr<const SceneAmbientBindings>> composed;
    std::array<std::array<ComPtr<ID3D12Resource>,4>,patterns> inputs;
    std::array<std::shared_ptr<HdrResolvedScene>,patterns> colours,surface;
    std::vector<std::shared_ptr<SceneGeometryBindings>> geometry;
    std::vector<std::shared_ptr<SceneOcclusionBindings>> occlusion;
    std::vector<std::shared_ptr<SceneIndirectBindings>> bindings;
};
std::array<float,4> Position(unsigned pattern,unsigned x,unsigned y){
 if(pattern==3 && x==3)return {.25f,float(y)*.1f,.1f,1};
 return {x<4?float(x)*.1f:.4f,float(y)*.1f,pattern==1 || x<4?0.f:float(x-3)*.1f,1};
}
std::array<float,4> Normal(unsigned pattern,unsigned x){if(pattern==3 && x==3)return {0,0,0,0};return pattern==1||x<4?std::array<float,4>{0,0,1,1}:std::array<float,4>{pattern==4?1.f:-1.f,0,0,1};}
std::array<float,4> Surface(unsigned pattern,unsigned x,unsigned){if(pattern==6 && x<2)return {.125f,.5f,.75f,16};return {.5f,.25f,.75f,pattern==5?-1.f:16.f};}
std::array<float,4> Colour(unsigned pattern,unsigned x){return {pattern!=2 && x>=4?8.f:0.f,pattern!=2 && x>=4?2.f:0.f,pattern!=2 && x>=4?.5f:0.f,.625f};}
using V=std::array<double,3>;
V Vec(const std::array<float,4>& a){return {a[0],a[1],a[2]};}
V Sub(V a,V b){for(unsigned i=0;i<3;++i)a[i]-=b[i];return a;}
V Add(V a,V b){for(unsigned i=0;i<3;++i)a[i]+=b[i];return a;}
V Mul(V a,double b){for(auto& c:a)c*=b;return a;}
double Dot(V a,V b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
V Cross(V a,V b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
bool Inside(int x,int y){return x>=0&&y>=0&&x<8&&y<8;}
std::array<double,4> Reference(unsigned pattern,unsigned x,unsigned y,double strength){
 const auto material=Surface(pattern,x,y),normal=Normal(pattern,x);if(material[3]<0 || normal[3]!=1)return {};
 const V receiver=Vec(Position(pattern,x,y)),nr=Vec(normal);V incoming{};double fraction=0;
 for(int sy:{-3,-1,1,3})for(int sx:{-3,-1,1,3}){const int qx=int(x)+sx,qy=int(y)+sy;if(!Inside(qx,qy))continue;
  const auto emitterNormal=Normal(pattern,qx);if(emitterNormal[3]!=1)continue;const V emitter=Vec(Position(pattern,qx,qy)),ne=Vec(emitterNormal),separation=Sub(emitter,receiver);const double d2=Dot(separation,separation);if(d2<=4*.06*.06 || d2>=4)continue;
  const double distance=std::sqrt(d2),facing=std::max(0.,Dot(nr,separation)/distance)*std::max(0.,-Dot(ne,separation)/distance);if(facing==0)continue;
  V basis[2];bool areaValid=true;for(unsigned axis=0;axis<2;++axis){bool valid=false;double closest=1e30;for(int sign:{1,-1}){const int xx=qx+(axis?0:sign),yy=qy+(axis?sign:0);if(!Inside(xx,yy))continue;V delta=Mul(Sub(Vec(Position(pattern,xx,yy)),emitter),sign);const double length=Dot(delta,delta);if(std::abs(Dot(delta,ne))>.06 || length>=1)continue;if(length<closest){closest=length;basis[axis]=delta;valid=true;}}areaValid&=valid;}if(!areaValid)continue;
  const double area=std::abs(Dot(Cross(basis[0],basis[1]),ne));if(area<=1e-12)continue;
  const V a=Add(receiver,Mul(nr,.03)),b=Add(emitter,Mul(ne,.03)),ray=Sub(b,a);const double length=Dot(ray,ray);if(length<1e-10)continue;bool visible=true;
  for(unsigned step=1;step<=4;++step){const double t=double(step)/5;const int xx=int(std::nearbyint(double(x)+(qx-int(x))*t)),yy=int(std::nearbyint(double(y)+(qy-int(y))*t));const V blocker=Vec(Position(pattern,xx,yy));const auto dr=Sub(blocker,receiver),de=Sub(blocker,emitter);if(Dot(dr,dr)<=4*.06*.06 || Dot(de,de)<=4*.06*.06)continue;const double along=Dot(Sub(blocker,a),ray)/length;const V miss=Sub(blocker,Add(a,Mul(ray,along)));if(along>0&&along<1&&Dot(miss,miss)<.06*.06)visible=false;}if(!visible)continue;
  const double weight=facing*area*4/(3.141592653589793*d2)*std::pow(1-distance/2,2);const auto radiance=Colour(pattern,qx);for(unsigned c=0;c<3;++c)incoming[c]+=radiance[c]*weight;fraction+=weight;
 }
 std::array<double,4> result{0,0,0,1};for(unsigned c=0;c<3;++c)result[c]=material[c]*incoming[c]/std::max(1.,fraction)*strength;return result;
}

}
bool em2::TestSceneIndirectLighting(CP& cp,const std::filesystem::path& directory){
    std::filesystem::create_directories(directory);
    if(!(cp.*MemberOf(Begin{}))(false))return false;
    rex::thread::global_critical_region region;auto memory_lock=region.Acquire();
    auto f=std::make_unique<FixtureData>();auto* device=cp.GetD3D12Provider().GetDevice();ComPtr<ID3D12InfoQueue> validation;device->QueryInterface(IID_PPV_ARGS(&validation));const auto first=validation?validation->GetNumStoredMessagesAllowedByRetrievalFilter():0;
    if(!f->event || !f->ao.Initialize(device) || !f->composition.Initialize(device) || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f->allocator))) || FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f->allocator.Get(),nullptr,IID_PPV_ARGS(&f->list))) || FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f->fence))) || !Buffer(device,patterns*4*image,D3D12_HEAP_TYPE_UPLOAD,&f->upload) || !Buffer(device,images*image,D3D12_HEAP_TYPE_READBACK,&f->readback))return false;
    auto& stream=cp.GetDeferredCommandList().*MemberOf(Stream{});auto saved=std::move(stream);stream.clear();struct Restore{decltype(stream) stream;decltype(saved)& saved;CP& cp;~Restore(){cp.SetExternalPipeline(nullptr);stream=std::move(saved);}}restore{stream,saved,cp};
    void* mapped=nullptr;D3D12_RANGE none{};if(FAILED(f->upload->Map(0,&none,&mapped)))return false;std::memset(mapped,0,patterns*4*image);
    for(unsigned pattern=0;pattern<patterns;++pattern){
        SceneViewIdentity metadata;metadata.frame=cp.GetCurrentFrame();metadata.submission=cp.GetCurrentSubmission();metadata.draw=pattern+1;metadata.width=metadata.height=side;auto view=std::make_shared<const SceneViewIdentity>(metadata);
        for(unsigned input=0;input<4;++input){D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=d.Height=side;d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=input==2?DXGI_FORMAT_R32G32B32A32_FLOAT:DXGI_FORMAT_R16G16B16A16_FLOAT;
            if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&f->inputs[pattern][input]))))return false;
            for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x){const auto values=input==0?Colour(pattern,x):input==1?Surface(pattern,x,y):input==2?Position(pattern,x,y):Normal(pattern,x);auto* destination=static_cast<uint8_t*>(mapped)+(pattern*4+input)*image+y*256+x*(input==2?16:8);for(unsigned c=0;c<4;++c){if(input==2)std::memcpy(destination+c*4,&values[c],4);else{const auto bits=Bits(values[c]);std::memcpy(destination+c*2,&bits,2);}}}
        }
        for(unsigned layer=0;layer<2;++layer){auto source=std::make_shared<HdrResolvedScene>();source.get()->*MemberOf(Encoding{})=HdrSceneEncoding::kNativeLinear;source.get()->*MemberOf(Texture{})=f->inputs[pattern][layer];source.get()->*MemberOf(Layer{})=layer?SceneResolveLayer::kMaterialSurface:SceneResolveLayer::kHdr;source.get()->*MemberOf(Valid{})=true;source.get()->*MemberOf(View{})=view;source.get()->*MemberOf(Submission{})=cp.GetCurrentSubmission();source.get()->*MemberOf(Frame{})=cp.GetCurrentFrame();source.get()->*MemberOf(Revision{})=1;auto& info=source.get()->*MemberOf(Info{});info.copy_dest_base=0x100000+pattern*65536;info.copy_dest_extent_start=info.copy_dest_base;info.copy_dest_extent_length=65536;info.coordinate_info.width_div_8=info.height_div_8=1;info.coordinate_info.draw_resolution_scale_x=info.coordinate_info.draw_resolution_scale_y=1;(layer?f->surface:f->colours)[pattern]=std::move(source);}
        auto geometry=std::make_shared<SceneGeometryBindings>();auto targets=std::make_shared<SceneGeometryTargets>();targets->positions=f->inputs[pattern][2];targets->normals=f->inputs[pattern][3];targets->written=true;geometry.get()->*MemberOf(Geometry{})=std::move(targets);f->geometry.push_back(geometry);
    }
    f->upload->Unmap(0,nullptr);
    for(unsigned pattern=0;pattern<patterns;++pattern)for(unsigned input=0;input<4;++input){D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=f->upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint.Offset=(pattern*4+input)*image;from.PlacedFootprint.Footprint={f->inputs[pattern][input]->GetDesc().Format,side,side,1,256};to.pResource=f->inputs[pattern][input].Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),to.pResource,D3D12_RESOURCE_STATE_COPY_DEST,read);}
    auto execute=[&]{ID3D12DescriptorHeap* heaps[]={(cp.*MemberOf(Mode{}))?(cp.*MemberOf(BindlessHeap{})):(cp.*MemberOf(ViewHeap{}))};if(!heaps[0])return false;f->list->SetDescriptorHeaps(1,heaps);cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();cp.*MemberOf(External{})=nullptr;return true;};
    auto copy=[&](ID3D12Resource* source,unsigned index,unsigned width,unsigned height){Barrier(f->list.Get(),source,read,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=source;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Offset=index*image;to.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,width,height,1,256};f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),source,D3D12_RESOURCE_STATE_COPY_SOURCE,read);};
    unsigned guards=0,reuses=0;
    for(unsigned pattern=0;pattern<patterns;++pattern){
        auto ao=f->ao.CreateBindings(f->geometry[pattern],f->occlusion.empty()?nullptr:f->occlusion.back(),f->colours[pattern]->view());if(!ao || !f->ao.Record(cp,*ao,{2,4,.01f,1}) || !execute())return false;f->occlusion.push_back(ao);
        auto colour=f->colours[pattern],surface=f->surface[pattern];
        auto frame=std::make_shared<SceneLightingFrame>();frame->submission=cp.GetCurrentSubmission();frame->view=ao->view();frame->occlusion=ao;frame->geometry_recorded=frame->occlusion_recorded=true;
        (f->managed.*MemberOf(ManagedPending{})).push_back(frame);
        f->managed.*MemberOf(ManagedLatest{})=frame;
        auto ambient=std::make_shared<HdrResolvedScene>(*surface);ambient.get()->*MemberOf(Layer{})=SceneResolveLayer::kMaterialAmbient;copy(ao->result(),aoBase+pattern,4,4);
        auto make=[&](SceneIndirectEncoding encoding){return f->composition.CreateBindings(colour,surface,ao,encoding);};
        if(make(SceneIndirectEncoding::kUnknown))return false;++guards;
        const auto view=surface->view();surface.get()->*MemberOf(View{})=std::make_shared<const SceneViewIdentity>(*view);if(make(SceneIndirectEncoding::kLinearDiffuse))return false;surface.get()->*MemberOf(View{})=view;++guards;
        surface.get()->*MemberOf(Valid{})=false;if(make(SceneIndirectEncoding::kLinearDiffuse))return false;surface.get()->*MemberOf(Valid{})=true;++guards;
        surface.get()->*MemberOf(Layer{})=SceneResolveLayer::kMaterialAmbient;if(make(SceneIndirectEncoding::kLinearDiffuse))return false;surface.get()->*MemberOf(Layer{})=SceneResolveLayer::kMaterialSurface;++guards;
        for(unsigned selection=0;selection<3;++selection){
            auto b=f->composition.CreateBindings(colour,surface,ao,SceneIndirectEncoding::kLinearDiffuse,f->bindings.empty()?nullptr:f->bindings.back());if(!b || b->current())return false;if(!f->bindings.empty()){if(b->result()!=f->bindings.back()->result())return false;++reuses;}
            const auto before=stream.size();SceneIndirectSettings bad{2,4,.06f,2};if(f->composition.Record(cp,*b,bad) || stream.size()!=before)return false;++guards;bad.strength=std::numeric_limits<float>::quiet_NaN();if(f->composition.Record(cp,*b,bad) || stream.size()!=before)return false;++guards;
            if(!f->composition.Record(cp,*b,{2,4,.06f,selection*.5f}) || !b->current() || !b->can_use_with(*colour) || !execute())return false;
            if(!f->bindings.empty() && f->bindings.back()->current())return false;
            copy(b->result(),pattern*3+selection,4,4);f->bindings.push_back(b);
            auto mixed=f->managed.Compose(cp,colour,ambient,SceneAmbientEncoding::kLinear,{.5f,.05f,.5f,1,.1f},surface,SceneIndirectEncoding::kLinearDiffuse,{2,4,.06f,selection*.5f});
            if(!mixed || !mixed->can_publish_to(*colour) || frame->indirect.size()!=selection+1 || frame->compositions.size()!=selection+1 || !execute())return false;
            copy(mixed->result(),composeBase+pattern*3+selection,8,8);f->composed.push_back(mixed);
            surface.get()->*MemberOf(Revision{})+=1;if(b->current() || b->can_use_with(*colour) || mixed->can_publish_to(*colour) || f->composition.Record(cp,*b,{2,4,.06f,1}))return false;surface.get()->*MemberOf(Revision{})-=1;++guards;
        }
        copy(colour->texture(),sourceBase+pattern,8,8);
    }
    const auto retained=f->managed.pending();if(retained!=patterns)return false;f->managed.Disable();f->managed.Collect(0);if(f->managed.latest() || f->managed.pending()!=retained)return false;
    if(FAILED(f->list->Close()))return false;ID3D12CommandList* lists[]={f->list.Get()};cp.GetD3D12Provider().GetDirectQueue()->ExecuteCommandLists(1,lists);if(FAILED(cp.GetD3D12Provider().GetDirectQueue()->Signal(f->fence.Get(),1)) || FAILED(f->fence->SetEventOnCompletion(1,f->event)) || WaitForSingleObject(f->event,15000)!=WAIT_OBJECT_0){f.release();return false;}
    D3D12_RANGE range{0,images*image};if(FAILED(f->readback->Map(0,&range,&mapped)))return false;const auto* bytes=static_cast<const uint8_t*>(mapped);unsigned bad=0,positive=0,components=0,preserved=0;double maximum=0,unblocked=0,blocked=0;
    auto value=[&](unsigned index,unsigned x,unsigned y,unsigned c){uint16_t bits;std::memcpy(&bits,bytes+index*image+y*256+x*8+c*2,2);return Half(bits);};
    for(unsigned pattern=0;pattern<patterns;++pattern)for(unsigned selection=0;selection<3;++selection)for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x){const auto expected=Reference(pattern,x*2+1,y*2+1,selection*.5);for(unsigned c=0;c<4;++c){const double actual=value(pattern*3+selection,x,y,c),delta=std::abs(actual-expected[c]);maximum=std::max(maximum,delta);bad+=!std::isfinite(actual)||delta>std::max(1e-5,std::abs(expected[c])*.002);++components;if(c<3){positive+=actual>0;if(pattern!=0 && pattern!=3 && pattern!=6)bad+=actual!=0;if(!selection)bad+=actual!=0;if(selection==2 && x==0){if(pattern==0)unblocked+=actual;if(pattern==3)blocked+=actual;}}}}
    for(unsigned pattern=0;pattern<patterns;++pattern)for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)for(unsigned c=0;c<4;++c){bad+=value(sourceBase+pattern,x,y,c)!=Colour(pattern,x)[c];++preserved;}
    unsigned composition_bad=0,composition_components=0;double composition_error=0;
    for(unsigned pattern=0;pattern<patterns;++pattern)for(unsigned selection=0;selection<3;++selection)for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){
        const auto p=Vec(Position(pattern,x,y));const auto n=Normal(pattern,x);double total=0,visibility=0,gi_total=0;V indirect{};
        if(n[3]==1){const double fx=(double(x)-1)*.5,fy=(double(y)-1)*.5,tx=fx-std::floor(fx),ty=fy-std::floor(fy);const int bx=int(std::floor(fx)),by=int(std::floor(fy));for(unsigned dy=0;dy<2;++dy)for(unsigned dx=0;dx<2;++dx){const int ax=std::clamp(bx+int(dx),0,3),ay=std::clamp(by+int(dy),0,3);const auto q=Vec(Position(pattern,ax*2+1,ay*2+1));const auto qn=Normal(pattern,ax*2+1);const double agreement=Dot(Vec(n),Vec(qn)),separation=std::abs(Dot(Vec(n),Sub(q,p)));if(qn[3]!=1 || agreement<.5 || separation>=.05)continue;const double w=(dx?tx:1-tx)*(dy?ty:1-ty)*(1-separation/.05)*(agreement-.5)/.5;total+=w;visibility+=w*value(aoBase+pattern,ax,ay,3);const auto receiverMaterial=Surface(pattern,x,y),sampleMaterial=Surface(pattern,ax*2+1,ay*2+1);bool sameMaterial=receiverMaterial[3]>=0 && sampleMaterial[3]>=0;for(unsigned c=0;c<3;++c)sameMaterial&=std::abs(receiverMaterial[c]-sampleMaterial[c])<=.1;if(sameMaterial && value(pattern*3+selection,ax,ay,3)==1){for(unsigned c=0;c<3;++c)indirect[c]+=value(pattern*3+selection,ax,ay,c)*w;gi_total+=w;}}}
        const double ao=total>1e-6?visibility/total:1;const auto colour=Colour(pattern,x),material=Surface(pattern,x,y);for(unsigned c=0;c<4;++c){double expected=colour[c];if(c<3 && n[3]==1){expected-=material[c]*(1-ao)*.5;if(gi_total>1e-6)expected+=indirect[c]/gi_total;}const double actual=value(composeBase+pattern*3+selection,x,y,c),delta=std::abs(actual-expected);composition_error=std::max(composition_error,delta);composition_bad+=!std::isfinite(actual)||delta>std::max(1e-5,std::abs(expected)*.002);if(c==3)composition_bad+=actual!=.625;++composition_components;}
    }
    {std::ofstream pixels(directory/"indirect-readback.bin",std::ios::binary);pixels.write(reinterpret_cast<const char*>(bytes),images*image);}f->readback->Unmap(0,&none);
    unsigned errors=0;if(validation)for(uint64_t i=first;i<validation->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;validation->GetMessage(i,nullptr,&size);std::vector<uint8_t> storage(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());if(SUCCEEDED(validation->GetMessage(i,message,&size)) && message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;REXLOG_ERROR("Indirect lighting validation: {}",message->pDescription);}}
    std::filesystem::create_directories(directory);std::ofstream report(directory/"indirect-results.json");report<<"{\"dispatches\":21,\"components\":"<<components<<",\"invalid\":"<<bad<<",\"max_error\":"<<maximum<<",\"positive_components\":"<<positive<<",\"preserved_hdr_components\":"<<preserved<<",\"unblocked_receiver\":"<<unblocked<<",\"blocked_receiver\":"<<blocked<<",\"guards\":"<<guards<<",\"reuses\":"<<reuses<<",\"managed_dispatches\":21,\"retained_frames\":"<<retained<<",\"composition_components\":"<<composition_components<<",\"composition_invalid\":"<<composition_bad<<",\"composition_max_error\":"<<composition_error<<",\"device_errors\":"<<errors<<",\"scope\":\"Half-resolution diffuse transport using world-space emitter area, distance and facing, screen-surfel segment visibility and energy bound. Seven seeded scenes: coloured wall, coplanar, dark, blocker, backface, unavailable receiver and planar material-colour seam; three strengths, double CPU equation reference and independent zero/positive/blocker conditions; HDR input preserved, metadata/revision/encoding/settings/reuse guards. Explicit linear fixture material data, Managed GI bindings retained before Record, seven frames retained through disable and premature retirement rejected; AO residual plus GI composition checked with independent edge-aware upsample reference, original alpha retained and source revision invalidates publication eligibility; no live game colour-space or actual native publication proof.\"}\n";
    REXLOG_INFO("Indirect diffuse:{} components invalid{}, error{}, positive{}, blocked{} versus{}, device errors{}",components,bad,maximum,positive,blocked,unblocked,errors);
    return !bad && !composition_bad && !errors && positive && unblocked>0 && blocked<unblocked && reuses==patterns*3-1 && report.good();
}
