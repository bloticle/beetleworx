#include "online_button_actions.h"
#include <rex/sha256.h>
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <bit>
#include <charconv>
#include <algorithm>

namespace em2::net {
namespace {
struct Preserve {unsigned fp=_mm_getcsr();DWORD error=GetLastError();~Preserve(){_mm_setcsr(fp);SetLastError(error);}};
uint32_t Word(const NativeTriggerAction& a,unsigned at){uint32_t n=0;for(unsigned i=0;i<4;++i)n=(n<<8)|a.shape[at+i];return n;}
bool ButtonKey(uint32_t key){const auto code=key&65535;return (key&0xffff0000)==0x01400000 && ((code>=1 && code<=9) || (code>=16 && code<=22));}
bool Name(std::string_view name){return !name.empty() && name.size()<96 && name.find('\0')==name.npos;}
bool Bit(const RoomButtonActions& batch,unsigned index){return (batch.fired[index/8]&(1u<<(index%8)))!=0;}
}
std::optional<std::array<uint8_t,32>> HashNativeButtonSchema(const NativeTriggerBinding& b){
    Preserve preserve;
    if((b.scope!=NativeButtonScope::TriggersOnly && b.scope!=NativeButtonScope::DigitalAndTriggers) ||
       b.parent.retail_slot>1 || b.parent.logical_user>1 || !b.threshold || b.threshold>255 ||
       !Name(b.active_context_name) || b.contexts.empty() || b.contexts.size()>2 || b.actions.empty() || b.actions.size()>256)return {};
    std::string bytes="Beetleworx-native-button-schema-v1";
    auto put=[&](uint64_t n,unsigned count){for(unsigned i=count;i;--i)bytes.push_back(char(n>>((i-1)*8)));};
    auto name=[&](std::string_view n){put(n.size(),1);bytes.append(n);};
    put(unsigned(b.scope),1);put(b.parent.retail_slot,1);put(b.parent.logical_user,1);put(b.threshold,4);name(b.active_context_name);
    put(b.contexts.size(),1);
    for(const auto& context:b.contexts){
        if(context.action_order.size()>256)return {};
        put(context.retail_listener?1:0,1);put(context.action_order.size(),2);
    }
    put(b.actions.size(),2);unsigned previous=0;bool first=true;
    for(const auto& a:b.actions){
        const auto index=a.context*256+a.ordinal;
        if(a.context>=b.contexts.size() || a.ordinal>=b.contexts[a.context].action_order.size() || a.ordinal>=256 ||
           (!first && index<=previous) || !Name(a.name) || a.kind>3 || !ButtonKey(a.key) ||
           Word(a,80)!=a.kind || Word(a,88)!=b.parent.logical_user || Word(a,92)!=a.key || Word(a,408)!=1 ||
           Word(a,96) || Word(a,100) || a.shape[104] || Word(a,412) || Word(a,432))return {};
        const auto repeat=Word(a,108);
        if((repeat&0x7f800000)==0x7f800000)return {};
        const float interval=std::bit_cast<float>(repeat);if(interval<0 || interval>60)return {};
        first=false;previous=index;
        put(a.context,1);put(a.ordinal,2);name(a.name);put(a.kind,1);put(a.key,4);put(repeat,4);
        // UID addresses, allocator padding, latches, press stamps and timer
        // counters are deliberately absent. Digital response-curve floats
        // are not consumed by the audited 5F30 path and are absent too.
    }
    const auto hex=rex::util::sha256(bytes);if(hex.size()!=64)return {};
    std::array<uint8_t,32> hash{};
    for(unsigned i=0;i<hash.size();++i){unsigned n=0;const auto parsed=std::from_chars(hex.data()+i*2,hex.data()+i*2+2,n,16);
        if(parsed.ec!=std::errc{} || parsed.ptr!=hex.data()+i*2+2)return {};hash[i]=uint8_t(n);}
    return hash;
}
bool EncodeNativeButtonActions(const NativeTriggerBinding& b,const NativeTriggerBatch& native,RoomButtonActions& out){
    Preserve preserve;const auto schema=HashNativeButtonSchema(b);
    if(!schema || !native.source_epoch || native.count!=b.actions.size())return false;
    RoomButtonActions next;next.schema=*schema;next.source_tick=native.source_tick;
    for(unsigned i=0;i<native.count;++i){const auto& result=native.actions[i];const auto& a=b.actions[i];
        // The digital interpreter has boolean outputs. Reject rather than
        // discard future analog/aim arguments, including signed zero changes.
        if(result.descriptor!=i || std::bit_cast<uint32_t>(result.x) || std::bit_cast<uint32_t>(result.y))return false;
        if(result.dispatch && b.contexts[a.context].retail_listener){const auto index=a.context*256+a.ordinal;
            next.fired[index/8]|=uint8_t(1u<<(index%8));}
    }
    out=next;return true;
}
bool MatchNativeButtonActions(const NativeTriggerBinding& b,const RoomButtonActions& packet,std::vector<NativeActorAction>& out){
    Preserve preserve;const auto schema=HashNativeButtonSchema(b);if(!schema || packet.schema!=*schema)return false;
    std::array<bool,512> known{};std::vector<NativeActorAction> next;
    for(const auto& a:b.actions){const auto index=a.context*256+a.ordinal;
        if(!b.contexts[a.context].retail_listener)continue;
        known[index]=true;if(Bit(packet,index))next.push_back({a.event,a.action,0,0});
    }
    for(unsigned i=0;i<512;++i)if(Bit(packet,i) && !known[i])return false;
    out=std::move(next);return true;
}
bool TestNativeButtonActions(){
    Preserve preserve;unsigned checks=0;auto check=[&](bool okay,const char* why){++checks;if(!okay)REXLOG_ERROR("Native button schema: {}",why);return okay;};
    NativeTriggerBinding b;b.scope=NativeButtonScope::DigitalAndTriggers;b.threshold=32;b.active_context_name="game";b.contexts.resize(2);
    b.contexts[0].action_order.resize(16);b.contexts[1].action_order.resize(256);b.contexts[1].retail_listener=true;
    auto action=[&](unsigned context,unsigned ordinal,const char* label,unsigned kind,uint32_t key){NativeTriggerAction a;
        a.context=context;a.ordinal=ordinal;a.name=label;a.kind=kind;a.key=key;a.event=0x10000+context*4096;a.action=0x20000+ordinal*512;
        auto put=[&](unsigned at,uint32_t n){for(unsigned i=0;i<4;++i)a.shape[at+i]=uint8_t(n>>(24-i*8));};
        put(0,a.action+100);put(80,kind);put(92,key);put(408,1);b.actions.push_back(a);
    };
    action(0,7,"UI_B",2,0x01400010);action(1,3,"TapB",0,0x01400010);
    action(1,4,"HoldB",1,0x01400010);action(1,255,"ReleaseLT",3,0x01400006);
    NativeTriggerBatch native;native.source_epoch=19;native.source_tick=300;native.count=4;
    for(unsigned i=0;i<4;++i)native.actions[i]={uint16_t(i),i!=2,0,0};
    const auto schema=HashNativeButtonSchema(b);RoomButtonActions wire;
    std::string schema_hex;if(schema){constexpr char digits[]="0123456789abcdef";for(auto byte:*schema){schema_hex+=digits[byte>>4];schema_hex+=digits[byte&15];}}
    if(!check(schema_hex=="2e30d0fec5a7743debdbe23ca44dc96159935006bfe048a5d88f612d01397833",
        "schema bytes retain independently packed Python hashlib SHA256 golden identity"))return false;
    if(!check(schema && EncodeNativeButtonActions(b,native,wire) && wire.schema==*schema && wire.source_tick==300 &&
        wire.fired[32]==8 && wire.fired[63]==128 && wire.fired[0]==0,"actual authored order maps to compact context/ordinal bits without authorizing default UI"))return false;
    std::vector<NativeActorAction> descriptors;
    if(!check(MatchNativeButtonActions(b,wire,descriptors) && descriptors.size()==2 && descriptors[0].action==b.actions[1].action &&
        descriptors[1].action==b.actions[3].action,"receiver metadata resolves exactly the fired local descriptors in native order"))return false;
    auto relocated=b;relocated.parent.manager=0x33330000;relocated.parent.device=0x44440000;
    for(auto& c:relocated.contexts){c.event+=0x100000;for(auto& p:c.action_order)p+=0x100000;for(auto& p:c.header)p+=0x100000;}
    for(auto& a:relocated.actions){a.event+=0x100000;a.action+=0x100000;a.uid+=0x100000;a.shape[0]^=255;a.shape[105]=1;a.shape[112]=127;a.shape[416]=123;}
    if(!check(HashNativeButtonSchema(relocated)==schema && MatchNativeButtonActions(relocated,wire,descriptors) &&
        descriptors[0].action==relocated.actions[1].action,"relocation, native UIDs, latches, timers and unused response floats do not change canonical identity"))return false;
    for(unsigned field=0;field<7;++field){auto changed=b;
        if(field==0)changed.active_context_name="vehicle";
        if(field==1)changed.threshold=33;
        if(field==2)changed.actions[1].name="OtherTap";
        if(field==3)changed.actions[1].ordinal=2;
        if(field==4)changed.actions[1].shape[111]=1;
        if(field==5)changed.contexts[0].retail_listener=true;
        if(field==6)changed.parent.retail_slot=1;
        if(!check(HashNativeButtonSchema(changed) && HashNativeButtonSchema(changed)!=schema,"context, threshold, action identity/order/repeat, listener capability and actor slot participate in schema"))return false;
    }
    const auto retained=descriptors;
    for(unsigned invalid=0;invalid<3;++invalid){auto bad=wire;
        if(invalid==0)bad.schema[0]^=1;if(invalid==1)bad.fired[0]|=1;if(invalid==2)bad.fired[0]|=128;
        const bool rejected=!MatchNativeButtonActions(b,bad,descriptors);
        const bool unchanged=descriptors.size()==retained.size() && std::equal(descriptors.begin(),descriptors.end(),retained.begin(),[](const auto& a,const auto& c){
            return a.event==c.event && a.action==c.action && std::bit_cast<uint32_t>(a.x)==std::bit_cast<uint32_t>(c.x) && std::bit_cast<uint32_t>(a.y)==std::bit_cast<uint32_t>(c.y);});
        if(!check(rejected && unchanged,"foreign schema, unknown ordinals and UI bits reject without partial descriptors"))return false;
    }
    const auto stable=wire;
    for(unsigned invalid=0;invalid<5;++invalid){auto bad=native;
        if(invalid==0)bad.count=3;if(invalid==1)bad.source_epoch=0;if(invalid==2)bad.actions[1].descriptor=2;
        if(invalid==3)bad.actions[1].x=1;if(invalid==4)bad.actions[1].y=std::bit_cast<float>(0x80000000u);
        if(!check(!EncodeNativeButtonActions(b,bad,wire) && wire==stable,"partial, unowned, reordered and non-boolean/signed-zero argument batches cannot lose data silently"))return false;
    }
    {auto bad=b;std::swap(bad.actions[1],bad.actions[2]);if(!check(!HashNativeButtonSchema(bad),"unordered native definitions reject"))return false;}
    RoomFrame phase;phase.tick=40;phase.players_mask=3;
    for(unsigned i=0;i<2;++i){phase.players[i].actor_epoch=1;phase.players[i].source=RoomMovementSource::NativeActionV1;}
    if(!check(ClassifyRetailButtonFrame(phase,40,2)==RetailButtonPhase::StartupNeutral,
        "negotiated startup frames remain exact original neutral commands"))return false;
    auto changed=phase;changed.players[1].x=std::bit_cast<float>(0x80000000u);
    if(!check(ClassifyRetailButtonFrame(changed,40,2)==RetailButtonPhase::Invalid,
        "startup seed cannot hide even a signed-zero command substitution"))return false;
    phase.tick=42;
    if(!check(ClassifyRetailButtonFrame(phase,40,2)==RetailButtonPhase::Invalid,
        "legacy neutral cannot delay the native button transition"))return false;
    for(unsigned i=0;i<2;++i){auto& c=phase.players[i];c.source=RoomMovementSource::NativeActionV3;c.jump_button_mask=0x1000;
        c.button_actions=stable;c.button_actions.source_tick=40;}
    if(!check(ClassifyRetailButtonFrame(phase,40,2)==RetailButtonPhase::NativeButtons &&
        ClassifyRetailButtonFrame(phase,42,0)==RetailButtonPhase::Invalid,
        "button commands own their negotiated delayed source ticks, including zero delay"))return false;
    changed=phase;changed.players[1].button_actions.source_tick=41;
    if(!check(ClassifyRetailButtonFrame(changed,40,2)==RetailButtonPhase::Invalid &&
        ClassifyRetailButtonFrame(phase,UINT64_MAX,2)==RetailButtonPhase::Invalid,
        "later-slot source mismatch and cutover overflow reject the whole frame"))return false;
    REXLOG_INFO("Native button schema passed {} canonical relocation/definition/bitmap/UI/transaction checks; metadata only, no native dispatch",checks);return true;
}
}
