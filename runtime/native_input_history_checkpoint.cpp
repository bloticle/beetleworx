#include "native_input_history_checkpoint.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <vector>
#include <array>
#include <utility>
#include <set>
#include <cstring>
#include <stdexcept>
#include <sstream>
PPC_EXTERN_FUNC(__imp__sub_830A7FD0);
PPC_EXTERN_FUNC(__imp__sub_830A8030);
PPC_EXTERN_FUNC(__imp__sub_830A8200);
PPC_EXTERN_FUNC(__imp__sub_830A8B58);
namespace em2 {namespace {
struct Host {DWORD error=GetLastError();unsigned csr=_mm_getcsr();~Host(){SetLastError(error);_mm_setcsr(csr);}};
void Need(bool value,const char* error){if(!value)throw std::runtime_error(error);}
bool Readable(uint8_t* base,uint32_t at,size_t size){return base && at>=4096 && size && uint64_t(at)+size<=0x100000000ull && GuestReadBatch::Readable(base+at,size);}
struct Block {uint32_t at=0;std::vector<uint8_t> bytes;bool operator==(const Block&)const=default;};
struct Graph {
    std::vector<Block> guards,values;std::vector<uint32_t> device_objects;unsigned devices=0,buttons=0,axes=0,actions=0,bindings=0,unsupported=0;size_t bytes=0;
    Block& Add(uint8_t* base,std::vector<Block>& list,uint32_t at,size_t count){
        Need(Readable(base,at,count),"Native input history extent unavailable");bytes+=count;Need(bytes<=8u*1024*1024 && list.size()<65536,"Native input history snapshot bound exceeded");
        list.push_back({at,std::vector<uint8_t>(base+at,base+at+count)});return list.back();
    }
    void Guard(uint8_t* base,uint32_t at,size_t count){Add(base,guards,at,count);}
    void Value(uint8_t* base,uint32_t at,size_t count){Add(base,values,at,count);}
};
Graph Read(uint8_t* base){
    Graph g;g.Guard(base,0x833DBAD8,4);const auto manager=PPC_LOAD_U32(0x833DBAD8);
    Need(Readable(base,manager,76),"Native action manager unavailable");g.Guard(base,manager,20);g.Guard(base,manager+72,4);
    const auto input=PPC_LOAD_U32(manager+72);g.Guard(base,input,4);g.Guard(base,input+72,16);
    std::set<uint32_t> devices,actions,contexts;
    for(unsigned user=0;user<4;++user){const auto device=PPC_LOAD_U32(input+72+user*4);if(!device || !devices.insert(device).second)continue;
        Need(Readable(base,device,96) && PPC_LOAD_U32(device)==0x821DE6E8,"Native input device class requires a different history adapter");
        auto& shape=g.Add(base,g.guards,device,96);
        for(auto range:{std::pair{20u,4u},std::pair{40u,8u},std::pair{76u,4u}}){std::memset(shape.bytes.data()+range.first,0,range.second);g.Value(base,device+range.first,range.second);}
        const auto buttons=PPC_LOAD_U32(device+48),button_data=PPC_LOAD_U32(device+52),axes=PPC_LOAD_U32(device+56),axis_data=PPC_LOAD_U32(device+60);
        Need(buttons<=32 && axes<=32,"Native input device history count exceeds audited bounds");
        for(unsigned i=0;i<buttons;++i){const auto at=button_data+i*20;g.Value(base,at,4);g.Guard(base,at+4,4);g.Value(base,at+8,12);}
        for(unsigned i=0;i<axes;++i){g.Value(base,axis_data+i*32,16);g.Guard(base,axis_data+i*32+16,16);}
        g.device_objects.push_back(device);++g.devices;g.buttons+=buttons;g.axes+=axes;
    }
    const auto count=PPC_LOAD_U32(manager+8),list=PPC_LOAD_U32(manager+4);Need(count<=128,"Native action context count exceeds bound");
    if(count)g.Guard(base,list,size_t(count)*4);
    contexts.insert(manager+24);for(unsigned i=0;i<count;++i)contexts.insert(PPC_LOAD_U32(list+i*4));
    for(const auto context:contexts){g.Guard(base,context,24);
        const auto listeners=PPC_LOAD_U32(context+8),listener_data=PPC_LOAD_U32(context+4),records=PPC_LOAD_U32(context+20),record_data=PPC_LOAD_U32(context+16);
        Need(listeners<=128 && records<=256,"Native input context lists exceed audited bounds");
        if(listeners)g.Guard(base,listener_data,size_t(listeners)*4);
        if(records)g.Guard(base,record_data,size_t(records)*4);
        for(unsigned i=0;i<records;++i){const auto action=PPC_LOAD_U32(record_data+i*4);if(!actions.insert(action).second)continue;
            Need(actions.size()<=4096,"Native input action snapshot bound exceeded");auto& shape=g.Add(base,g.guards,action,436);
            const auto count=PPC_LOAD_U32(action+408),kind=PPC_LOAD_U32(action+80);Need(count<=10,"Native action binding count exceeds record storage");
            if(kind>4){++g.unsupported;continue;}
            for(unsigned j=0;j<count;++j){const auto offset=88+j*32;
                // Original 827C5F30 mutates binding+17 and the QPC timer +24.
                shape.bytes[offset+17]=0;std::memset(shape.bytes.data()+offset+24,0,8);
                g.Value(base,action+offset+17,1);g.Value(base,action+offset+24,8);++g.bindings;
            }++g.actions;
        }
    }
    Need(g.devices && g.bindings,"Native input history has no audited devices/action bindings");return g;
}
void Layout(const Graph& saved,const Graph& current){
    if(saved.guards!=current.guards){
        size_t i=0;while(i<saved.guards.size() && i<current.guards.size() && saved.guards[i]==current.guards[i])++i;
        std::ostringstream why;why<<"Native input definition/device/context changed before writes (guard "<<i;
        if(i<saved.guards.size())why<<", address 0x"<<std::hex<<saved.guards[i].at;
        why<<")";throw std::runtime_error(why.str());
    }
    Need(saved.values.size()==current.values.size(),"Native input mutable record inventory changed");
    for(size_t i=0;i<saved.values.size();++i)Need(saved.values[i].at==current.values[i].at && saved.values[i].bytes.size()==current.values[i].bytes.size(),"Native input mutable record owner changed");
}
}
struct NativeInputHistoryCheckpoint::Impl {uint8_t* base=nullptr;uint32_t thread=0;uint64_t epoch=0;Graph graph;
    void Scope(uint8_t* memory,uint64_t expected)const{Need(epoch && expected==epoch && memory==base && thread==GetCurrentThreadId(),"Native input history world/thread/memory epoch mismatch");}
};
NativeInputHistoryCheckpoint::NativeInputHistoryCheckpoint():impl_(std::make_unique<Impl>()){}
NativeInputHistoryCheckpoint::~NativeInputHistoryCheckpoint()=default;
bool NativeInputHistoryCheckpoint::Capture(uint8_t* base,uint64_t epoch,std::string& error){Host host;GuestReadBatch reads;try{
    Need(epoch && !impl_->epoch,"Native input history requires an empty checkpoint/nonzero epoch");auto graph=Read(base);const auto again=Read(base);Layout(graph,again);
    Need(graph.values==again.values,"Native input history changed during capture");auto& s=*impl_;s.graph=std::move(graph);s.base=base;s.thread=GetCurrentThreadId();s.epoch=epoch;
    error.clear();return true;
}catch(const std::exception& e){error=e.what();return false;}}
bool NativeInputHistoryCheckpoint::Restore(uint8_t* base,uint64_t epoch,std::string& error)const{Host host;GuestReadBatch reads;try{
    const auto& s=*impl_;s.Scope(base,epoch);const auto current=Read(base);Layout(s.graph,current);
    for(const auto& value:s.graph.values)std::memcpy(base+value.at,value.bytes.data(),value.bytes.size());error.clear();return true;
}catch(const std::exception& e){error=e.what();return false;}}
bool NativeInputHistoryCheckpoint::Matches(uint8_t* base,uint64_t epoch,std::string& error)const{Host host;GuestReadBatch reads;try{
    const auto& s=*impl_;s.Scope(base,epoch);const auto current=Read(base);Layout(s.graph,current);Need(s.graph.values==current.values,"Native input button/axis/action history differs from checkpoint");error.clear();return true;
}catch(const std::exception& e){error=e.what();return false;}}
bool NativeInputHistoryCheckpoint::TestButtonEdges(PPCContext& ctx,uint8_t* base,uint64_t epoch,std::string& error)const{Host host;GuestReadBatch reads;
    const auto saved=ctx;struct RestoreContext{PPCContext& ctx;PPCContext saved;~RestoreContext(){ctx=saved;}}restore_context{ctx,saved};
    try{
        Need(Matches(base,epoch,error),"Native edge replay probe did not begin at captured history");const auto& g=impl_->graph;
        uint32_t device=0;for(auto at:g.device_objects)if(PPC_LOAD_U32(at+48)){device=at;break;}
        Need(device && ctx.r1.u32>=4096+16 && Readable(base,ctx.r1.u32-16,8),"Native edge replay probe has no button device/owner scratch");
        const auto scratch=ctx.r1.u32-16,record=PPC_LOAD_U32(device+52);
        struct Undo{uint8_t* base;const Graph& g;uint32_t scratch;std::array<uint8_t,8> bytes;~Undo(){for(const auto& v:g.values)std::memcpy(base+v.at,v.bytes.data(),v.bytes.size());std::memcpy(base+scratch,bytes.data(),8);}}undo{base,g,scratch,{}};
        std::memcpy(undo.bytes.data(),base+scratch,8);
        std::array<std::array<uint32_t,12>,2> outputs{};
        for(unsigned pass=0;pass<2;++pass){
            // Button zero has an explicit rising edge. No dispatcher, actor,
            // device poll or action callback runs while private history differs.
            PPC_STORE_U8(record+1,1);PPC_STORE_U8(record+3,0);PPC_STORE_U32(record+8,1);PPC_STORE_U32(record+12,0);
            for(unsigned stage=0;stage<3;++stage){
                if(stage==1){ctx=saved;ctx.r3.u64=device;__imp__sub_830A8200(ctx,base);}
                if(stage==2){ctx=saved;ctx.r3.u64=device;ctx.r4.u64=0;__imp__sub_830A8B58(ctx,base);}
                for(unsigned release=0;release<2;++release){ctx=saved;ctx.r3.u64=device;ctx.r4.u64=0;ctx.r5.u64=scratch;PPC_STORE_U32(scratch,0);
                    if(release)__imp__sub_830A8030(ctx,base);else __imp__sub_830A7FD0(ctx,base);
                    outputs[pass][stage*4+release*2]=ctx.r3.u32;outputs[pass][stage*4+release*2+1]=PPC_LOAD_U32(scratch);
                }
            }
            Need(Restore(base,epoch,error) && Matches(base,epoch,error),"Original input maintenance/release did not restore all native history");
        }
        const std::array<uint32_t,12> expected{1,1,0,0,0,1,0,1,0,0,1,1};
        Need(outputs[0]==expected && outputs[1]==expected,"Original native pressed/held/released outputs differ after restoration");
        REXLOG_INFO("Native input edge replay passed: original pressed/released queries, maintenance and release reproduce press once / held without a new press / release once, including native masks; two branches from restored history, no gameplay callbacks");
        error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
bool NativeInputHistoryCheckpoint::TestRoundTrip(uint8_t* base,uint64_t epoch,std::string& error)const{Host host;GuestReadBatch reads;try{
    Need(Matches(base,epoch,error),"Native input probe did not begin at its captured boundary");const auto& g=impl_->graph;
    struct Undo{uint8_t* base;const Graph& g;~Undo(){for(const auto& v:g.values)std::memcpy(base+v.at,v.bytes.data(),v.bytes.size());}}undo{base,g};
    // No interpreter or device poll runs while these private mutations exist.
    for(const auto& value:g.values)base[value.at]^=1;
    Need(!Restore(base,epoch+1,error),"Native input wrong epoch was accepted");
    for(const auto& v:g.values)Need(base[v.at]==uint8_t(v.bytes[0]^1),"Native input wrong epoch wrote partial history");
    Need(!g.guards.empty(),"Native input ownership guard was not covered");
    const auto at=g.guards.front().at;const auto original=base[at];
    {struct RestoreByte{uint8_t& at;uint8_t value;~RestoreByte(){at=value;}}restore{base[at],original};base[at]^=1;
        Need(!Restore(base,epoch,error),"Native input changed owner was accepted");
        for(const auto& v:g.values)Need(base[v.at]==uint8_t(v.bytes[0]^1),"Native input changed owner caused partial history writes");}
    Need(Restore(base,epoch,error) && Matches(base,epoch,error),"Native input history restore did not reproduce all captured bytes");
    REXLOG_INFO("Native input history roundtrip passed: {} devices, {} button records, {} axis records, {} actions/{} binding latches and timers; all mutable ranges changed/restored, wrong epoch and changed manager rejected before writes, {} unsupported action kinds; no input dispatched",g.devices,g.buttons,g.axes,g.actions,g.bindings,g.unsupported);
    error.clear();return true;
}catch(const std::exception& e){error=e.what();return false;}}
}
