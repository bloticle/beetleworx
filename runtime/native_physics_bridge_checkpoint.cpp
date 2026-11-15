#include "native_physics_bridge_checkpoint.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <array>
#include <vector>
#include <set>
#include <cstring>
#include <stdexcept>
namespace em2 {namespace {
struct Host{DWORD error=GetLastError();unsigned csr=_mm_getcsr();~Host(){SetLastError(error);_mm_setcsr(csr);}};
void Need(bool value,const char* why){if(!value)throw std::runtime_error(why);}
bool Readable(uint8_t* base,uint32_t at,size_t size){return base && at>=4096 && size && uint64_t(at)+size<=0x100000000ull && GuestReadBatch::Readable(base+at,size);}
struct Block{uint32_t at=0;std::vector<uint8_t> bytes;bool operator==(const Block&)const=default;};
struct View{std::vector<Block> guards,values;size_t bodies=0,data=0;
    void Add(uint8_t* base,std::vector<Block>& list,uint32_t at,size_t size){Need(Readable(base,at,size),"EM2 physics bridge extent unavailable");list.push_back({at,{base+at,base+at+size}});}
};
View Read(uint8_t* base,const NativeEntityOwnership& expected){
    NativeEntityOwnership now;const char* why=nullptr;uint32_t where=0;
    Need(ReadNativeEntityOwnership(base,now,why,where) && now==expected,"EM2 physics bridge entity/world lifetime changed");
    View view;const auto manager=expected.world;Need(Readable(base,manager,84),"EM2 physics bridge manager unavailable");
    view.Add(base,view.guards,manager,8);view.Add(base,view.guards,manager+24,24);
    const auto world=PPC_LOAD_U32(manager+4);std::set<uint32_t> bodies,data,entities;
    for(const auto& entity:expected.entities)entities.insert(entity.entity);
    for(unsigned offset:{24u,36u}){
        const auto list=PPC_LOAD_U32(manager+offset),count=PPC_LOAD_U32(manager+offset+4),capacity=PPC_LOAD_U32(manager+offset+8)&0x3FFFFFFF;
        Need(count<=capacity && count<=65536,"EM2 physics bridge body list exceeds bound");
        if(count)view.Add(base,view.guards,list,size_t(count)*4);
        for(unsigned i=0;i<count;++i){const auto body=PPC_LOAD_U32(list+i*4);if(!bodies.insert(body).second)continue;
            Need(Readable(base,body,544) && PPC_LOAD_U32(body)==0x82030384 && PPC_LOAD_U32(body+8)==world,"EM2 physics bridge body owner/class changed");
            view.Add(base,view.guards,body,4);view.Add(base,view.guards,body+8,8);++view.bodies;
            const auto userdata=PPC_LOAD_U32(body+12);if(!userdata || !data.insert(userdata).second)continue;
            Need(data.size()<=65536 && Readable(base,userdata,112),"EM2 physics bridge userdata unavailable");
            const auto entity=PPC_LOAD_U32(userdata);Need(!entity || entities.contains(entity),"EM2 physics userdata owner is outside retained entity inventory");
            // Original 827D2C50 returns body+12. 827ADCC8 reads flags +12..16
            // and scalar +20, writes vectors +48/+64/+80/+96. Do not flatten
            // the pointer/array ownership between +24 and +47.
            view.Add(base,view.guards,userdata,12);view.Add(base,view.guards,userdata+24,24);
            view.Add(base,view.values,userdata+12,12);view.Add(base,view.values,userdata+48,64);++view.data;
        }
    }
    Need(view.data>0,"No audited EM2 physics bridge userdata captured");return view;
}
void Layout(const View& expected,const View& now){
    Need(expected.guards==now.guards,"EM2 physics body/userdata/array ownership changed; scalar restore rejected before writes");
    Need(expected.values.size()==now.values.size(),"EM2 physics userdata scalar inventory changed");
    for(size_t i=0;i<expected.values.size();++i)Need(expected.values[i].at==now.values[i].at && expected.values[i].bytes.size()==now.values[i].bytes.size(),"EM2 physics userdata scalar extent changed");
}
}
struct NativePhysicsBridgeCheckpoint::Impl {NativeEntityOwnership ownership;View view;uint64_t epoch=0;DWORD thread=0;uint8_t* base=nullptr;
    void Scope(uint8_t* memory,uint64_t expected)const{Need(epoch && epoch==expected && memory==base && thread==GetCurrentThreadId(),"EM2 physics bridge world/thread/memory epoch mismatch");}
};
NativePhysicsBridgeCheckpoint::NativePhysicsBridgeCheckpoint():impl_(std::make_unique<Impl>()){}
NativePhysicsBridgeCheckpoint::~NativePhysicsBridgeCheckpoint()=default;
bool NativePhysicsBridgeCheckpoint::Capture(uint8_t* base,const NativeEntityOwnership& ownership,uint64_t epoch,std::string& error){Host host;GuestReadBatch reads;try{
    Need(!impl_->epoch && epoch,"EM2 physics bridge requires an empty checkpoint/nonzero epoch");auto view=Read(base,ownership);auto again=Read(base,ownership);Layout(view,again);Need(view.values==again.values,"EM2 physics bridge changed during capture");
    auto& s=*impl_;s.view=std::move(view);s.ownership=ownership;s.epoch=epoch;s.base=base;s.thread=GetCurrentThreadId();error.clear();return true;
}catch(const std::exception& e){error=e.what();return false;}}
bool NativePhysicsBridgeCheckpoint::Restore(uint8_t* base,uint64_t epoch,std::string& error)const{Host host;GuestReadBatch reads;try{
    const auto& s=*impl_;s.Scope(base,epoch);const auto current=Read(base,s.ownership);Layout(s.view,current);
    for(const auto& v:s.view.values)std::memcpy(base+v.at,v.bytes.data(),v.bytes.size());error.clear();return true;
}catch(const std::exception& e){error=e.what();return false;}}
bool NativePhysicsBridgeCheckpoint::Matches(uint8_t* base,uint64_t epoch,std::string& error)const{Host host;GuestReadBatch reads;try{
    const auto& s=*impl_;s.Scope(base,epoch);const auto current=Read(base,s.ownership);Layout(s.view,current);Need(s.view.values==current.values,"EM2 per-body flags/velocity/last-position caches differ from checkpoint");error.clear();return true;
}catch(const std::exception& e){error=e.what();return false;}}
bool NativePhysicsBridgeCheckpoint::TestRoundTrip(uint8_t* base,uint64_t epoch,std::string& error)const{Host host;GuestReadBatch reads;try{
    Need(Matches(base,epoch,error),"EM2 physics bridge probe did not begin at captured state");const auto& view=impl_->view;
    struct Undo{uint8_t* base;const View& v;~Undo(){for(const auto& b:v.values)std::memcpy(base+b.at,b.bytes.data(),b.bytes.size());}}undo{base,view};
    for(const auto& v:view.values)base[v.at]^=1;
    Need(!Restore(base,epoch+1,error),"EM2 physics bridge accepted another epoch");for(const auto& v:view.values)Need(base[v.at]==uint8_t(v.bytes[0]^1),"EM2 physics bridge wrong epoch wrote partial scalar data");
    Need(Restore(base,epoch,error) && Matches(base,epoch,error),"EM2 physics bridge scalar roundtrip differed");
    REXLOG_INFO("Native EM2 physics bridge roundtrip passed: {} manager bodies/{} unique userdata blocks, flags/scalar and four cached vectors changed/restored, wrong epoch rejected; native pointers and owned arrays never overwritten",view.bodies,view.data);
    error.clear();return true;
}catch(const std::exception& e){error=e.what();return false;}}
}
