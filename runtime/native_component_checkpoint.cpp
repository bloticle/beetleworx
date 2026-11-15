#include "native_component_checkpoint.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <map>
#include <array>
#include <cstring>
#include <stdexcept>
#include <sstream>
#include <iomanip>
namespace em2 {namespace {
bool Readable(uint8_t* base,uint32_t at,size_t size){return base && at>=4096 && size && uint64_t(at)+size<=0x100000000ull && GuestReadBatch::Readable(base+at,size);}
void Need(bool yes,const char* why){if(!yes)throw std::runtime_error(why);}
struct Host {DWORD error=GetLastError();unsigned csr=_mm_getcsr();~Host(){SetLastError(error);_mm_setcsr(csr);}};
unsigned PodSize(std::string_view type){
    if(type=="Boolean")return 1;
    if(type=="Unsigned Short")return 2;
    if(type=="Float" || type=="Integer" || type=="Unsigned Integer")return 4;
    if(type=="Point2")return 8;
    if(type=="Point3" || type=="Color (RGB)")return 12;
    if(type=="Color (RGBA)")return 16;
    // Original transform setter 827D4D68 copies six 64-bit chunks, not nine
    // tightly packed floats. Xbox 360 Matrix3 uses three padded 16-byte rows.
    if(type=="Matrix3")return 48;
    return 0;
}
bool DirectOffset(uint8_t* base,uint32_t getter,uint32_t& offset){
    if(getter<0x822F0000 || getter>=0x83346908 || (getter&3) || !Readable(base,getter,8))return false;
    const auto instruction=PPC_LOAD_U32(getter);
    // Exact original `return &this->member`: addi r3,r3,offset; blr.
    // Do not mistake `return &box->value` (r4) or pointer loads for members.
    if((instruction&0xFFFF0000)!=0x38630000 || PPC_LOAD_U32(getter+4)!=0x4E800020 || int16_t(instruction)<0)return false;
    offset=uint16_t(instruction);return offset>=48;
}
bool EmbeddedOffset(uint8_t* base,uint32_t accessor,uint32_t& offset){
    if(accessor<0x822F0000 || accessor>=0x83346908 || (accessor&3) || !Readable(base,accessor,8))return false;
    const auto op=PPC_LOAD_U32(accessor);if((op&0xFFFF0000)!=0x38640000 || PPC_LOAD_U32(accessor+4)!=0x4E800020 || int16_t(op)<48)return false;
    offset=uint16_t(op);return true;
}
struct SplineProgress {uint32_t object=0,definition=0,word=0;std::array<uint32_t,4> route{};std::array<uint32_t,3> drivers{};};
void CheckSpline(uint8_t* base,const SplineProgress& p){
    Need(Readable(base,p.object,240) && PPC_LOAD_U32(p.object)==0x820CB3C8 && PPC_LOAD_U32(p.object+8)==0x820CB238 && PPC_LOAD_U32(p.object+48)==p.definition,"Native spline follower ownership changed");
    unsigned i=0;for(unsigned offset:{124u,136u,140u,144u})Need(PPC_LOAD_U32(p.object+offset)==p.route[i++],"Native spline route changed; scalar progress restore requires route lifetime support");
    i=0;for(unsigned offset:{168u,172u,176u})Need(PPC_LOAD_U32(p.object+offset)==p.drivers[i++],"Native spline physics driver changed");
}
// Original 8278D4D8/8278A4A8 use these as position/velocity vectors;
// 82EB8738 reads controller+304 and writes +352/+368. +664 is the
// floating height correction written by 8278D4D8, not a pointer or handle.
constexpr std::array<unsigned,18> motion_offsets{288,292,296,304,308,312,352,356,360,368,372,376,416,420,424,664,464,468};
constexpr std::array<unsigned,6> motion_owner_offsets{48,80,92,108,116,128};
// Original 82790300 writes only two vectors and their magnitudes in each
// 64-byte ring slot. The 24 padding bytes per slot are never copied here.
struct MotionState {uint32_t object=0,primary=0,face=0,data=0,capacity=0;std::array<uint32_t,3> indices{};std::vector<std::array<uint8_t,40>> history;std::array<uint32_t,6> owners{};std::array<uint32_t,18> words{};};
bool MotionClass(uint32_t primary,uint32_t face){return(primary==0x82082028 && face==0x82081E98) || (primary==0x8210B598 && face==0x8210B408);}
void ReadMotionRecord(uint8_t* base,uint32_t at,std::array<uint8_t,40>& bytes){std::memcpy(bytes.data(),base+at,20);std::memcpy(bytes.data()+20,base+at+32,20);}
void WriteMotion(uint8_t* base,const MotionState& p){
    for(unsigned i=0;i<motion_offsets.size();++i)PPC_STORE_U32(p.object+motion_offsets[i],p.words[i]);
    for(unsigned i=0;i<p.history.size();++i){const auto at=p.data+i*64;std::memcpy(base+at,p.history[i].data(),20);std::memcpy(base+at+32,p.history[i].data()+20,20);}
    for(unsigned i=0;i<3;++i)PPC_STORE_U32(p.object+488+i*4,p.indices[i]);
}
void CheckMotion(uint8_t* base,const MotionState& p){
    Need(Readable(base,p.object,668) && MotionClass(p.primary,p.face) && PPC_LOAD_U32(p.object)==p.primary && PPC_LOAD_U32(p.object+8)==p.face,"Native basic motion component class/extent changed");
    Need(PPC_LOAD_U32(p.object+480)==p.data && PPC_LOAD_U32(p.object+484)==p.capacity && p.capacity<=4096 && p.history.size()==p.capacity,"Native movement history allocation changed; lifetime support required");
    Need(!p.capacity || (p.data && !(p.data&15) && Readable(base,p.data,size_t(p.capacity)*64)),"Native movement history allocation unavailable");
    for(unsigned offset:{488u,492u})Need(p.capacity?PPC_LOAD_U32(p.object+offset)<p.capacity:!PPC_LOAD_U32(p.object+offset),"Native movement history index out of bounds");
    Need(PPC_LOAD_U32(p.object+496)<=p.capacity,"Native movement history count out of bounds");
    for(unsigned i=0;i<motion_owner_offsets.size();++i)Need(PPC_LOAD_U32(p.object+motion_owner_offsets[i])==p.owners[i],"Native basic motion controller dependency changed; lifetime support required");
}
// Original 827D4CC8/4CD0/4CC0 return cached world translation,
// padded Matrix3 and scale directly. Restoring only reflected local properties
// leaves these downstream native getter results at the future frame.
constexpr std::array<unsigned,9> transform_owner_offsets{268,312,316,320,400,404,408,416,420};
// 827D78D0 writes the64-byte parent basis at336; 827D6848 composes it
// with local pose. Original constructor827DA0C8 initializes the version412 and
// flag byte414. +312/+316/+320 is the owned child array header, not a parent.
struct TransformCache {uint32_t object=0,definition=0;std::array<uint32_t,9> owners{};std::vector<uint32_t> children;std::array<uint8_t,131> bytes{};};
void CheckTransform(uint8_t* base,const TransformCache& p){
    Need(Readable(base,p.object,424) && PPC_LOAD_U32(p.object)==0x820904D0 && PPC_LOAD_U32(p.object+8)==0x82090340 && PPC_LOAD_U32(p.object+48)==p.definition,"Native transform cache class/definition changed");
    for(unsigned i=0;i<transform_owner_offsets.size();++i)Need(PPC_LOAD_U32(p.object+transform_owner_offsets[i])==p.owners[i],"Native transform parent/children/source/callback ownership changed");
    const auto data=p.owners[1],count=p.owners[2],capacity=p.owners[3]&0x3FFFFFFF;
    Need(count<=capacity && capacity<=65536 && p.children.size()==count && (!count || Readable(base,data,size_t(count)*4)),"Native transform child list extent invalid");
    for(unsigned i=0;i<count;++i)Need(PPC_LOAD_U32(data+i*4)==p.children[i],"Native transform child list identities changed");
}
void ReadTransform(uint8_t* base,uint32_t object,std::array<uint8_t,131>& bytes){std::memcpy(bytes.data(),base+object+172,12);std::memcpy(bytes.data()+12,base+object+208,48);std::memcpy(bytes.data()+60,base+object+260,4);std::memcpy(bytes.data()+64,base+object+336,64);std::memcpy(bytes.data()+128,base+object+412,3);}
void WriteTransform(uint8_t* base,const TransformCache& p){std::memcpy(base+p.object+172,p.bytes.data(),12);std::memcpy(base+p.object+208,p.bytes.data()+12,48);std::memcpy(base+p.object+260,p.bytes.data()+60,4);std::memcpy(base+p.object+336,p.bytes.data()+64,64);std::memcpy(base+p.object+412,p.bytes.data()+128,3);}
// Hover constructor 82891A80 initializes three floating runtime values at
// +140/+144/+148; 82891A60 selects the +148 override using byte +136.
constexpr std::array<unsigned,18> hover_driver_float_offsets{48,52,80,84,88,92,96,100,104,108,112,116,120,124,128,132,136,148};
constexpr std::array<unsigned,9> hover_driver_flag_offsets{140,141,142,143,144,152,153,154,155};
struct HoverRing {uint32_t data=0,capacity=0;std::array<uint32_t,3> indices{};std::vector<std::array<uint8_t,16>> vectors;};
struct HoverState {uint32_t object=0,definition=0,driver=0;std::array<uint32_t,3> words{};uint8_t override_active=0;
    std::array<uint32_t,18> driver_floats{};std::array<uint8_t,9> driver_flags{};std::array<HoverRing,2> rings;std::array<uint8_t,20> strings{};
};
void CheckHover(uint8_t* base,const HoverState& p){
    Need(Readable(base,p.object,152) && PPC_LOAD_U32(p.object)==0x820C0E28 && PPC_LOAD_U32(p.object+8)==0x820C0C98 && PPC_LOAD_U32(p.object+48)==p.definition && PPC_LOAD_U32(p.object+132)==p.driver,"Native hover state class/definition/driver changed");
    Need(p.driver && Readable(base,p.driver,176) && PPC_LOAD_U32(p.driver)==0x820C27D8,"Native hover driver class/extent changed");
    Need(!std::memcmp(base+p.driver+56,p.strings.data(),20),"Native hover driver string ownership changed");
    for(unsigned i=0;i<2;++i){const auto at=p.driver+8+i*20;const auto& r=p.rings[i];
        Need(PPC_LOAD_U32(at)==r.data && PPC_LOAD_U32(at+4)==r.capacity && r.capacity<=4096 && r.vectors.size()==r.capacity,"Native hover vector history allocation changed");
        Need(!r.capacity || (r.data && !(r.data&15) && Readable(base,r.data,size_t(r.capacity)*16)),"Native hover vector history extent unavailable");
        for(unsigned offset:{8u,12u})Need(r.capacity?PPC_LOAD_U32(at+offset)<r.capacity:!PPC_LOAD_U32(at+offset),"Native hover vector history index out of bounds");
        Need(PPC_LOAD_U32(at+16)<=r.capacity,"Native hover vector history count out of bounds");
    }
}
void WriteHover(uint8_t* base,const HoverState& p){
    for(unsigned i=0;i<3;++i)PPC_STORE_U32(p.object+140+i*4,p.words[i]);PPC_STORE_U8(p.object+136,p.override_active);
    for(unsigned i=0;i<hover_driver_float_offsets.size();++i)PPC_STORE_U32(p.driver+hover_driver_float_offsets[i],p.driver_floats[i]);
    for(unsigned i=0;i<hover_driver_flag_offsets.size();++i)PPC_STORE_U8(p.driver+hover_driver_flag_offsets[i],p.driver_flags[i]);
    for(unsigned i=0;i<2;++i){const auto& r=p.rings[i];for(unsigned slot=0;slot<r.vectors.size();++slot)std::memcpy(base+r.data+slot*16,r.vectors[slot].data(),16);
        for(unsigned j=0;j<3;++j)PPC_STORE_U32(p.driver+16+i*20+j*4,r.indices[j]);}
}
void MatchHover(uint8_t* base,const HoverState& p){
    for(unsigned i=0;i<3;++i)Need(PPC_LOAD_U32(p.object+140+i*4)==p.words[i],"Native hover runtime scalar differs");Need(PPC_LOAD_U8(p.object+136)==p.override_active,"Native hover override differs");
    for(unsigned i=0;i<hover_driver_float_offsets.size();++i)Need(PPC_LOAD_U32(p.driver+hover_driver_float_offsets[i])==p.driver_floats[i],"Native hover driver floating runtime differs");
    for(unsigned i=0;i<hover_driver_flag_offsets.size();++i)Need(PPC_LOAD_U8(p.driver+hover_driver_flag_offsets[i])==p.driver_flags[i],"Native hover driver flag differs");
    for(unsigned i=0;i<2;++i){const auto& r=p.rings[i];for(unsigned j=0;j<3;++j)Need(PPC_LOAD_U32(p.driver+16+i*20+j*4)==r.indices[j],"Native hover vector history indices/count differ");
        for(unsigned slot=0;slot<r.vectors.size();++slot)Need(!std::memcmp(base+r.data+slot*16,r.vectors[slot].data(),16),"Native hover vector history record differs");}
}
struct Field {
    uint32_t object=0,primary=0,face=0,descriptor=0,descriptor_table=0,getter=0,offset=0,size=0,box=0,box_table=0,box_accessor=0;
    std::array<uint8_t,48> bytes{};
    std::string component,name,type;
    uint32_t Address()const{return (box?box:object)+offset;}
};
// hkaPose constructor 823029A8 / initializer 823021C8 establish these four
// hkArrays. 82840050 and 82302370 read/update the model/local bone poses and
// validity bits during gameplay bone attachment queries, not just rendering.
constexpr std::array<unsigned,4> pose_array_offsets{4,16,28,44};
constexpr std::array<unsigned,4> pose_array_strides{48,48,4,4};
constexpr std::array<unsigned,5> animation_owner_offsets{48,180,192,196,208};
struct PoseArray {std::array<uint32_t,3> header{};std::vector<uint8_t> bytes;};
struct AnimationPose {uint32_t object=0,pose=0,skeleton=0,bones=0,floats=0,output=0,output_data=0,tracks=0;uint8_t output_owned=0;std::vector<uint8_t> previous_output;std::array<uint32_t,5> owners{};std::array<uint8_t,2> valid{};std::array<PoseArray,4> arrays;};
void CheckAnimationPose(uint8_t* base,const AnimationPose& p){
    Need(Readable(base,p.object,212) && PPC_LOAD_U32(p.object)==0x820B34F0 && PPC_LOAD_U32(p.object+8)==0x820B3360,"Native animation component class/extent changed");
    for(unsigned i=0;i<animation_owner_offsets.size();++i)Need(PPC_LOAD_U32(p.object+animation_owner_offsets[i])==p.owners[i],"Native animation pose/graph/context ownership changed");
    // 823B6DF8 reads the previous generator output at+28.
    // 823E3FA8 allocates one flat track block;823D73E8 writes byte size/count
    // at0/4 and16-byte track headers with relative data offsets, not hkArrays.
    const auto character=p.owners[3];
    if(character)Need(Readable(base,character,64) && PPC_LOAD_U32(character)==0x8201E410 && PPC_LOAD_U32(character+60)==p.owners[2] && PPC_LOAD_U32(character+28)==p.output,"Native behavior character/output ownership changed");
    else Need(!p.pose && !p.output,"Uninitialized native animation acquired pose/output state");
    if(p.output){
        Need(Readable(base,p.output,8) && PPC_LOAD_U32(p.output)==p.output_data && PPC_LOAD_U8(p.output+4)==p.output_owned,"Native previous generator-output allocation changed");
        Need(Readable(base,p.output_data,p.previous_output.size()) && PPC_LOAD_U32(p.output_data)==p.previous_output.size() && PPC_LOAD_U32(p.output_data+4)==p.tracks && p.tracks<=256 && p.previous_output.size()>=size_t(p.tracks+1)*16,"Native previous generator-output extent changed");
    }
    if(!p.pose)return;
    Need(Readable(base,p.pose,56) && PPC_LOAD_U32(p.pose)==p.skeleton && Readable(base,p.skeleton,68) && PPC_LOAD_U32(p.skeleton+28)==p.bones && PPC_LOAD_U32(p.skeleton+64)==p.floats,"Native pose skeleton identity/extent changed");
    Need(p.bones<=4096 && p.floats<=4096,"Native pose exceeds bounded skeleton extent");
    for(unsigned i=0;i<4;++i){const auto& a=p.arrays[i];const auto at=p.pose+pose_array_offsets[i];
        for(unsigned j=0;j<3;++j)Need(PPC_LOAD_U32(at+j*4)==a.header[j],"Native pose array allocation/count changed; lifetime rebuilding required");
        const auto count=a.header[1],capacity=a.header[2]&0x3FFFFFFF;
        Need(count==(i==3?p.floats:p.bones) && count<=capacity && capacity<=65536 && a.bytes.size()==size_t(count)*pose_array_strides[i],"Native pose array extent invalid");
        Need(!count || (a.header[0] && !(a.header[0]&(i<2?15u:3u)) && Readable(base,a.header[0],a.bytes.size())),"Native pose array payload unavailable");
    }
}
void WriteAnimationPose(uint8_t* base,const AnimationPose& p){
    if(!p.previous_output.empty())std::memcpy(base+p.output_data,p.previous_output.data(),p.previous_output.size());
    if(!p.pose)return;
    for(const auto& a:p.arrays)if(!a.bytes.empty())std::memcpy(base+a.header[0],a.bytes.data(),a.bytes.size());
    std::memcpy(base+p.pose+40,p.valid.data(),2);
}
void MatchAnimationPose(uint8_t* base,const AnimationPose& p){
    if(!p.previous_output.empty())Need(!std::memcmp(base+p.output_data,p.previous_output.data(),p.previous_output.size()),"Native previous behavior generator output differs");
    if(!p.pose)return;
    Need(!std::memcmp(base+p.pose+40,p.valid.data(),2),"Native skeletal pose validity differs");
    for(unsigned i=0;i<4;++i){const auto& a=p.arrays[i];if(!a.bytes.empty() && std::memcmp(base+a.header[0],a.bytes.data(),a.bytes.size())){
        std::ostringstream why;why<<"Native skeletal pose payload differs at animation 0x"<<std::hex<<p.object<<" array "<<std::dec<<i;throw std::runtime_error(why.str());
    }}
}
bool BoxOffset(uint8_t* base,uint32_t getter){
    return getter>=0x822F0000 && getter<0x83346908 && !(getter&3) && Readable(base,getter,8) &&
        PPC_LOAD_U32(getter)==0x38640004 && PPC_LOAD_U32(getter+4)==0x4E800020;
}
void CheckField(uint8_t* base,const Field& f){
    uint32_t offset=0;
    if(f.box_accessor){uint32_t member=0;Need(Readable(base,f.descriptor_table,20) && PPC_LOAD_U32(f.descriptor_table+16)==f.box_accessor && EmbeddedOffset(base,f.box_accessor,member) && uint64_t(f.object)+member==f.box,"Embedded property box accessor/owner changed");}
    Need(Readable(base,f.object,12) && PPC_LOAD_U32(f.object)==f.primary && PPC_LOAD_U32(f.object+8)==f.face &&
        Readable(base,f.descriptor,20) && PPC_LOAD_U32(f.descriptor)==f.descriptor_table && PPC_LOAD_U32(f.descriptor+16)==f.getter &&
        (f.box ? (Readable(base,f.box,4) && PPC_LOAD_U32(f.box)==f.box_table && BoxOffset(base,f.getter) && f.offset==4)
               : (DirectOffset(base,f.getter,offset) && offset==f.offset)) && uint64_t(f.box?f.box:f.object)+f.offset+f.size<=0x100000000ull && Readable(base,f.Address(),f.size),
        "Reflected native field ownership/metadata changed; restore rejected before writes");
}
}
struct NativeComponentFieldCheckpoint::Impl {
    NativeEntityOwnership ownership;std::vector<NativeComponentPropertyCache> caches;std::vector<Field> fields;std::vector<SplineProgress> splines;std::vector<MotionState> motions;std::vector<TransformCache> transforms;std::vector<HoverState> hovers;uint64_t epoch=0;uint32_t thread=0;
    std::vector<AnimationPose> animation_poses;
    std::map<uint32_t,std::string> component_names;size_t payload=0,unsupported=0;
};
namespace {
template<class State>void CheckState(uint8_t* base,const State& s,uint64_t epoch){
Need(epoch && epoch==s.epoch && s.thread==GetCurrentThreadId(),"Component field checkpoint belongs to another world/owner epoch");
        NativeEntityOwnership current;const char* why=nullptr;uint32_t where=0;
        Need(ReadNativeEntityOwnership(base,current,why,where) && current==s.ownership,"Native entity/component lifetimes changed; field restore rejected");
        for(const auto& cache:s.caches){
            Need(Readable(base,cache.object+36,12) && PPC_LOAD_U32(cache.object+36)==cache.data && PPC_LOAD_U32(cache.object+40)==cache.count && PPC_LOAD_U32(cache.object+44)==cache.capacity,
                "Native property cache allocation changed; restore rejected before writes");
            Need(!cache.count || Readable(base,cache.data,size_t(cache.count)*4),"Native property cache entries unavailable");
            for(size_t i=0;i<cache.boxes.size();++i)Need(PPC_LOAD_U32(cache.data+4*uint32_t(i))==cache.boxes[i].address &&
                Readable(base,cache.boxes[i].address,4) && PPC_LOAD_U32(cache.boxes[i].address)==cache.boxes[i].table,"Native property box lifetime changed; restore rejected before writes");
        }
        for(const auto& f:s.fields)CheckField(base,f);
        for(const auto& p:s.splines)CheckSpline(base,p);
        for(const auto& p:s.motions)CheckMotion(base,p);
        for(const auto& p:s.transforms)CheckTransform(base,p);
        for(const auto& p:s.hovers)CheckHover(base,p);
        for(const auto& p:s.animation_poses)CheckAnimationPose(base,p);
 }
}
bool NativeComponentFieldCheckpoint::Matches(uint8_t* base,uint64_t epoch,std::string& error)const{
    Host host;GuestReadBatch reads;try{
        const auto& s=*impl_;CheckState(base,s,epoch);
        for(const auto& p:s.splines)Need(PPC_LOAD_U32(p.object+128)==p.word,"Native spline follower progress differs from checkpoint");
        for(const auto& p:s.motions)for(unsigned i=0;i<motion_offsets.size();++i)if(PPC_LOAD_U32(p.object+motion_offsets[i])!=p.words[i]){
            std::ostringstream why;why<<"Native basic motion runtime scalar differs at object 0x"<<std::hex<<p.object<<" +"<<std::dec<<motion_offsets[i]<<" expected/actual "<<std::hex<<p.words[i]<<"/"<<PPC_LOAD_U32(p.object+motion_offsets[i]);throw std::runtime_error(why.str());
        }
        for(const auto& p:s.motions){
            for(unsigned i=0;i<3;++i)Need(PPC_LOAD_U32(p.object+488+i*4)==p.indices[i],"Native movement history indices/count differ");
            for(unsigned i=0;i<p.history.size();++i){std::array<uint8_t,40> now{};ReadMotionRecord(base,p.data+i*64,now);Need(now==p.history[i],"Native movement history vector/magnitude differs");}
        }
        for(const auto& p:s.transforms){std::array<uint8_t,131> now{};ReadTransform(base,p.object,now);if(now!=p.bytes){std::ostringstream why;why<<"Native transform cached world pose differs at object 0x"<<std::hex<<p.object;throw std::runtime_error(why.str());}}
        for(const auto& p:s.hovers)MatchHover(base,p);
        for(const auto& p:s.animation_poses)MatchAnimationPose(base,p);
        size_t changed=0;std::map<std::string,size_t> groups;std::string first;
        for(const auto& f:s.fields)if(std::memcmp(base+f.Address(),f.bytes.data(),f.size)){
            ++changed;++groups[f.component];if(!first.empty())continue;
            unsigned byte=0;while(byte<f.size && base[f.Address()+byte]==f.bytes[byte])++byte;
            std::ostringstream why;why<<"Native reflected payload differs: "<<f.component<<"/"<<f.name<<" ("<<f.type<<") at 0x"<<std::hex<<f.Address()<<" byte "<<std::dec<<byte<<" expected "<<unsigned(f.bytes[byte])<<" actual "<<unsigned(base[f.Address()+byte]);
            for(const auto& entity:s.ownership.entities){bool owns=false;for(const auto& c:entity.components)owns|=c[0]==f.object+8;if(!owns)continue;
                why<<" entity 0x"<<std::hex<<entity.entity<<" handle 0x"<<entity.handle<<" GUID";for(auto word:entity.id)why<<" "<<word;
                why<<" classes [";for(const auto& c:entity.components){auto name=s.component_names.find(c[0]-8);if(name!=s.component_names.end())why<<name->second<<",";}why<<"]";break;
            }
            if(f.size>=4){why<<" words expected/actual";for(unsigned word=0;word<std::min(4u,f.size/4);++word){const auto p=f.bytes.data()+word*4;
                const auto expected=uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];why<<" "<<std::hex<<expected<<"/"<<PPC_LOAD_U32(f.Address()+word*4);}}
            first=why.str();
        }
        if(changed){std::ostringstream why;why<<first<<"; changed "<<changed<<" fields [";for(const auto& [name,count]:groups)why<<name<<":"<<std::dec<<count<<",";why<<"]";throw std::runtime_error(why.str());}
        error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
NativeComponentFieldCheckpoint::NativeComponentFieldCheckpoint():impl_(std::make_unique<Impl>()){}
NativeComponentFieldCheckpoint::~NativeComponentFieldCheckpoint()=default;
std::string NativeComponentFieldCheckpoint::DescribeEntity(uint32_t entity)const{
    const auto& s=*impl_;std::ostringstream out;
    for(const auto& e:s.ownership.entities)if(e.entity==entity){out<<"entity 0x"<<std::hex<<entity<<" handle 0x"<<e.handle<<" GUID";for(auto n:e.id)out<<" "<<n;out<<" classes [";
        for(const auto& c:e.components){auto name=s.component_names.find(c[0]-8);if(name!=s.component_names.end())out<<name->second<<"@"<<std::hex<<c[0]-8<<",";}out<<"]";return out.str();}
    out<<"unidentified owner 0x"<<std::hex<<entity;return out.str();
}
size_t NativeComponentFieldCheckpoint::fields()const{return impl_->fields.size();}
size_t NativeComponentFieldCheckpoint::bytes()const{return impl_->payload;}
size_t NativeComponentFieldCheckpoint::unsupported()const{return impl_->unsupported;}
bool NativeComponentFieldCheckpoint::Capture(uint8_t* base,const NativeEntityOwnership& ownership,std::span<const NativeComponentSchema> schemas,uint64_t epoch,std::string& error){
    Host host;GuestReadBatch reads;
    try{
        Need(epoch && !impl_->epoch,"Component field checkpoint must be empty with a nonzero world epoch");
        NativeEntityOwnership current;const char* why=nullptr;uint32_t where=0;
        Need(ReadNativeEntityOwnership(base,current,why,where) && current==ownership,"Native component field capture owner changed");
        auto ready=std::make_unique<Impl>();ready->ownership=ownership;ready->epoch=epoch;ready->thread=GetCurrentThreadId();
        std::map<std::pair<uint32_t,uint32_t>,const NativeComponentSchema*> lookup;
        for(const auto& schema:schemas)Need(lookup.emplace(std::pair{schema.primary_table,schema.interface_table},&schema).second,"Duplicate local component schema");
        for(const auto& entity:ownership.entities)for(const auto& component:entity.components){
            const auto object=component[0]-8;Need(Readable(base,object,12),"Component primary object unavailable");
            auto found=lookup.find({PPC_LOAD_U32(object),component[1]});Need(found!=lookup.end(),"Missing local component schema");const auto& schema=*found->second;ready->component_names.emplace(object,schema.name);
            for(const auto& p:schema.declarations){
                uint32_t offset=0;const auto size=PodSize(p.type);
                if(!p.direct || !*p.direct || !p.array || *p.array || !size){++ready->unsupported;continue;}
                const bool direct=DirectOffset(base,p.getter,offset);uint32_t accessor=0;
                if(!direct){
                    if(!BoxOffset(base,p.getter) || !Readable(base,p.table,20) || !EmbeddedOffset(base,accessor=PPC_LOAD_U32(p.table+16),offset)){++ready->unsupported;continue;}
                }
                Field f;f.object=object;f.primary=schema.primary_table;f.face=schema.interface_table;f.descriptor=p.descriptor;f.descriptor_table=p.table;
                f.getter=p.getter;f.offset=offset;f.size=size;f.component=schema.name;f.name=p.name;f.type=p.type;
                if(!direct){Need(uint64_t(object)+offset+4+size<=0x100000000ull && Readable(base,object+offset,4+size),"Embedded native property box unavailable");f.box=object+offset;f.box_table=PPC_LOAD_U32(f.box);f.box_accessor=accessor;f.offset=4;
                    Need(f.box_table>=0x82000000 && f.box_table<0x83400000 && !(f.box_table&3),"Embedded native property box class unavailable");}
                CheckField(base,f);std::memcpy(f.bytes.data(),base+f.Address(),size);ready->payload+=size;
                Need(ready->payload<=8u*1024*1024 && ready->fields.size()<262144,"Component field checkpoint exceeds bounded storage");
                ready->fields.push_back(std::move(f));
            }
            if(schema.name=="AnimationComponent"){
                AnimationPose p;p.object=object;
                Need(schema.primary_table==0x820B34F0 && schema.interface_table==0x820B3360 && Readable(base,object,212),"Native animation pose layout differs from audited bone query");
                for(unsigned i=0;i<animation_owner_offsets.size();++i)p.owners[i]=PPC_LOAD_U32(object+animation_owner_offsets[i]);
                p.pose=p.owners[1];
                const auto character=p.owners[3];
                if(character){Need(Readable(base,character,64) && PPC_LOAD_U32(character)==0x8201E410,"Native behavior character differs from audited generator layout");p.output=PPC_LOAD_U32(character+28);}
                else Need(!p.pose,"Native pose exists without an initialized behavior character");
                if(p.output){Need(Readable(base,p.output,8),"Native previous generator-output header unavailable");p.output_data=PPC_LOAD_U32(p.output);p.output_owned=PPC_LOAD_U8(p.output+4);
                    Need(Readable(base,p.output_data,8),"Native previous generator-output data unavailable");const auto size=PPC_LOAD_U32(p.output_data);p.tracks=PPC_LOAD_U32(p.output_data+4);
                    Need(size>=16 && size<=1024*1024 && p.tracks<=256 && size>=size_t(p.tracks+1)*16 && Readable(base,p.output_data,size),"Native previous generator-output bounds invalid");
                    ready->payload+=size;Need(ready->payload<=8u*1024*1024,"Native previous generator outputs exceed checkpoint budget");p.previous_output.assign(base+p.output_data,base+p.output_data+size);
                }
                if(p.pose){
                    Need(Readable(base,p.pose,56),"Native skeletal pose header unavailable");p.skeleton=PPC_LOAD_U32(p.pose);
                    Need(Readable(base,p.skeleton,68),"Native skeletal pose skeleton unavailable");p.bones=PPC_LOAD_U32(p.skeleton+28);p.floats=PPC_LOAD_U32(p.skeleton+64);
                    Need(p.bones<=4096 && p.floats<=4096,"Native skeletal pose exceeds bounded capture extent");
                    for(unsigned i=0;i<4;++i){auto& a=p.arrays[i];const auto at=p.pose+pose_array_offsets[i];for(unsigned j=0;j<3;++j)a.header[j]=PPC_LOAD_U32(at+j*4);
                        Need(a.header[1]==(i==3?p.floats:p.bones),"Native pose array count differs from skeleton");
                        a.bytes.resize(size_t(a.header[1])*pose_array_strides[i]);ready->payload+=a.bytes.size();
                        Need(ready->payload<=8u*1024*1024,"Native skeletal pose exceeds checkpoint payload budget");
                    }
                    CheckAnimationPose(base,p);std::memcpy(p.valid.data(),base+p.pose+40,2);
                    for(auto& a:p.arrays)if(!a.bytes.empty())std::memcpy(a.bytes.data(),base+a.header[0],a.bytes.size());ready->payload+=2;
                }
                CheckAnimationPose(base,p);ready->animation_poses.push_back(std::move(p));
            }
            if(schema.name=="HoverStateComponent"){
                Need(schema.primary_table==0x820C0E28 && schema.interface_table==0x820C0C98 && Readable(base,object,152),"Native hover layout differs from audited constructor");
                HoverState p;p.object=object;p.definition=PPC_LOAD_U32(object+48);p.driver=PPC_LOAD_U32(object+132);p.override_active=PPC_LOAD_U8(object+136);
                for(unsigned i=0;i<3;++i)p.words[i]=PPC_LOAD_U32(object+140+i*4);
                Need(p.driver && Readable(base,p.driver,176) && PPC_LOAD_U32(p.driver)==0x820C27D8,"Native hover driver differs from audited 82899028 constructor");
                std::memcpy(p.strings.data(),base+p.driver+56,20);
                for(unsigned i=0;i<hover_driver_float_offsets.size();++i)p.driver_floats[i]=PPC_LOAD_U32(p.driver+hover_driver_float_offsets[i]);
                for(unsigned i=0;i<hover_driver_flag_offsets.size();++i)p.driver_flags[i]=PPC_LOAD_U8(p.driver+hover_driver_flag_offsets[i]);
                for(unsigned i=0;i<2;++i){const auto at=p.driver+8+i*20;auto& r=p.rings[i];r.data=PPC_LOAD_U32(at);r.capacity=PPC_LOAD_U32(at+4);Need(r.capacity<=4096,"Native hover history exceeds bounded capacity");
                    r.vectors.resize(r.capacity);for(unsigned j=0;j<3;++j)r.indices[j]=PPC_LOAD_U32(at+8+j*4);}
                CheckHover(base,p);for(auto& r:p.rings)for(unsigned slot=0;slot<r.vectors.size();++slot)std::memcpy(r.vectors[slot].data(),base+r.data+slot*16,16);
                ready->payload+=118+(p.rings[0].capacity+p.rings[1].capacity)*16;Need(ready->payload<=8u*1024*1024,"Native hover runtime exceeds checkpoint budget");ready->hovers.push_back(std::move(p));
            }
            if(schema.name=="JPSTransformationComponent"){
                TransformCache p;p.object=object;
                Need(schema.primary_table==0x820904D0 && schema.interface_table==0x82090340 && Readable(base,object,424),"Native transform cache differs from audited getter layout");
                p.definition=PPC_LOAD_U32(object+48);for(unsigned i=0;i<transform_owner_offsets.size();++i)p.owners[i]=PPC_LOAD_U32(object+transform_owner_offsets[i]);
                const auto count=p.owners[2],capacity=p.owners[3]&0x3FFFFFFF;Need(count<=capacity && capacity<=65536 && (!count || Readable(base,p.owners[1],size_t(count)*4)),"Native transform child list exceeds bounded extent");
                p.children.resize(count);for(unsigned i=0;i<count;++i)p.children[i]=PPC_LOAD_U32(p.owners[1]+i*4);
                CheckTransform(base,p);ReadTransform(base,object,p.bytes);ready->transforms.push_back(std::move(p));ready->payload+=131;
            }
            if(schema.name=="BasicMotionControllerComponent" || schema.name=="MickeyPlayerMotionControllerComponent"){
                MotionState p;p.object=object;p.primary=schema.primary_table;p.face=schema.interface_table;
                Need(MotionClass(p.primary,p.face) && Readable(base,object,668),"Basic motion runtime layout differs from audited native methods");
                for(unsigned i=0;i<motion_owner_offsets.size();++i)p.owners[i]=PPC_LOAD_U32(object+motion_owner_offsets[i]);
                for(unsigned i=0;i<motion_offsets.size();++i)p.words[i]=PPC_LOAD_U32(object+motion_offsets[i]);
                p.data=PPC_LOAD_U32(object+480);p.capacity=PPC_LOAD_U32(object+484);Need(p.capacity<=4096,"Native movement history exceeds bounded capacity");
                p.history.resize(p.capacity);for(unsigned i=0;i<3;++i)p.indices[i]=PPC_LOAD_U32(object+488+i*4);
                CheckMotion(base,p);for(unsigned i=0;i<p.history.size();++i)ReadMotionRecord(base,p.data+i*64,p.history[i]);
                ready->payload+=84+p.history.size()*40;Need(ready->payload<=8u*1024*1024,"Native movement history exceeds checkpoint payload budget");ready->motions.push_back(std::move(p));
            }
            if(schema.name=="SplineFollowerComponent"){
                Need(schema.primary_table==0x820CB3C8 && schema.interface_table==0x820CB238 && Readable(base,object,240),"Native spline progress layout differs from audited 828C8EB0 constructor");
                SplineProgress p;p.object=object;p.definition=PPC_LOAD_U32(object+48);p.word=PPC_LOAD_U32(object+128);
                unsigned i=0;for(unsigned offset:{124u,136u,140u,144u})p.route[i++]=PPC_LOAD_U32(object+offset);
                i=0;for(unsigned offset:{168u,172u,176u})p.drivers[i++]=PPC_LOAD_U32(object+offset);
                CheckSpline(base,p);ready->splines.push_back(p);ready->payload+=4;
            }
            if(!schema.properties)++ready->unsupported;
        }
        Need(ReadNativeComponentPropertyCaches(base,ownership,ready->caches,error),"Native property cache inventory failed");
        size_t boxed=0;
        for(const auto& cache:ready->caches)for(const auto& box:cache.boxes){
            const auto& p=box.property;const auto size=PodSize(p.type);
            if(!p.array || *p.array || !size || !BoxOffset(base,p.getter)){++ready->unsupported;continue;}
            Field f;f.object=cache.object;f.primary=PPC_LOAD_U32(f.object);f.face=PPC_LOAD_U32(f.object+8);
            f.box=box.address;f.box_table=box.table;f.descriptor=p.descriptor;f.descriptor_table=p.table;f.getter=p.getter;f.offset=4;f.size=size;f.name=p.name;f.type=p.type;
            CheckField(base,f);std::memcpy(f.bytes.data(),base+f.Address(),size);ready->payload+=size;
            Need(ready->payload<=8u*1024*1024 && ready->fields.size()<262144,"Property box checkpoint exceeds bounded storage");
            ready->fields.push_back(std::move(f));++boxed;
        }
        REXLOG_INFO("Native cached property checkpoint: {} component cache headers, {} audited existing POD boxes; no lazy creation",ready->caches.size(),boxed);
        size_t embedded=0;for(const auto& f:ready->fields)embedded+=f.box_accessor!=0;
        REXLOG_INFO("Native embedded property checkpoint: {} audited inline POD boxes, {} native spline progress scalars; original member accessors decoded, routes/drivers fenced",embedded,ready->splines.size());
        REXLOG_INFO("Native hover runtime checkpoint: {} exact-class components, component scalars/override plus owned driver timers/vectors/flags and two vector-history rings, string/allocation identities fenced",ready->hovers.size());
        REXLOG_INFO("Native transform cached world checkpoint: {} exact-class world pose/parent basis/version/dirty payloads, definition/parent/source/callback and ordered child-list identities fenced",ready->transforms.size());
        size_t pose_bones=0,pose_bytes=0;for(const auto& p:ready->animation_poses){pose_bones+=p.bones;for(const auto& a:p.arrays)pose_bytes+=a.bytes.size();}
        REXLOG_INFO("Native skeletal pose checkpoint: {} exact-class animation components, {} bones, {} local/model/validity/float bytes; skeleton and all array ownership fenced; behavior graphs not captured",ready->animation_poses.size(),pose_bones,pose_bytes);
        size_t outputs=0,output_bytes=0;for(const auto& p:ready->animation_poses){outputs+=p.output!=0;output_bytes+=p.previous_output.size();}REXLOG_INFO("Native previous behavior generator output checkpoint: {} flat buffers, {} bytes, character/output allocation identities and track extents fenced",outputs,output_bytes);
        REXLOG_INFO("Native basic motion runtime checkpoint: {} audited basic/player controllers, vectors/height/history averages and ring entries/indices captured; dependency and ring allocation identities fenced",ready->motions.size());
        Need(!ready->fields.empty(),"No audited native POD fields were captured");
        Need(ReadNativeEntityOwnership(base,current,why,where) && current==ownership,"Native component ownership changed during capture");
        impl_=std::move(ready);error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
bool NativeComponentFieldCheckpoint::Restore(uint8_t* base,uint64_t epoch,std::string& error)const{
    Host host;GuestReadBatch reads;
    try{
        const auto& s=*impl_;CheckState(base,s,epoch);
        // All ownership/type/extent checks finish before changing any field.
        // No setters run: they may fire gameplay events or rebuild other state.
        for(const auto& f:s.fields)std::memcpy(base+f.Address(),f.bytes.data(),f.size);
        for(const auto& p:s.transforms)WriteTransform(base,p);
        for(const auto& p:s.hovers)WriteHover(base,p);
        for(const auto& p:s.splines)PPC_STORE_U32(p.object+128,p.word);
        for(const auto& p:s.motions)WriteMotion(base,p);
        for(const auto& p:s.animation_poses)WriteAnimationPose(base,p);
        error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
bool NativeComponentFieldCheckpoint::TestRoundTrip(uint8_t* base,uint64_t epoch,std::string& error,const std::function<bool(std::string&)>& coordinated_restore)const{
    Host host;GuestReadBatch reads;
    try{
        const auto& s=*impl_;Need(epoch==s.epoch && s.thread==GetCurrentThreadId(),"Component field test has another owner epoch");
        // Mutate health, a padded matrix word and an existing POD box before simulation resumes.
        // Even a failed assertion restores the original bytes on scope exit.
        const Field* health=nullptr;const Field* matrix=nullptr;const Field* boxed=nullptr;
        for(const auto& f:s.fields){if(f.box && !boxed)boxed=&f;if(!health && f.component=="HealthComponent" && f.name=="Health" && f.type=="Integer")health=&f;
            if(!matrix && f.component=="JPSTransformationComponent" && f.type=="Matrix3" && f.size==48)matrix=&f;}
        Need(health && matrix && boxed,"Live health, padded matrix and cached POD box were not covered by native descriptors");CheckField(base,*health);CheckField(base,*matrix);
        for(const auto& f:s.fields)Need(!std::memcmp(base+f.Address(),f.bytes.data(),f.size),"Native reflected fields changed between capture and immediate probe");
        const auto address=health->Address(),matrix_word=matrix->Address()+40;std::array<uint8_t,4> original{},original_matrix{};std::memcpy(original.data(),base+address,4);std::memcpy(original_matrix.data(),base+matrix_word,4);
        struct Undo {uint8_t* base;uint32_t address;std::array<uint8_t,4> bytes;~Undo(){std::memcpy(base+address,bytes.data(),4);}}undo{base,address,original};
        Undo undo_matrix{base,matrix_word,original_matrix};
        std::array<uint8_t,4> box_original{};
        const auto box_address=boxed?boxed->Address():address;
        std::memcpy(box_original.data(),base+box_address,1);
        struct UndoByte{uint8_t* byte;uint8_t value;~UndoByte(){*byte=value;}} undo_box{base+box_address,box_original[0]};
        if(boxed)base[box_address]^=1;
        PPC_STORE_U32(address,PPC_LOAD_U32(address)^1u);PPC_STORE_U32(matrix_word,PPC_LOAD_U32(matrix_word)^1u);
        Need(!Restore(base,epoch+1,error) && std::memcmp(base+address,original.data(),4) && std::memcmp(base+matrix_word,original_matrix.data(),4) && (!boxed || base[box_address]!=box_original[0]),"Wrong component-field epoch did not reject before writes");
        Need(!s.caches.empty(),"Native component cache lifetime guards were not covered");
        {
            const auto& cache=s.caches.front();const auto count_address=cache.object+40;
            std::array<uint8_t,4> original_count{};std::memcpy(original_count.data(),base+count_address,4);
            Undo undo_count{base,count_address,original_count};PPC_STORE_U32(count_address,cache.count^1u);
            Need(!Restore(base,epoch,error) && std::memcmp(base+address,original.data(),4) && std::memcmp(base+matrix_word,original_matrix.data(),4) && (!boxed || base[box_address]!=box_original[0]),
                "Changed property cache did not reject before native payload writes");
        }
        struct UndoHover{uint8_t* base;const std::vector<HoverState>& states;~UndoHover(){for(const auto& p:states)WriteHover(base,p);}}undo_hover{base,s.hovers};
        for(const auto& p:s.hovers){for(unsigned i=0;i<3;++i)PPC_STORE_U32(p.object+140+i*4,p.words[i]^1u);PPC_STORE_U8(p.object+136,p.override_active^1u);
            for(unsigned i=0;i<hover_driver_float_offsets.size();++i)PPC_STORE_U32(p.driver+hover_driver_float_offsets[i],p.driver_floats[i]^1u);
            for(unsigned i=0;i<hover_driver_flag_offsets.size();++i)PPC_STORE_U8(p.driver+hover_driver_flag_offsets[i],p.driver_flags[i]^1u);
            for(unsigned i=0;i<2;++i){const auto& r=p.rings[i];for(unsigned slot=0;slot<r.vectors.size();++slot)base[r.data+slot*16]^=1;
                if(r.capacity){PPC_STORE_U32(p.driver+16+i*20,(r.indices[0]+1)%r.capacity);PPC_STORE_U32(p.driver+20+i*20,(r.indices[1]+1)%r.capacity);PPC_STORE_U32(p.driver+24+i*20,r.indices[2]?0:r.capacity);}}
        }
        if(!s.hovers.empty()){
            const auto address=s.hovers.front().object+132;std::array<uint8_t,4> driver{};std::memcpy(driver.data(),base+address,4);Undo undo_driver{base,address,driver};PPC_STORE_U32(address,PPC_LOAD_U32(address)^4u);
            Need(!Restore(base,epoch,error),"Changed hover driver accepted by checkpoint");for(const auto& p:s.hovers)Need(PPC_LOAD_U32(p.object+140)==(p.words[0]^1u),"Hover driver rejection partially wrote runtime values");
        }
        struct UndoTransforms{uint8_t* base;const std::vector<TransformCache>& transforms;~UndoTransforms(){for(const auto& p:transforms)WriteTransform(base,p);}}undo_transforms{base,s.transforms};
        for(const auto& p:s.transforms){for(unsigned offset:{172u,208u,248u,260u,336u,376u,396u,412u,414u})base[p.object+offset]^=1;}
        if(!s.transforms.empty()){
            const auto address=s.transforms.front().object+316;std::array<uint8_t,4> parent{};std::memcpy(parent.data(),base+address,4);Undo undo_parent{base,address,parent};PPC_STORE_U32(address,PPC_LOAD_U32(address)^4u);
            Need(!Restore(base,epoch,error),"Changed native transform child count accepted by checkpoint");
            for(const auto& p:s.transforms)Need(base[p.object+172]==uint8_t(p.bytes[0]^1),"Transform child-count rejection partially wrote cached pose");
        }
        struct UndoPoses{uint8_t* base;const std::vector<AnimationPose>& poses;~UndoPoses(){for(const auto& p:poses)WriteAnimationPose(base,p);}}undo_poses{base,s.animation_poses};
        for(const auto& p:s.animation_poses)if(!p.previous_output.empty()){const auto offset=size_t(p.tracks+1)*16;Need(offset<p.previous_output.size(),"Native generator output has no track payload for mutation probe");base[p.output_data+offset]^=1;}
        for(const auto& p:s.animation_poses)if(p.pose){base[p.pose+40]^=1;base[p.pose+41]^=1;for(const auto& a:p.arrays)if(!a.bytes.empty()){base[a.header[0]]^=1;base[a.header[0]+a.bytes.size()-1]^=1;}}
        for(const auto& p:s.animation_poses)if(p.pose && p.bones){
            const auto address=p.pose+4;std::array<uint8_t,4> allocation{};std::memcpy(allocation.data(),base+address,4);Undo undo_pose_allocation{base,address,allocation};PPC_STORE_U32(address,PPC_LOAD_U32(address)^16u);
            Need(!Restore(base,epoch,error),"Changed skeletal pose allocation accepted by checkpoint");
            for(const auto& q:s.animation_poses)if(q.pose)Need(base[q.pose+40]==uint8_t(q.valid[0]^1),"Pose allocation rejection partially wrote validity bits");
            break;
        }
        struct UndoMotion{uint8_t* base;const std::vector<MotionState>& motions;~UndoMotion(){for(const auto& p:motions)WriteMotion(base,p);}}undo_motion{base,s.motions};
        for(const auto& p:s.motions)for(unsigned i=0;i<motion_offsets.size();++i)PPC_STORE_U32(p.object+motion_offsets[i],p.words[i]^1u);
        for(const auto& p:s.motions){
            for(unsigned i=0;i<p.history.size();++i){base[p.data+i*64]^=1;base[p.data+i*64+48]^=1;}
            if(p.capacity){PPC_STORE_U32(p.object+488,(p.indices[0]+1)%p.capacity);PPC_STORE_U32(p.object+492,(p.indices[1]+1)%p.capacity);PPC_STORE_U32(p.object+496,p.indices[2]?0:p.capacity);}
        }
        if(!s.motions.empty()){
            const auto address=s.motions.front().object+80;std::array<uint8_t,4> dependency{};std::memcpy(dependency.data(),base+address,4);Undo undo_dependency{base,address,dependency};
            PPC_STORE_U32(address,PPC_LOAD_U32(address)^4u);
            Need(!Restore(base,epoch,error),"Changed motion dependency accepted by checkpoint");
            for(const auto& p:s.motions)for(unsigned i=0;i<motion_offsets.size();++i)Need(PPC_LOAD_U32(p.object+motion_offsets[i])==(p.words[i]^1u),"Motion dependency rejection partially wrote runtime values");
        }
        if(!s.motions.empty()){
            const auto address=s.motions.front().object+480;std::array<uint8_t,4> allocation{};std::memcpy(allocation.data(),base+address,4);Undo undo_allocation{base,address,allocation};
            PPC_STORE_U32(address,PPC_LOAD_U32(address)^16u);
            Need(!Restore(base,epoch,error),"Changed movement history allocation accepted by checkpoint");
            for(const auto& p:s.motions){
                for(unsigned i=0;i<motion_offsets.size();++i)Need(PPC_LOAD_U32(p.object+motion_offsets[i])==(p.words[i]^1u),"History allocation rejection partially wrote controller values");
                for(unsigned i=0;i<p.history.size();++i)Need(base[p.data+i*64]==uint8_t(p.history[i][0]^1),"History allocation rejection partially wrote records");
            }
        }
        Need(coordinated_restore?coordinated_restore(error):Restore(base,epoch,error),"Native reflected/coordinated field restore failed");
        for(const auto& f:s.fields)Need(!std::memcmp(base+f.Address(),f.bytes.data(),f.size),"Native reflected field payload did not restore exactly");
        for(const auto& p:s.motions)for(unsigned i=0;i<motion_offsets.size();++i)Need(PPC_LOAD_U32(p.object+motion_offsets[i])==p.words[i],"Motion runtime scalar roundtrip differs");
        for(const auto& p:s.transforms){std::array<uint8_t,131> now{};ReadTransform(base,p.object,now);Need(now==p.bytes,"Native transform cached world pose roundtrip differs");}
        REXLOG_INFO("Native transform cached world roundtrip passed: {} components, world translation/matrix/scale, parent basis incl byte40, version and dirty flags mutated; all131 payload bytes restored, changed child count rejected before writes",s.transforms.size());
        for(const auto& p:s.hovers)MatchHover(base,p);
        for(const auto& p:s.animation_poses)MatchAnimationPose(base,p);
        REXLOG_INFO("Native skeletal pose roundtrip passed: {} animation components, local/model bone poses, per-bone validity, float tracks and both overall validity bytes mutated/restored; changed pose allocation rejected before writes",s.animation_poses.size());
        REXLOG_INFO("Native hover runtime roundtrip passed: {} components, component scalars/override and owned driver timers/vectors/flags plus both ring entries/indices mutated/restored, changed driver rejected before writes",s.hovers.size());
        size_t slots=0;unsigned players=0;for(const auto& p:s.motions){
            players+=p.primary==0x8210B598;slots+=p.history.size();
            for(unsigned i=0;i<3;++i)Need(PPC_LOAD_U32(p.object+488+i*4)==p.indices[i],"Native movement history indices/count roundtrip differs");
            for(unsigned i=0;i<p.history.size();++i){std::array<uint8_t,40> now{};ReadMotionRecord(base,p.data+i*64,now);Need(now==p.history[i],"Native movement history record roundtrip differs");}
        }
        REXLOG_INFO("Native basic motion runtime roundtrip passed: {} controllers incl {} player controllers, all18 scalar words and {} ring slots/indices mutated/restored, changed dependency and ring allocation rejected before writes; only audited vector/magnitude bytes copied, ring allocation never overwritten",s.motions.size(),players,slots);
        REXLOG_INFO("Native reflected-field roundtrip passed: {} fields/{} payload bytes across actual native components, mutated Health and Matrix3 byte-40 word restored, all 48 matrix bytes checked, existing POD box mutated/restored, wrong epoch and changed cache rejected before writes, no setters/events executed; {} unsupported declarations, not complete entity state",fields(),bytes(),unsupported());error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
}
