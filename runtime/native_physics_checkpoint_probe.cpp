#include "native_physics_checkpoint_probe.h"
#include "native_rigid_body_checkpoint.h"
#include "native_entity_lifetime.h"
#include "native_entity_probe.h"
#include "native_component_schema.h"
#include "native_deferred_entity_checkpoint.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/kernel/kernel_state.h>
#include <rex/logging.h>
#include <rex/cvar.h>
#include <windows.h>
#include <immintrin.h>
#include <array>
#include <vector>
#include <cstring>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <algorithm>
#include <unordered_set>
#include <map>
#include <sstream>
PPC_EXTERN_FUNC(__imp__sub_824D7BC0); // PhysicsData constructor
PPC_EXTERN_FUNC(__imp__sub_824D7A20); // PhysicsData createWorld/register agents
PPC_EXTERN_FUNC(__imp__sub_824D7CB0); // populateFromWorld, optional contacts
PPC_EXTERN_FUNC(__imp__sub_824DFA58); // load saved contact points
PPC_EXTERN_FUNC(__imp__sub_824DF228); // restore one serialized native collision agent
PPC_EXTERN_FUNC(__imp__sub_82E52F30); // read-only native collidable-pair lookup
PPC_EXTERN_FUNC(__imp__sub_8249B5E0); // native remove modifier by atom type
PPC_EXTERN_FUNC(__imp__sub_8249D328); // native disable constraint / ignore atom
PPC_EXTERN_FUNC(__imp__sub_8249CF58); // native soft-contact modifier factory/update
PPC_EXTERN_FUNC(__imp__sub_8249CC40); // native inverse-mass modifier factory/update
PPC_EXTERN_FUNC(__imp__sub_8249B308); // native constraint atom replacement
PPC_EXTERN_FUNC(__imp__sub_8249F470); // native base contact atom allocation
PPC_EXTERN_FUNC(__imp__sub_824522B0); // BoxShape constructor
PPC_EXTERN_FUNC(__imp__sub_8249B068); // RigidBodyCinfo constructor
PPC_EXTERN_FUNC(__imp__sub_82474700); // RigidBody constructor
PPC_EXTERN_FUNC(__imp__sub_8246A600); // world addEntity
PPC_EXTERN_FUNC(__imp__sub_8246A7C8); // world removeEntity, hidden hkBool result
PPC_EXTERN_FUNC(__imp__sub_8246A1C0); // markForWrite
PPC_EXTERN_FUNC(__imp__sub_8246A1F8); // unmarkForWrite
PPC_EXTERN_FUNC(__imp__sub_8246AD50); // original full world step
PPC_EXTERN_FUNC(__imp__sub_82473DF0); // original moved-body broadphase/reset
PPC_EXTERN_FUNC(__imp__sub_82473C90); // original rigid body setMotionType
PPC_EXTERN_FUNC(__imp__sub_82485D78); // AABB phantom setAabb/broadphase update
PPC_EXTERN_FUNC(__imp__sub_82486B48); // shape phantom setTransform/broadphase update
PPC_EXTERN_FUNC(__imp__sub_8238EDD8); // native hkArray reserve, original container allocator
PPC_EXTERN_FUNC(__imp__sub_82486600); // native AABB phantom constructor
PPC_EXTERN_FUNC(__imp__sub_824872A0); // native simple shape phantom constructor
PPC_EXTERN_FUNC(__imp__sub_8246CA90); // native world addPhantom
PPC_EXTERN_FUNC(__imp__sub_82486588); // AABB overlap storage optimization
PPC_EXTERN_FUNC(__imp__sub_82487570); // simple phantom overlap storage optimization
PPC_EXTERN_FUNC(__imp__sub_8238C448); // original removeReference
PPC_EXTERN_FUNC(__imp__sub_8238C398); // original addReference
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DEFINE_BOOL(em2_probe_native_physics_restore,false,"Debug","Private persistence probe only: immediately restore actual level physics component at the same boundary; not frame rollback");

namespace em2 {
namespace {
constexpr unsigned MotionBytes=288;
bool Readable(uint8_t* base,uint32_t at,size_t size){return base && at>=4096 && size && uint64_t(at)+size<=0x100000000ull && GuestReadBatch::Readable(PPC_RAW_ADDR(at),size);}
void Need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void Float(uint8_t* base,uint32_t at,float value){PPC_STORE_U32(at,std::bit_cast<uint32_t>(value));}
struct Calls {
    PPCContext& ctx;PPCContext saved;uint8_t* base;unsigned csr=_mm_getcsr();DWORD error=GetLastError();uint32_t stack=0;
    Calls(PPCContext& c,uint8_t* b):ctx(c),saved(c),base(b){Need(c.r1.u32>=65536 && Readable(base,c.r1.u32-65536,65536),"Native physics probe requires an initialized owner stack");stack=c.r1.u32-32768;}
    void Reset(){ctx=saved;ctx.r1.u64=stack;}
    ~Calls(){ctx=saved;_mm_setcsr(csr);SetLastError(error);}
    uint32_t Allocate(unsigned bytes){
        Reset();Need(Readable(base,ctx.r13.u32,4),"Havok thread router unavailable");const auto tls=PPC_LOAD_U32(ctx.r13.u32);
        Need(Readable(base,tls+8,4),"Havok router slot unavailable");const auto router=PPC_LOAD_U32(tls+8);
        Need(Readable(base,router,48),"Havok router unavailable");const auto heap=PPC_LOAD_U32(router+44);
        Need(Readable(base,heap,4),"Havok heap unavailable");const auto table=PPC_LOAD_U32(heap);
        Need(Readable(base,table,8),"Havok allocation interface unavailable");const auto function=PPC_LOAD_U32(table+4);
        Need(function>=0x822F0000 && function<0x83350000 && !(function&3),"Invalid Havok allocation interface");
        ctx.r3.u64=heap;ctx.r4.u64=bytes;PPC_CALL_INDIRECT_FUNC(function);const auto result=ctx.r3.u32;
        Need(Readable(base,result,bytes),"Havok allocation failed");std::memset(PPC_RAW_ADDR(result),0,bytes);PPC_STORE_U16(result+4,uint16_t(bytes));return result;
    }
};
struct Owners {
    Calls& calls;std::vector<uint32_t> objects;
    void Keep(uint32_t object){objects.push_back(object);}
    ~Owners(){for(auto it=objects.rbegin();it!=objects.rend();++it){calls.Reset();calls.ctx.r3.u64=*it;__imp__sub_8238C448(calls.ctx,calls.base);}}
};
struct FrozenMotion {
    uint32_t body=0,shape=0,stored=0;std::array<uint8_t,320> bytes{};
    std::array<uint8_t,288> stored_bytes{};
};
// Constructor82499030 initializes runtime flag byte37. Constraint insertion/
// removal (82488828/8248F0F0) sets bit0x40; island split82493968 clears its upper
// bits before processing. Native contact reconstruction can leave future flags.
struct FrozenIsland {uint32_t object=0,world=0;uint8_t flags=0,activity=0;std::array<uint32_t,4> constraint_info{};std::array<uint32_t,13> owners{};std::vector<uint32_t> bodies;};
constexpr std::array<unsigned,13> island_owner_offsets{24,60,64,68,76,80,84,96,100,104,116,120,124};
void CheckIsland(uint8_t* base,const FrozenIsland& p,bool solver_layout=true){
    Need(Readable(base,p.object,132) && PPC_LOAD_U32(p.object)==0x8203098C && PPC_LOAD_U32(p.object+24)==p.world,"Native island class/world owner changed");
    Need(PPC_LOAD_U8(p.object+38)==p.activity,"Native island activation changed; full island lifecycle restoration required");
    // We write no array storage. Native reserve/reallocation is allowed when
    // membership and counts still match, as in the add/remove-body fixture.
    for(unsigned i:{0u,2u,8u,11u})Need(PPC_LOAD_U32(p.object+island_owner_offsets[i])==p.owners[i],"Native island owner/action/track counts changed; topology restoration required");
    for(unsigned at:{60u,76u,96u,116u}){const auto data=PPC_LOAD_U32(p.object+at),count=PPC_LOAD_U32(p.object+at+4),capacity=PPC_LOAD_U32(p.object+at+8)&0x3FFFFFFF;
        Need(count<=capacity && capacity<=65536 && (!count || Readable(base,data,size_t(count)*4)),"Native island array/track extent unavailable");}
    if(solver_layout)for(unsigned i=0;i<4;++i)Need(PPC_LOAD_U32(p.object+8+i*4)==p.constraint_info[i],"Native island solver layout differs; contact/constraint reconstruction incomplete");
    const auto data=PPC_LOAD_U32(p.object+76);Need(PPC_LOAD_U32(p.object+80)==p.bodies.size() && (p.bodies.empty() || Readable(base,data,p.bodies.size()*4)),"Native island body membership unavailable");
    for(unsigned i=0;i<p.bodies.size();++i)Need(PPC_LOAD_U32(data+i*4)==p.bodies[i],"Native island ordered body membership changed");
}
FrozenIsland FreezeIsland(uint8_t* base,uint32_t object,uint32_t world){
    Need(Readable(base,object,132),"Native island header unavailable");FrozenIsland p;p.object=object;p.world=world;p.flags=PPC_LOAD_U8(object+37);p.activity=PPC_LOAD_U8(object+38);
    for(unsigned i=0;i<island_owner_offsets.size();++i)p.owners[i]=PPC_LOAD_U32(object+island_owner_offsets[i]);
    for(unsigned i=0;i<4;++i)p.constraint_info[i]=PPC_LOAD_U32(object+8+i*4);
    const auto data=PPC_LOAD_U32(object+76),count=PPC_LOAD_U32(object+80),capacity=PPC_LOAD_U32(object+84)&0x3FFFFFFF;
    Need(count<=capacity && capacity<=65536 && (!count || Readable(base,data,size_t(count)*4)),"Native island body list exceeds checkpoint bounds");
    p.bodies.resize(count);for(unsigned i=0;i<count;++i)p.bodies[i]=PPC_LOAD_U32(data+i*4);CheckIsland(base,p);return p;
}
uint32_t StoredMotion(uint8_t* base,uint32_t body){
    // Original hkpRigidBody::getStoredDynamicMotion (82473DA0): the tail
    // pointer is meaningful only for keyframed/fixed motion, not other classes.
    const auto type=PPC_LOAD_U8(body+232);return type==4 || type==5?PPC_LOAD_U32(body+504):0;
}
FrozenMotion Freeze(uint8_t* base,uint32_t body){
    Need(Readable(base,body,544) && PPC_LOAD_U32(body)==0x82030384,"Native rigid-body layout differs from audited image");
    FrozenMotion state;state.body=body;state.shape=PPC_LOAD_U32(body+16);std::memcpy(state.bytes.data(),PPC_RAW_ADDR(body+224),state.bytes.size());
    state.stored=StoredMotion(base,body);
    if(state.stored){
        // Native clone 82474F70 allocates/copies exactly 288 bytes and preserves
        // the allocated reference header. Never flatten an owned saved motion
        // into a copied pointer or overwrite its reference count on restore.
        Need(Readable(base,state.stored,state.stored_bytes.size()),"Stored dynamic motion is unreadable");
        const auto table=PPC_LOAD_U32(state.stored);const auto type=PPC_LOAD_U8(state.stored+8);
        Need(table>=0x82020000 && table<0x82040000 && type>=1 && type<=7 && type!=4 && type!=5,
            "Stored dynamic motion class is unsupported or recursively owns another saved motion");
        std::memcpy(state.stored_bytes.data(),PPC_RAW_ADDR(state.stored),state.stored_bytes.size());
    }
    return state;
}
uint32_t Word(const FrozenMotion& m,unsigned offset){const auto* p=m.bytes.data()+offset;return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
bool SameKinematics(const FrozenMotion& a,const FrozenMotion& b){
    for(unsigned offset:{16u,20u,24u,32u,36u,40u,48u,52u,56u,64u,68u,72u,208u,212u,216u,224u,228u,232u})if(Word(a,offset)!=Word(b,offset))return false;return true;
}
struct Inventory {
    std::array<std::vector<uint32_t>,4> objects;
    std::vector<uint32_t> contact_systems;unsigned contacts=0;
};
struct AgentLoad {uint32_t source=0,status=0,a=0,b=0,manager=0,old_atom=0,new_atom=0;unsigned calls=0,replacements=0;};
struct ContactLoads {uint32_t collision=0;unsigned unexpected=0;std::vector<AgentLoad> agents;std::map<uint32_t,size_t> indices;std::map<std::pair<uint32_t,uint32_t>,size_t> pairs;AgentLoad* active=nullptr;};
thread_local ContactLoads* contact_loads=nullptr;
struct ContactLoadScope {
    ContactLoads data;
    ContactLoadScope(uint8_t* base,uint32_t world,const Inventory& inventory){
        Need(!contact_loads,"Nested native contact checkpoint restoration is unsupported");data.collision=PPC_LOAD_U32(world+120);
        for(const auto system:inventory.contact_systems){Need(Readable(base,system,80),"Saved contact system unavailable");const auto array=PPC_LOAD_U32(system+68),count=PPC_LOAD_U32(system+72);
            Need(count<=65536 && data.agents.size()+count<=65536 && (!count || Readable(base,array,size_t(count)*4)),"Saved contact agent extent invalid");
            for(unsigned i=0;i<count;++i){const auto source=PPC_LOAD_U32(array+i*4);Need(Readable(base,source,32),"Saved native agent unavailable");
                Need(data.indices.emplace(source,data.agents.size()).second,"Duplicate serialized agent ownership in checkpoint");AgentLoad entry;entry.source=source;entry.a=PPC_LOAD_U32(source+8);entry.b=PPC_LOAD_U32(source+12);const auto pair=std::minmax(entry.a,entry.b);
                Need(data.pairs.emplace(std::pair{pair.first,pair.second},data.agents.size()).second,"Duplicate serialized agent body pair");data.agents.push_back(entry);}
        }
        Need(data.agents.size()==inventory.contacts,"Saved agent count changed before native contact load");contact_loads=&data;
    }
    ~ContactLoadScope(){contact_loads=nullptr;}
    void Verify(){
        unsigned skipped=0,failed=0,duplicate=0;for(const auto& a:data.agents){skipped+=!a.calls;failed+=a.calls && a.status!=0;duplicate+=a.calls>1;}
        REXLOG_INFO("Native contact reconstruction accounting: {} saved agents, {} skipped, {} failed, {} duplicate, {} unexpected calls",data.agents.size(),skipped,failed,duplicate,data.unexpected);
        unsigned logged=0;for(const auto& a:data.agents)if(!a.calls || a.status || a.calls>1){if(++logged>8)break;REXLOG_ERROR("Native serialized agent {:08X}: {} load calls, status {}",a.source,a.calls,a.status);}
        Need(!skipped && !failed && !duplicate && !data.unexpected,"Native contact reconstruction did not restore every serialized agent exactly once");
    }
};
Inventory ReadInventory(uint8_t* base,uint32_t data){
    Inventory result;Need(Readable(base,data,24),"Native physics-data owner unavailable");
    const auto systems=PPC_LOAD_U32(data+12),count=PPC_LOAD_U32(data+16);
    Need(count<=4096 && (!count || Readable(base,systems,size_t(count)*4)),"Native physics-data system list invalid");
    for(unsigned i=0;i<count;++i){const auto system=PPC_LOAD_U32(systems+i*4);Need(Readable(base,system,68),"Invalid retained native physics system");
        for(unsigned kind=0;kind<4;++kind){const auto array=system+8+kind*12,pointer=PPC_LOAD_U32(array),length=PPC_LOAD_U32(array+4);
            Need(length<=32768 && result.objects[kind].size()+length<=65536 && (!length || Readable(base,pointer,size_t(length)*4)),"Native physics inventory exceeds its supported bounds");
            for(unsigned j=0;j<length;++j)result.objects[kind].push_back(PPC_LOAD_U32(pointer+j*4));}
        if(PPC_LOAD_U32(system)==0x82032F20){Need(Readable(base,system,80),"Invalid native saved-contact system");
            const auto contacts=PPC_LOAD_U32(system+72);Need(contacts<=65536 && uint64_t(result.contacts)+contacts<=65536,"Native saved contact limit exceeded");
            result.contacts+=contacts;result.contact_systems.push_back(system);}
    }
    for(auto& entries:result.objects){std::sort(entries.begin(),entries.end());entries.erase(std::unique(entries.begin(),entries.end()),entries.end());}
    return result;
}
// Read-only native save/load comparison. 824DE520 copies properties at80 as
// bytes, contact points at92 with stride32, and ID-map bytes at104. 824DFAF0
// explicitly copies the atom fields below, leaving padding11/14/15 untouched.
struct ModifierView {uint16_t type=0,size=0;std::vector<uint8_t> payload;bool operator==(const ModifierView&)const=default;};
struct SavedAgentView {uint32_t a=0,b=0,modifier_body_a=0;uint8_t type=0;std::array<uint8_t,45> atom{};std::array<std::vector<uint8_t>,3> arrays;
    std::vector<ModifierView> modifiers;uint16_t solver_atom=0;bool terminal_matches=true;
};
using AgentViews=std::map<std::pair<uint32_t,uint32_t>,SavedAgentView>;
std::pair<uint32_t,uint32_t> AgentConstraint(Calls& calls,const SavedAgentView& v){
    auto* base=calls.base;Need(Readable(base,v.a,128) && Readable(base,v.b,128),"Native modifier body pair unavailable");
    calls.Reset();calls.ctx.r3.u64=v.a+16;calls.ctx.r4.u64=v.b+16;__imp__sub_82E52F30(calls.ctx,base);
    const auto agent=calls.ctx.r3.u32;Need(Readable(base,agent,24),"Native modifier pair lookup failed");const auto manager=PPC_LOAD_U32(agent+8);
    Need(Readable(base,manager,128),"Native contact manager unavailable for modifier inspection");const auto table=PPC_LOAD_U32(manager);
    Need(Readable(base,table,64),"Native contact manager methods unavailable");const auto getter=PPC_LOAD_U32(table+60);
    Need(Readable(base,getter,8) && PPC_LOAD_U32(getter)==0x38630044 && PPC_LOAD_U32(getter+4)==0x4E800020,"Native constraint getter differs from audited inline manager layout");
    const auto constraint=manager+68,a=PPC_LOAD_U32(constraint+20),b=PPC_LOAD_U32(constraint+24);
    Need((a==v.a && b==v.b) || (a==v.b && b==v.a),"Native modifier constraint body identity differs");return{manager,constraint};
}
AgentViews ReadAgentViews(uint8_t* base,const Inventory& inventory){
    AgentViews out;size_t bytes=0;
    for(const auto system:inventory.contact_systems){Need(Readable(base,system,80),"Native saved contact system unavailable for comparison");
        const auto array=PPC_LOAD_U32(system+68),count=PPC_LOAD_U32(system+72);Need(count<=65536 && (!count || Readable(base,array,size_t(count)*4)),"Native saved contact comparison extent invalid");
        for(unsigned i=0;i<count;++i){const auto at=PPC_LOAD_U32(array+i*4);Need(Readable(base,at,308) && PPC_LOAD_U32(at)==0x82032F34 && !PPC_LOAD_U8(at+24),"Native saved agent class/identity representation differs from audited local capture");
            SavedAgentView v;v.a=PPC_LOAD_U32(at+8);v.b=PPC_LOAD_U32(at+12);v.type=PPC_LOAD_U8(at+25);
            std::memcpy(v.atom.data(),base+at+32,11);std::memcpy(v.atom.data()+11,base+at+44,2);std::memcpy(v.atom.data()+13,base+at+48,32);
            for(unsigned j=0;j<3;++j){const auto header=at+80+j*12,data=PPC_LOAD_U32(header),length=PPC_LOAD_U32(header+4),capacity=PPC_LOAD_U32(header+8)&0x3FFFFFFF;
                const auto stride=j==1?32u:1u;Need(length<=capacity && length<=65536 && capacity<=1048576,"Native saved agent array extent invalid");
                const auto size=size_t(length)*stride;bytes+=size;Need(bytes<=8u*1024*1024 && (!size || Readable(base,data,size)),"Native saved agent comparison exceeds payload bound");
                v.arrays[j].resize(size);if(size)std::memcpy(v.arrays[j].data(),base+data,size);
            }
            const auto key=std::minmax(v.a,v.b);Need(out.emplace(std::pair{key.first,key.second},std::move(v)).second,"Duplicate native saved-agent pair in comparison");
        }
    }return out;
}
void ReadAgentModifiers(Calls& calls,AgentViews& views){
    auto* base=calls.base;
    for(auto& [pair,v]:views){
        const auto [manager,constraint]=AgentConstraint(calls,v);v.modifier_body_a=PPC_LOAD_U32(constraint+20);
        const auto internal=PPC_LOAD_U32(constraint+48);
        if(internal){Need(Readable(base,internal,20),"Native constraint internal unavailable");const auto atom=PPC_LOAD_U32(internal+12);Need(Readable(base,atom,2),"Native solver atom unavailable");v.solver_atom=PPC_LOAD_U16(atom);}
        auto atom=PPC_LOAD_U32(constraint+16);std::unordered_set<uint32_t> seen;
        while(atom){Need(Readable(base,atom,24) && seen.insert(atom).second && seen.size()<=16,"Native modifier chain unavailable/cyclic/unbounded");
            const auto type=PPC_LOAD_U16(atom);if(type<26){v.terminal_matches=atom==PPC_LOAD_U32(manager+60);break;}
            const auto size=PPC_LOAD_U16(atom+16);Need(type<=64 && size>=32 && size<=512 && Readable(base,atom,size),"Native modifier atom extent invalid");
            ModifierView m;m.type=type;m.size=size;const size_t payload=type==26?8:type==27?32:std::min<unsigned>(32,size-32);
            Need(32+payload<=size,"Native modifier payload exceeds atom");m.payload.assign(base+atom+32,base+atom+32+payload);
            // Native loading may reverse the embedded constraint's body order.
            // Store mass vectors by stable local body-pair order instead.
            if(type==27 && v.modifier_body_a!=pair.first)for(unsigned i=0;i<16;++i)std::swap(m.payload[i],m.payload[i+16]);
            v.modifiers.push_back(std::move(m));atom=PPC_LOAD_U32(atom+20);
        }
    }
}
struct LiveModifierNode {uint32_t at=0,child=0;uint16_t type=0,size=0;};
struct LiveModifierLink {uint32_t manager=0,constraint=0,base_atom=0,root=0;std::vector<LiveModifierNode> nodes;};
using LiveModifierLinks=std::map<std::pair<uint32_t,uint32_t>,LiveModifierLink>;
LiveModifierLinks CaptureModifierLinks(Calls& calls,const AgentViews& views){
    auto* base=calls.base;LiveModifierLinks result;
    for(const auto& [pair,v]:views){const auto [manager,constraint]=AgentConstraint(calls,v);LiveModifierLink link;link.manager=manager;link.constraint=constraint;link.base_atom=PPC_LOAD_U32(manager+60);link.root=PPC_LOAD_U32(constraint+16);
        Need(Readable(base,link.base_atom,48) && PPC_LOAD_U16(link.base_atom)==25,"Native base contact atom unavailable before load");auto at=link.root;std::unordered_set<uint32_t> seen;
        while(at && at!=link.base_atom){Need(Readable(base,at,24) && seen.insert(at).second && seen.size()<=16,"Native live modifier chain unavailable/cyclic/unbounded");
            LiveModifierNode n;n.at=at;n.type=PPC_LOAD_U16(at);n.size=PPC_LOAD_U16(at+16);n.child=PPC_LOAD_U32(at+20);
            Need(n.type>=26 && n.type<=64 && n.size>=32 && n.size<=512 && Readable(base,at,n.size),"Native live modifier node extent invalid");link.nodes.push_back(n);at=n.child;
        }
        Need(!link.root || (!link.nodes.empty() && at==link.base_atom),"Native live modifier chain does not end in owned contact atom");result.emplace(pair,std::move(link));
    }return result;
}
uint32_t ModifierOwner(uint8_t* base,const SavedAgentView& v){
    auto owner=PPC_LOAD_U32(v.a+200);Need(Readable(base,owner,40),"Native modifier first island unavailable");
    if(PPC_LOAD_U16(owner+32)==0xFFFF)owner=PPC_LOAD_U32(v.b+200);
    Need(Readable(base,owner,132) && PPC_LOAD_U32(owner)==0x8203098C && PPC_LOAD_U32(owner+24)==PPC_LOAD_U32(v.a+8),"Native modifier island/world owner invalid");return owner;
}
bool SupportedModifier(const ModifierView& m){return (m.type==26 && m.size==48 && m.payload.size()==8) || (m.type==27 && m.size==64 && m.payload.size()==32) || (m.type==30 && m.size==32 && m.payload.empty());}
void RemoveModifiersBeforeLoad(Calls& calls,const AgentViews& captured,const LiveModifierLinks& links){
    auto* base=calls.base;
    // Removing through the original API while the old base atom is still alive
    // avoids retaining a chain whose child storage the loader will replace.
    for(const auto& [pair,link]:links){const auto& v=captured.at(pair);ModifierOwner(base,v);std::unordered_set<unsigned> types;
        for(const auto& m:v.modifiers)Need(SupportedModifier(m),"Native captured modifier type requires a reconstruction adapter");
        for(const auto& n:link.nodes)Need((n.type==26 || n.type==27 || n.type==30) && types.insert(n.type).second,"Native current modifier topology requires a reconstruction adapter");
    }
    for(const auto& [pair,link]:links){const auto owner=ModifierOwner(base,captured.at(pair));
        for(const auto& n:link.nodes){calls.Reset();calls.ctx.r3.u64=link.constraint;calls.ctx.r4.u64=owner;calls.ctx.r5.u64=n.type;__imp__sub_8249B5E0(calls.ctx,base);}
        Need(!PPC_LOAD_U32(link.constraint+16),"Native modifier removal left an owned chain");
    }
}
void RecreateModifiersAfterLoad(Calls& calls,const AgentViews& captured,const ContactLoads& loads){
    auto* base=calls.base;unsigned rebuilt=0;
    for(const auto& [pair,v]:captured){const auto [manager,constraint]=AgentConstraint(calls,v);const auto owner=ModifierOwner(base,v);
        if(const auto root=PPC_LOAD_U32(constraint+16)){
            // Native constraint-added callbacks may install an ignore atom while
            // the OLD base atom is alive. Observe that exact replacement inside
            // the loader; never identify a freed terminal by reading its memory.
            const auto record=loads.pairs.find(pair);Need(record!=loads.pairs.end(),"Native regenerated modifier has no load record");const auto& a=loads.agents[record->second];const auto atom=PPC_LOAD_U32(manager+60);
            Need(a.manager==manager && a.replacements==1 && a.new_atom==atom && Readable(base,root,32) && PPC_LOAD_U16(root)==30 && PPC_LOAD_U16(root+16)==32 && (PPC_LOAD_U32(root+20)==a.old_atom || PPC_LOAD_U32(root+20)==atom),"Native regenerated modifier topology requires additional reconstruction");
            if(PPC_LOAD_U32(root+20)!=atom){calls.Reset();calls.ctx.r3.u64=constraint;calls.ctx.r4.u64=a.old_atom;calls.ctx.r5.u64=atom;calls.ctx.r6.u64=PPC_LOAD_U16(atom+2);__imp__sub_8249B308(calls.ctx,base);}
            calls.Reset();calls.ctx.r3.u64=constraint;calls.ctx.r4.u64=root;calls.ctx.r5.u64=root;calls.ctx.r6.u64=32;__imp__sub_8249B308(calls.ctx,base);
            calls.Reset();calls.ctx.r3.u64=constraint;calls.ctx.r4.u64=owner;calls.ctx.r5.u64=30;__imp__sub_8249B5E0(calls.ctx,base);
        }
        Need(!PPC_LOAD_U32(constraint+16),"Native modifier root did not clear before recreation");
        for(auto it=v.modifiers.rbegin();it!=v.modifiers.rend();++it){const auto& m=*it;
            if(m.type==30){calls.Reset();calls.ctx.r3.u64=constraint;calls.ctx.r4.u64=owner;__imp__sub_8249D328(calls.ctx,base);}
            else if(m.type==26){auto word=[&](unsigned i){const auto* p=m.payload.data()+i;return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];};
                const auto a=std::bit_cast<float>(word(0)),b=std::bit_cast<float>(word(4));Need(std::isfinite(a) && std::isfinite(b),"Native soft-contact payload is not finite");
                calls.Reset();calls.ctx.r3.u64=manager;calls.ctx.r4.u64=v.a;calls.ctx.r5.u64=v.b;calls.ctx.r6.u64=owner;calls.ctx.f1.f64=a;calls.ctx.f2.f64=b;__imp__sub_8249CF58(calls.ctx,base);
            }else if(m.type==27){const auto body_a=pair.first,body_b=pair.second;const auto scratch=calls.stack+2048;
                // Install a nonzero W scale first so native validation never
                // encounters the transient forbidden all-zero mass pair.
                const bool a_nonzero=(m.payload[12]&0x7F) || m.payload[13] || m.payload[14] || m.payload[15];
                for(unsigned pass=0;pass<2;++pass){const unsigned side=a_nonzero?pass:1-pass;std::memcpy(base+scratch,m.payload.data()+side*16,16);
                    calls.Reset();calls.ctx.r3.u64=manager;calls.ctx.r4.u64=side?body_b:body_a;calls.ctx.r5.u64=owner;calls.ctx.r6.u64=scratch;__imp__sub_8249CC40(calls.ctx,base);}
            }
            ++rebuilt;
        }
    }
    REXLOG_INFO("Native contact modifier reconstruction: {} captured ignore/soft-contact/mass modifiers rebuilt through original factories; old chains removed before native contact atom replacement",rebuilt);
}

void DescribeAgentRestore(const AgentViews& expected,const AgentViews& now){
    unsigned changed=0,missing=0,reordered=0;
    for(const auto& [pair,a]:expected){const auto found=now.find(pair);if(found==now.end()){++missing;continue;}const auto& b=found->second;
        if(a.a!=b.a || a.b!=b.b){++reordered;continue;}
        if(a.type==b.type && a.atom==b.atom && a.arrays==b.arrays && a.modifiers==b.modifiers && a.modifier_body_a==b.modifier_body_a && a.solver_atom==b.solver_atom && a.terminal_matches==b.terminal_matches)continue;
        if(++changed>8)continue;std::ostringstream text;
        auto diff=[&](const char* label,const auto& x,const auto& y){if(x==y)return;size_t i=0;while(i<x.size() && i<y.size() && x[i]==y[i])++i;text<<" "<<label<<" sizes "<<x.size()<<"/"<<y.size()<<" first "<<i;if(i<x.size() && i<y.size())text<<" value "<<unsigned(x[i])<<"/"<<unsigned(y[i]);};
        diff("atom",a.atom,b.atom);for(unsigned j=0;j<3;++j)diff(j==0?"properties":j==1?"points":"ID-map",a.arrays[j],b.arrays[j]);
        text<<" modifiers";for(const auto& m:a.modifiers)text<<" "<<m.type<<":"<<m.size;text<<" /";for(const auto& m:b.modifiers)text<<" "<<m.type<<":"<<m.size;
        text<<" solver-atom "<<a.solver_atom<<"/"<<b.solver_atom<<" terminal-matches "<<a.terminal_matches<<"/"<<b.terminal_matches;
        REXLOG_INFO("Native contact restore pair {:08X}/{:08X} type {}/{} differs:{}",pair.first,pair.second,a.type,b.type,text.str());
    }
    REXLOG_INFO("Native contact saved/recaptured comparison: {} expected/{} current pairs, {} changed, {} missing, {} reversed; diagnostic of audited fields/arrays, not full native agent equivalence",expected.size(),now.size(),changed,missing,reordered);
}
uint32_t CaptureData(Calls& calls,uint32_t world,bool contacts){
    const auto data=calls.Allocate(24);calls.Reset();calls.ctx.r3.u64=data;__imp__sub_824D7BC0(calls.ctx,calls.base);
    try{calls.Reset();calls.ctx.r3.u64=data;calls.ctx.r4.u64=world;calls.ctx.r5.u64=contacts?1:0;__imp__sub_824D7CB0(calls.ctx,calls.base);return data;}
    catch(...){calls.Reset();calls.ctx.r3.u64=data;__imp__sub_8238C448(calls.ctx,calls.base);throw;}
}
struct FrozenPhantom {
    uint32_t object=0,table=0,world=0,shape=0,userdata=0,filter=0;
    unsigned array=0,pose_size=0;uint8_t order_dirty=0;
    std::array<uint8_t,32> listeners{};
    std::array<uint8_t,176> pose{};
    std::array<uint8_t,16> derived_owner{};
    std::array<std::vector<uint32_t>,2> listener_entries;
    std::array<uint32_t,2> listener_pointers{};
    std::vector<uint32_t> overlaps;
};
uint32_t CollidableOwner(uint8_t* base,uint32_t collidable,uint32_t world){
    Need(Readable(base,collidable,32),"Phantom overlap collidable is unreadable");
    const auto owner=uint32_t(int64_t(collidable)+int8_t(PPC_LOAD_U8(collidable+16)));
    Need(Readable(base,owner,128) && uint64_t(owner)+16==collidable && PPC_LOAD_U32(owner+8)==world,
        "Phantom overlap no longer belongs to the captured world");
    const auto table=PPC_LOAD_U32(owner);
    Need(table==0x82030384 || table==0x8202E7DC || table==0x8202E86C || table==0x82093948,
        "Phantom overlap owner class requires another lifetime adapter");
    return owner;
}
FrozenPhantom FreezePhantom(uint8_t* base,uint32_t object,uint32_t world){
    Need(Readable(base,object,160) && PPC_LOAD_U32(object+8)==world,"Phantom world ownership changed");
    FrozenPhantom p;p.object=object;p.world=world;p.table=PPC_LOAD_U32(object);
    // Audited native AABB, simple shape and the game's AABB trigger subclass.
    if(p.table==0x8202E7DC || p.table==0x82093948){p.array=192;p.pose_size=32;}
    else if(p.table==0x8202E86C){p.array=336;p.pose_size=176;}
    else Need(false,"Native phantom class is unsupported");
    Need(Readable(base,object,p.array+16) && (p.table!=0x82093948 || Readable(base,object,224)),"Native phantom layout unavailable");
    p.shape=PPC_LOAD_U32(object+16);p.userdata=PPC_LOAD_U32(object+12);p.filter=PPC_LOAD_U32(object+44);
    std::memcpy(p.listeners.data(),base+object+128,32);std::memcpy(p.pose.data(),base+object+160,p.pose_size);
    for(unsigned kind=0;kind<2;++kind){const auto at=object+136+kind*12,ptr=PPC_LOAD_U32(at),count=PPC_LOAD_U32(at+4),capacity=PPC_LOAD_U32(at+8)&0x3FFFFFFF;
        Need(count<=1024 && count<=capacity && (!count || Readable(base,ptr,size_t(count)*4)),"Phantom listener array is invalid");
        p.listener_pointers[kind]=ptr;for(unsigned i=0;i<count;++i)p.listener_entries[kind].push_back(PPC_LOAD_U32(ptr+i*4));
    }
    if(p.table==0x82093948)std::memcpy(p.derived_owner.data(),base+object+208,16);
    const auto pointer=PPC_LOAD_U32(object+p.array),count=PPC_LOAD_U32(object+p.array+4),capacity=PPC_LOAD_U32(object+p.array+8)&0x3FFFFFFF;
    Need(count<=capacity && capacity<=65536 && (!count || Readable(base,pointer,size_t(count)*4)),"Phantom overlap array is outside bounds");
    p.overlaps.reserve(count);for(unsigned i=0;i<count;++i){const auto collidable=PPC_LOAD_U32(pointer+i*4);CollidableOwner(base,collidable,world);p.overlaps.push_back(collidable);}
    p.order_dirty=PPC_LOAD_U8(object+p.array+12);return p;
}
void CheckPhantomIdentity(uint8_t* base,const FrozenPhantom& p){
    const auto current=FreezePhantom(base,p.object,p.world);
    Need(current.table==p.table && current.shape==p.shape && current.userdata==p.userdata && current.filter==p.filter &&
        current.listeners==p.listeners && current.listener_entries==p.listener_entries && current.derived_owner==p.derived_owner,"Native phantom class/shape/listener/engine-owner lifetime changed");
    for(const auto collidable:p.overlaps)CollidableOwner(base,collidable,p.world);
}
// Suppress overlap/lifecycle listeners and the audited game trigger owner only
// during reconstruction. Restore their exact live ownership even on failure.
// Overlap arrays are installed afterward without issuing new enter/exit events.
struct MutedPhantomCallbacks {
    uint8_t* base;const std::vector<FrozenPhantom>& phantoms;
    MutedPhantomCallbacks(uint8_t* b,const std::vector<FrozenPhantom>& p):base(b),phantoms(p){
        for(const auto& x:phantoms){PPC_STORE_U32(x.object+140,0);PPC_STORE_U32(x.object+152,0);if(x.table==0x82093948)PPC_STORE_U32(x.object+212,0);}
    }
    bool FixedOwnership(const FrozenPhantom& x)const{
        for(unsigned offset=0;offset<32;offset+=4)if(offset!=12 && offset!=24 &&
            std::memcmp(base+x.object+128+offset,x.listeners.data()+offset,4))return false;
        for(unsigned kind=0;kind<2;++kind){
            if(PPC_LOAD_U32(x.object+140+kind*12))return false;
            const auto& entries=x.listener_entries[kind];const auto pointer=x.listener_pointers[kind];
            if(!entries.empty() && !Readable(base,pointer,entries.size()*4))return false;
            for(unsigned i=0;i<entries.size();++i)if(PPC_LOAD_U32(pointer+i*4)!=entries[i])return false;
        }
        if(x.table==0x82093948){
            if(PPC_LOAD_U32(x.object+212))return false;
            for(unsigned offset:{0u,8u,12u})if(std::memcmp(base+x.object+208+offset,x.derived_owner.data()+offset,4))return false;
        }
        return true;
    }
    bool Intact()const{for(const auto& x:phantoms)if(!FixedOwnership(x))return false;return true;}
    ~MutedPhantomCallbacks(){
        for(const auto& x:phantoms)if(FixedOwnership(x)){
            // Restore only the temporarily changed fields. Never revive an old
            // listener-array pointer if unexpected engine callbacks replaced it.
            std::memcpy(base+x.object+140,x.listeners.data()+12,4);std::memcpy(base+x.object+152,x.listeners.data()+24,4);
            if(x.table==0x82093948)std::memcpy(base+x.object+212,x.derived_owner.data()+4,4);
        }
    }
};
void ReserveOverlaps(Calls& calls,const FrozenPhantom& p){
    auto* base=calls.base;const auto header=p.object+p.array;const auto capacity=PPC_LOAD_U32(header+8)&0x3FFFFFFF;
    if(capacity<p.overlaps.size()){
        calls.Reset();calls.ctx.r3.u64=0x8338A734;calls.ctx.r4.u64=header;calls.ctx.r5.u64=p.overlaps.size();calls.ctx.r6.u64=4;__imp__sub_8238EDD8(calls.ctx,base);
        Need(!calls.ctx.r3.u32,"Native phantom overlap allocation failed; world transaction recovery required");
    }
    Need((PPC_LOAD_U32(header+8)&0x3FFFFFFF)>=p.overlaps.size() &&
        (p.overlaps.empty() || Readable(base,PPC_LOAD_U32(header),p.overlaps.size()*4)),"Prepared phantom overlap storage unavailable");
}
}
void ObserveNativeCheckpointAgentLoad(uint32_t collision,uint32_t source,uint32_t status)noexcept{
    if(!contact_loads)return;
    // Prebuilt map lookup and scalar updates only: do not allocate, log, change
    // guest registers or consume game random/time state in the native loader.
    if(collision!=contact_loads->collision){++contact_loads->unexpected;return;}
    const auto found=contact_loads->indices.find(source);if(found==contact_loads->indices.end()){++contact_loads->unexpected;return;}
    auto& a=contact_loads->agents[found->second];++a.calls;a.status=status;contact_loads->active=nullptr;
}
void BeginNativeCheckpointAgentLoad(PPCContext& ctx,uint8_t* base)noexcept{
    if(!contact_loads)return;const auto found=contact_loads->indices.find(ctx.r4.u32);
    if(ctx.r7.u32!=contact_loads->collision || found==contact_loads->indices.end() || contact_loads->active){++contact_loads->unexpected;return;}
    auto& a=contact_loads->agents[found->second];a.manager=PPC_LOAD_U32(ctx.r8.u32+8);contact_loads->active=&a;
}
std::pair<uint32_t,uint32_t> NativeCheckpointContactAtomOwner(PPCContext& ctx,uint8_t* base)noexcept{
    if(!contact_loads || !contact_loads->active || uint32_t(ctx.lr)!=0x824DF5A8)return{};
    const auto manager=ctx.r31.u32;if(manager!=contact_loads->active->manager){++contact_loads->unexpected;return{};}
    return{manager,PPC_LOAD_U32(manager+60)};
}
void ObserveNativeCheckpointContactAtom(uint32_t manager,uint32_t old_atom,uint32_t new_atom)noexcept{
    if(!manager || !contact_loads || !contact_loads->active)return;auto& a=*contact_loads->active;
    if(a.manager!=manager){++contact_loads->unexpected;return;}a.old_atom=old_atom;a.new_atom=new_atom;++a.replacements;
}
struct NativeRigidBodyCheckpoint::Impl {
    uint32_t data=0,world=0,thread=0,simulation=0,collision=0;uint64_t epoch=0;
    Inventory inventory;std::vector<FrozenMotion> motion;std::vector<uint32_t> stored_references;
    AgentViews agent_views;
    std::vector<FrozenIsland> islands;
    std::vector<FrozenPhantom> phantoms;std::vector<uint32_t> overlap_references;uint64_t entity_lifetime=0;
    std::array<uint8_t,28> simulation_clock{};std::array<uint8_t,16> world_step{},collision_step{};
};
NativeRigidBodyCheckpoint::NativeRigidBodyCheckpoint():impl_(std::make_unique<Impl>()){}
NativeRigidBodyCheckpoint::~NativeRigidBodyCheckpoint(){if(impl_->data || !impl_->stored_references.empty() || !impl_->overlap_references.empty())REXLOG_ERROR("Native rigid-body checkpoint destroyed before owner-thread Release; native references retained");}
unsigned NativeRigidBodyCheckpoint::bodies()const{return unsigned(impl_->motion.size());}
unsigned NativeRigidBodyCheckpoint::contacts()const{return impl_->inventory.contacts;}
unsigned NativeRigidBodyCheckpoint::stored_motions()const{return unsigned(impl_->stored_references.size());}
unsigned NativeRigidBodyCheckpoint::phantoms()const{return unsigned(impl_->phantoms.size());}
unsigned NativeRigidBodyCheckpoint::overlaps()const{unsigned result=0;for(const auto& p:impl_->phantoms)result+=unsigned(p.overlaps.size());return result;}
bool NativeRigidBodyCheckpoint::Capture(PPCContext& ctx,uint8_t* base,uint32_t world,uint64_t epoch,std::string& error){
    error.clear();auto& s=*impl_;if(s.data || !s.stored_references.empty() || !s.overlap_references.empty() || !epoch){error="Checkpoint must be empty and have a nonzero world epoch";return false;}
    try{
        Calls calls(ctx,base);Need(Readable(base,world,896) && PPC_LOAD_U32(world)==0x820300C4 && !PPC_LOAD_U32(world+148) && !PPC_LOAD_U32(world+152),"Native physics world is unavailable or inside a critical operation");
        Need(SampleNativeEntityLifetimeRevision(s.entity_lifetime),"Native entity owners are being mutated during physics capture");
        s.data=CaptureData(calls,world,true);s.world=world;s.epoch=epoch;s.thread=GetCurrentThreadId();
        s.inventory=ReadInventory(base,s.data);
        s.agent_views=ReadAgentViews(base,s.inventory);ReadAgentModifiers(calls,s.agent_views);
        REXLOG_INFO("Private native physics inventory: {} bodies, {} constraints, {} actions, {} phantoms, {} saved contacts",s.inventory.objects[0].size(),s.inventory.objects[1].size(),s.inventory.objects[2].size(),s.inventory.objects[3].size(),s.inventory.contacts);
        unsigned action_index=0;for(const auto action:s.inventory.objects[2]){if(action_index++>=16)break;Need(Readable(base,action,8),"Native physics action header unavailable");REXLOG_INFO("Private native physics action {:08X} class {:08X}",action,PPC_LOAD_U32(action));}
        s.stored_references.reserve(s.inventory.objects[0].size());std::unordered_set<uint32_t> retained,islands;
        for(const auto body:s.inventory.objects[0]){
            auto motion=Freeze(base,body);Need(PPC_LOAD_U32(body+8)==world,"Native body ownership changed");
            if(motion.stored && retained.insert(motion.stored).second){
                calls.Reset();ctx.r3.u64=motion.stored;__imp__sub_8238C398(ctx,base);s.stored_references.push_back(motion.stored);
            }
            s.motion.push_back(std::move(motion));
            const auto island=PPC_LOAD_U32(body+200);if(island && islands.insert(island).second)s.islands.push_back(FreezeIsland(base,island,world));
        }
        std::unordered_set<uint32_t> retained_overlaps;size_t overlap_count=0;
        for(const auto object:s.inventory.objects[3]){
            auto p=FreezePhantom(base,object,world);
            overlap_count+=p.overlaps.size()+p.listener_entries[0].size()+p.listener_entries[1].size();Need(overlap_count<=262144,"Native phantom snapshot exceeds bounded overlap/listener storage");
            for(const auto collidable:p.overlaps){const auto owner=CollidableOwner(base,collidable,world);
                if(retained_overlaps.insert(owner).second){s.overlap_references.push_back(owner);calls.Reset();ctx.r3.u64=owner;__imp__sub_8238C398(ctx,base);}
            }
            s.phantoms.push_back(std::move(p));
        }
        s.simulation=PPC_LOAD_U32(world+8);s.collision=PPC_LOAD_U32(world+120);
        Need(Readable(base,s.simulation,44) && Readable(base,s.collision+80,16),"Native simulation/step-info layout unavailable");
        std::memcpy(s.simulation_clock.data(),PPC_RAW_ADDR(s.simulation+16),s.simulation_clock.size());
        std::memcpy(s.world_step.data(),PPC_RAW_ADDR(world+480),s.world_step.size());std::memcpy(s.collision_step.data(),PPC_RAW_ADDR(s.collision+80),s.collision_step.size());
        uint64_t entity_lifetime=0;Need(SampleNativeEntityLifetimeRevision(entity_lifetime) && entity_lifetime==s.entity_lifetime,"Native entity owners changed during physics capture");return true;
    }catch(const std::exception& exception){error=exception.what();Release(ctx,base);return false;}
    catch(...){error="Native rigid-body capture failed";Release(ctx,base);return false;}
}
bool NativeRigidBodyCheckpoint::Release(PPCContext& ctx,uint8_t* base){
    auto& s=*impl_;if(!s.data && s.stored_references.empty() && s.overlap_references.empty())return true;if(s.thread && s.thread!=GetCurrentThreadId())return false;
    try{Calls calls(ctx,base);
        if(s.data){calls.Reset();calls.ctx.r3.u64=s.data;__imp__sub_8238C448(calls.ctx,base);s.data=0;}
        while(!s.stored_references.empty()){calls.Reset();calls.ctx.r3.u64=s.stored_references.back();__imp__sub_8238C448(calls.ctx,base);s.stored_references.pop_back();}
        while(!s.overlap_references.empty()){calls.Reset();calls.ctx.r3.u64=s.overlap_references.back();__imp__sub_8238C448(calls.ctx,base);s.overlap_references.pop_back();}
        s.motion.clear();s.phantoms.clear();s.islands.clear();s.agent_views.clear();s.inventory={};return true;
    }catch(...){return false;}
}
bool NativeRigidBodyCheckpoint::SampleMotion(uint8_t* base,std::vector<NativePhysicsMotionSample>& out,std::string& error)const{
    struct Host{DWORD error=GetLastError();unsigned csr=_mm_getcsr();~Host(){SetLastError(error);_mm_setcsr(csr);}}host;GuestReadBatch reads;
    try{const auto& s=*impl_;Need(s.data && s.thread==GetCurrentThreadId(),"Native physics phase sample has another owner");
        std::vector<NativePhysicsMotionSample> next;next.reserve(s.motion.size());
        for(const auto& captured:s.motion){Need(Readable(base,captured.body,544) && PPC_LOAD_U32(captured.body)==0x82030384 && PPC_LOAD_U32(captured.body+8)==s.world,"Native physics phase body owner changed");
            NativePhysicsMotionSample sample;sample.body=captured.body;std::memcpy(sample.bytes.data(),base+sample.body+224,320);std::memcpy(sample.metadata.data(),base+sample.body+128,sample.metadata.size());
            sample.island=PPC_LOAD_U32(sample.body+200);if(sample.island){Need(Readable(base,sample.island,sample.island_metadata.size()),"Native physics phase island header unavailable");std::memcpy(sample.island_metadata.data(),base+sample.island,sample.island_metadata.size());}
            next.push_back(sample);
        }out=std::move(next);error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
bool NativeRigidBodyCheckpoint::Matches(PPCContext& ctx,uint8_t* base,uint32_t world,uint64_t epoch,std::string& error)const{
    try{
        const auto& s=*impl_;Calls calls(ctx,base);uint64_t revision=0;
        Need(s.data && s.world==world && s.epoch==epoch && s.thread==GetCurrentThreadId() &&
            SampleNativeEntityLifetimeRevision(revision) && revision==s.entity_lifetime && Readable(base,world,896) &&
            PPC_LOAD_U32(world)==0x820300C4 && !PPC_LOAD_U32(world+148) && !PPC_LOAD_U32(world+152) &&
            PPC_LOAD_U32(world+8)==s.simulation && PPC_LOAD_U32(world+120)==s.collision,"Native physics comparison scope changed");
        Owners temporary{calls};const auto current=CaptureData(calls,world,false);temporary.Keep(current);
        Need(ReadInventory(base,current).objects==s.inventory.objects,"Native physics comparison inventory changed");
        for(const auto& island:s.islands){CheckIsland(base,island);Need(PPC_LOAD_U8(island.object+37)==island.flags,"Native island runtime flags differ from checkpoint");}
        for(const auto& original:s.motion){const auto now=Freeze(base,original.body);
            if(now.bytes!=original.bytes || now.shape!=original.shape || now.stored!=original.stored ||
                (now.stored && std::memcmp(now.stored_bytes.data()+8,original.stored_bytes.data()+8,280))){
                unsigned byte=0;while(byte<now.bytes.size() && now.bytes[byte]==original.bytes[byte])++byte;
                std::ostringstream why;why<<"Native motion differs: body 0x"<<std::hex<<original.body<<" payload byte "<<std::dec<<byte<<" kinematics "<<SameKinematics(now,original)<<" linear velocity words expected/actual";
                for(unsigned offset:{208u,212u,216u})why<<" "<<std::hex<<Word(original,offset)<<"/"<<Word(now,offset);
                throw std::runtime_error(why.str());
            }
        }
        for(const auto& original:s.phantoms){const auto now=FreezePhantom(base,original.object,world);
            if(!(now.table==original.table && now.shape==original.shape && now.userdata==original.userdata && now.filter==original.filter &&
                now.pose==original.pose && now.overlaps==original.overlaps && now.order_dirty==original.order_dirty &&
                now.listeners==original.listeners && now.listener_entries==original.listener_entries && now.derived_owner==original.derived_owner)){
                std::ostringstream why;why<<"Native phantom differs: object 0x"<<std::hex<<original.object<<" class "<<original.table<<" ownership "<<(now.table==original.table && now.shape==original.shape && now.userdata==original.userdata && now.filter==original.filter)
                    <<" pose "<<(now.pose==original.pose)<<" overlaps "<<(now.overlaps==original.overlaps)<<" dirty "<<unsigned(original.order_dirty)<<"/"<<unsigned(now.order_dirty)<<" listeners "<<(now.listeners==original.listeners && now.listener_entries==original.listener_entries)<<" derived "<<(now.derived_owner==original.derived_owner);
                if(now.pose!=original.pose){unsigned i=0;while(i<now.pose.size() && now.pose[i]==original.pose[i])++i;why<<" first pose byte "<<std::dec<<i;}
                why<<" overlap counts "<<std::dec<<original.overlaps.size()<<"/"<<now.overlaps.size();throw std::runtime_error(why.str());
            }
        }
        Need(Readable(base,s.simulation+16,s.simulation_clock.size()) && Readable(base,s.collision+80,s.collision_step.size()) &&
            !std::memcmp(base+s.simulation+16,s.simulation_clock.data(),s.simulation_clock.size()) &&
            !std::memcmp(base+world+480,s.world_step.data(),s.world_step.size()) &&
            !std::memcmp(base+s.collision+80,s.collision_step.data(),s.collision_step.size()),"Native simulation clocks differ from checkpoint");
        Need(SampleNativeEntityLifetimeRevision(revision) && revision==s.entity_lifetime,"Entity lifetime changed during physics comparison");
        error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}catch(...){error="Native physics comparison failed";return false;}
}
bool NativeRigidBodyCheckpoint::Restore(PPCContext& ctx,uint8_t* base,uint32_t world,uint64_t epoch,std::string& error)const{
    error.clear();const auto& s=*impl_;
    if(!s.data || s.world!=world || s.epoch!=epoch || s.thread!=GetCurrentThreadId()){error="Native physics checkpoint belongs to a different world, epoch or owner";return false;}
    try{
        Calls calls(ctx,base);Need(Readable(base,world,896) && PPC_LOAD_U32(world)==0x820300C4 && !PPC_LOAD_U32(world+148) && !PPC_LOAD_U32(world+152) &&
            PPC_LOAD_U32(world+8)==s.simulation && PPC_LOAD_U32(world+120)==s.collision,"Native world or solver ownership changed");
        uint64_t entity_lifetime=0;Need(SampleNativeEntityLifetimeRevision(entity_lifetime) && entity_lifetime==s.entity_lifetime,"Native entity lifetime changed; restore engine ownership before physics");
        Owners temporary{calls};const auto current=CaptureData(calls,world,false);temporary.Keep(current);
        Need(ReadInventory(base,current).objects==s.inventory.objects,"Native physics object inventory changed; lifetime restoration is required before motion restore");
        for(const auto& motion:s.motion){
            Need(Readable(base,motion.body,544) && PPC_LOAD_U32(motion.body)==0x82030384 && PPC_LOAD_U32(motion.body+8)==world &&
                PPC_LOAD_U32(motion.body+16)==motion.shape && PPC_LOAD_U32(motion.body+224)==Word(motion,0) &&
                PPC_LOAD_U8(motion.body+232)==motion.bytes[8] && StoredMotion(base,motion.body)==motion.stored,"Native body ownership/type/shape/stored-motion identity changed; restore rejected");
            // Native rigid-body clone copies 288 motion bytes (18 vectors).
            // The following bytes own arrays (8247E0C0/8247E378); copying their
            // pointers/counters back could revive freed storage. Fence them.
            Need(!std::memcmp(PPC_RAW_ADDR(motion.body+224+MotionBytes),motion.bytes.data()+MotionBytes,motion.bytes.size()-MotionBytes),
                "Native rigid-body tail ownership changed; separate lifetime restoration required");
            if(motion.stored)Need(Readable(base,motion.stored,288) && !std::memcmp(PPC_RAW_ADDR(motion.stored),motion.stored_bytes.data(),4) &&
                PPC_LOAD_U8(motion.stored+8)==motion.stored_bytes[8],"Retained stored-motion class changed; restore rejected");
        }
        for(const auto& p:s.phantoms)CheckPhantomIdentity(base,p);
        for(const auto& p:s.islands)CheckIsland(base,p,false);
        // Native reserve may change array capacity, but never pose/list counts
        // or callbacks. A failed native allocation requires owner recovery.
        for(const auto& p:s.phantoms)ReserveOverlaps(calls,p);
        MutedPhantomCallbacks mute(base,s.phantoms);
        calls.Reset();ctx.r3.u64=world;__imp__sub_8246A1C0(ctx,base);
        struct Unlock {Calls& calls;uint32_t world;~Unlock(){calls.Reset();calls.ctx.r3.u64=world;__imp__sub_8246A1F8(calls.ctx,calls.base);}}unlock{calls,world};
        // Reintegrate/recollide reads the simulation's time. Rewind clocks
        // before invoking it, not only after updating the body's motion bytes.
        std::memcpy(PPC_RAW_ADDR(s.simulation+16),s.simulation_clock.data(),s.simulation_clock.size());
        std::memcpy(PPC_RAW_ADDR(world+480),s.world_step.data(),s.world_step.size());
        std::memcpy(PPC_RAW_ADDR(s.collision+80),s.collision_step.data(),s.collision_step.size());
        for(const auto& motion:s.motion){
            if(motion.stored)std::memcpy(PPC_RAW_ADDR(motion.stored+8),motion.stored_bytes.data()+8,motion.stored_bytes.size()-8);
            if(!std::memcmp(PPC_RAW_ADDR(motion.body+232),motion.bytes.data()+8,MotionBytes-8))continue;
            std::memcpy(PPC_RAW_ADDR(motion.body+232),motion.bytes.data()+8,MotionBytes-8);
            calls.Reset();ctx.r3.u64=motion.body;__imp__sub_82473DF0(ctx,base);
            std::memcpy(PPC_RAW_ADDR(motion.body+232),motion.bytes.data()+8,MotionBytes-8);
        }
        for(const auto& p:s.phantoms)if(std::memcmp(base+p.object+160,p.pose.data(),p.pose_size)){
            const auto pose=calls.stack+1024;std::memcpy(base+pose,p.pose.data(),p.pose_size);
            calls.Reset();ctx.r3.u64=p.object;ctx.r4.u64=pose;
            if(p.array==192)__imp__sub_82485D78(ctx,base);else __imp__sub_82486B48(ctx,base);
            std::memcpy(base+p.object+160,p.pose.data(),p.pose_size);
        }
        std::memcpy(PPC_RAW_ADDR(s.simulation+16),s.simulation_clock.data(),s.simulation_clock.size());
        std::memcpy(PPC_RAW_ADDR(world+480),s.world_step.data(),s.world_step.size());std::memcpy(PPC_RAW_ADDR(s.collision+80),s.collision_step.data(),s.collision_step.size());
        const auto load=calls.stack+1024;std::memset(PPC_RAW_ADDR(load),0,16);
        // Input+9 callback reconstruction was tested and did not fix replay;
        // keep it disabled while modifier ownership/state is audited separately.
        const auto modifier_links=CaptureModifierLinks(calls,s.agent_views);
        RemoveModifiersBeforeLoad(calls,s.agent_views,modifier_links);
        {
            ContactLoadScope loads(base,world,s.inventory);
            for(const auto system:s.inventory.contact_systems){calls.Reset();ctx.r3.u64=load;ctx.r4.u64=system;ctx.r5.u64=world;__imp__sub_824DFA58(ctx,base);}
            loads.Verify();
            RecreateModifiersAfterLoad(calls,s.agent_views,loads.data);
        }
        const auto recaptured=CaptureData(calls,world,true);temporary.Keep(recaptured);
        auto recaptured_agents=ReadAgentViews(base,ReadInventory(base,recaptured));ReadAgentModifiers(calls,recaptured_agents);DescribeAgentRestore(s.agent_views,recaptured_agents);
        for(const auto& [pair,v]:s.agent_views){const auto found=recaptured_agents.find(pair);Need(found!=recaptured_agents.end() && found->second.modifiers==v.modifiers && found->second.solver_atom==v.solver_atom && found->second.terminal_matches,"Native modifier reconstruction differs from captured semantic state");}
        // Native contact reconstruction marks island structural-work flags.
        // Restore only this audited byte after every topology/solver guard
        // passes. Never overwrite array headers, membership or solver totals.
        for(const auto& p:s.islands)CheckIsland(base,p);
        for(const auto& p:s.islands)PPC_STORE_U8(p.object+37,p.flags);
        Need(mute.Intact(),"Native phantom listener/engine ownership changed during reconstruction; world recovery required");
        for(const auto& p:s.phantoms){
            ReserveOverlaps(calls,p);const auto pointer=PPC_LOAD_U32(p.object+p.array);
            for(unsigned i=0;i<p.overlaps.size();++i)PPC_STORE_U32(pointer+i*4,p.overlaps[i]);
            PPC_STORE_U32(p.object+p.array+4,uint32_t(p.overlaps.size()));PPC_STORE_U8(p.object+p.array+12,p.order_dirty);
        }
        Need(SampleNativeEntityLifetimeRevision(entity_lifetime) && entity_lifetime==s.entity_lifetime,"Native physics restore changed engine entity ownership; world recovery required");
        return true;
    }catch(const std::exception& exception){error=exception.what();return false;}catch(...){error="Native physics restore failed during original native operations";return false;}
}
namespace {
bool CaptureActualLevelPhysics(PPCContext& ctx,uint8_t* base){
    if(!Readable(base,0x833DA7E4,4))return true;
    const auto registry=PPC_LOAD_U32(0x833DA7E4);
    if(!Readable(base,registry,232))return true;
    const auto manager=PPC_LOAD_U32(registry+228);
    if(!Readable(base,manager,8))return true;
    const auto world=PPC_LOAD_U32(manager+4);
    if(!world){REXLOG_INFO("Native actual physics checkpoint skipped: no loaded native world");return true;}
    Need(Readable(base,world,896) && PPC_LOAD_U32(world)==0x820300C4 &&
        !PPC_LOAD_U32(world+148) && !PPC_LOAD_U32(world+152),"Actual level world is not at a completed native physics boundary");
    Calls calls(ctx,base);Owners owners{calls};
    const auto data=CaptureData(calls,world,false);owners.Keep(data);
    const auto inventory=ReadInventory(base,data);
    std::array<unsigned,16> motion_types{};unsigned stored=0,derived=0;std::vector<FrozenMotion> before;
    std::vector<FrozenPhantom> phantoms_before;
    for(const auto body:inventory.objects[0]){
        Need(Readable(base,body,544),"Actual world contains an unreadable body");
        ++motion_types[PPC_LOAD_U8(body+232)&15];stored+=StoredMotion(base,body)!=0;derived+=PPC_LOAD_U32(body)!=0x82030384;
        before.push_back(Freeze(base,body));
    }
    REXLOG_INFO("Native actual physics inventory: world {:08X}, {} bodies, {} constraints, {} actions, {} phantoms, {} stored-motion bodies, {} derived bodies; type 1/2/3/4/5/6/7 {}/{}/{}/{}/{}/{}/{}",
        world,inventory.objects[0].size(),inventory.objects[1].size(),inventory.objects[2].size(),inventory.objects[3].size(),stored,derived,
        motion_types[1],motion_types[2],motion_types[3],motion_types[4],motion_types[5],motion_types[6],motion_types[7]);
    std::map<uint32_t,std::pair<unsigned,uint32_t>> phantom_classes;
    for(const auto phantom:inventory.objects[3]){
        Need(Readable(base,phantom,128),"Actual world phantom header is unreadable");
        auto& group=phantom_classes[PPC_LOAD_U32(phantom)];++group.first;group.second=phantom;
        phantoms_before.push_back(FreezePhantom(base,phantom,world));
    }
    Need(phantom_classes.size()<=32,"Actual world has too many phantom classes for bounded diagnostics");
    for(const auto& [table,group]:phantom_classes){
        Need(Readable(base,table,96),"Actual world phantom vtable unavailable");
        REXLOG_INFO("Native actual phantom class: vtable {:08X}, count {}, example {:08X}, native allocation size {}, virtual +48..92 {:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}",
            table,group.first,group.second,PPC_LOAD_U16(group.second+4),PPC_LOAD_U32(table+48),PPC_LOAD_U32(table+52),PPC_LOAD_U32(table+56),PPC_LOAD_U32(table+60),PPC_LOAD_U32(table+64),PPC_LOAD_U32(table+68),PPC_LOAD_U32(table+72),PPC_LOAD_U32(table+76),PPC_LOAD_U32(table+80),PPC_LOAD_U32(table+84),PPC_LOAD_U32(table+88),PPC_LOAD_U32(table+92));
    }
    NativeRigidBodyCheckpoint checkpoint;
    struct Release {NativeRigidBodyCheckpoint& value;PPCContext& ctx;uint8_t* base;~Release(){value.Release(ctx,base);}}release{checkpoint,ctx,base};
    std::string error;
    if(!checkpoint.Capture(ctx,base,world,1,error)){REXLOG_ERROR("Native actual level physics capture rejected: {}",error);return false;}
    Need(PPC_LOAD_U32(0x833DA7E4)==registry && PPC_LOAD_U32(registry+228)==manager && PPC_LOAD_U32(manager+4)==world,
        "Native actual world owner changed during capture");
    for(const auto& original:before){const auto current=Freeze(base,original.body);
        Need(current.bytes==original.bytes && current.stored==original.stored &&
            (!current.stored || !std::memcmp(current.stored_bytes.data()+8,original.stored_bytes.data()+8,280)),"Actual physics capture changed a live motion payload");
    }
    auto verify_phantoms=[&]{for(const auto& original:phantoms_before){const auto current=FreezePhantom(base,original.object,world);
        Need(current.pose==original.pose && current.overlaps==original.overlaps && current.order_dirty==original.order_dirty &&
            current.listeners==original.listeners && current.listener_entries==original.listener_entries && current.derived_owner==original.derived_owner,
            "Actual phantom payload/listener/engine ownership differs from the captured boundary");
    }};
    verify_phantoms();
    REXLOG_INFO("Native actual level physics capture passed: {} rigid bodies, {} retained stored motions, {} saved contact streams, {} phantoms/{} overlap entries; capture/release only, actual world not restored",checkpoint.bodies(),checkpoint.stored_motions(),checkpoint.contacts(),checkpoint.phantoms(),checkpoint.overlaps());
    if(REXCVAR_GET(em2_probe_native_physics_restore)){
        Need(REXCVAR_GET(em2_private_profile),"Actual physics restore is private-probe only");
        auto fatal=[&](const std::string& why){REXLOG_ERROR("Private actual physics restore stopped before simulation resumed: {}",why);spdlog::shutdown();ExitProcess(1);};
        if(!checkpoint.Restore(ctx,base,world,1,error))fatal(error);
        try{
            for(const auto& original:before){const auto current=Freeze(base,original.body);
                Need(current.bytes==original.bytes && current.stored==original.stored &&
                    (!current.stored || !std::memcmp(current.stored_bytes.data()+8,original.stored_bytes.data()+8,280)),"Actual rigid-body payload differs after immediate physics restore");
            }
            verify_phantoms();
        }catch(const std::exception& e){fatal(e.what());}
        REXLOG_INFO("Private actual physics immediate restore passed: {} bodies, {} stored motions, {} phantoms/{} ordered overlaps, {} contact streams, unchanged pose/ownership data checked; same boundary only, no simulation frame rewound",checkpoint.bodies(),checkpoint.stored_motions(),checkpoint.phantoms(),checkpoint.overlaps(),checkpoint.contacts());
    }
    Need(checkpoint.Release(ctx,base),"Actual native physics capture did not release its retained references");
    return true;
}
}

bool TestNativePhysicsCheckpoint(PPCContext& ctx,uint8_t* base,uint32_t,uint32_t,unsigned){
    try{
        uint64_t lifetime_revision=0;
        Need(SampleNativeEntityLifetimeRevision(lifetime_revision) && lifetime_revision>1,
            "Native lifecycle hooks have not observed a completed startup boundary");
        NativeEntityOwnership ownership;const char* ownership_error=nullptr;uint32_t ownership_address=0;
        if(ReadNativeEntityOwnership(base,ownership,ownership_error,ownership_address)){
            REXLOG_INFO("Native entity lifetime boundary: revision {}, registry {:08X}, {} actual native entities/components inspected; observation only",lifetime_revision,ownership.registry,ownership.entities.size());
            Need(ProbeNativeComponentSchemas(ctx,base,ownership),"Native component schema discovery failed");
        }
        else REXLOG_INFO("Native entity lifetime boundary: revision {}, ownership unavailable '{}' at {:08X}; no ownership snapshot accepted",lifetime_revision,ownership_error?ownership_error:"unknown",ownership_address);
        Need(Readable(base,0x833DA7E4,4),"Actual native deferred queue registry unavailable");
        NativeDeferredEntityCheckpoint live_deferred;std::string deferred_error;
        struct ReleaseDeferred {NativeDeferredEntityCheckpoint& value;PPCContext& ctx;uint8_t* base;~ReleaseDeferred(){value.Release(ctx,base);}}release_deferred{live_deferred,ctx,base};
        if(!live_deferred.Capture(ctx,base,PPC_LOAD_U32(0x833DA7E4),1,deferred_error))throw std::runtime_error(deferred_error);
        REXLOG_INFO("Native deferred queue actual capture: {} retained interfaces; capture/release only, no live actor or queue restored",live_deferred.size());
        Need(live_deferred.Release(ctx,base),"Actual native deferred queue capture release failed");
        Need(TestNativeDeferredEntityCheckpoint(ctx,base),"Original native deferred queue checkpoint fixture failed");
        Calls calls(ctx,base);Owners owners{calls};auto& c=calls.ctx;
        auto* kernel=rex::kernel::kernel_state();Need(kernel && kernel->memory(),"Native physics probe allocation context unavailable");
        struct Scratch {rex::memory::Memory& memory;uint32_t address;~Scratch(){if(address)memory.SystemHeapFree(address);}}scratch{*kernel->memory(),kernel->memory()->SystemHeapAlloc(1024,32)};
        Need(Readable(base,scratch.address,1024),"Native physics probe scratch unavailable");
        const auto info=scratch.address,extents=info+256;
        const auto empty=calls.Allocate(24);calls.Reset();c.r3.u64=empty;__imp__sub_824D7BC0(c,base);owners.Keep(empty);
        REXLOG_INFO("Native physics checkpoint probe: creating independent original Havok world");
        calls.Reset();c.r3.u64=empty;c.r4.u64=1;__imp__sub_824D7A20(c,base);const auto world=c.r3.u32;
        Need(Readable(base,world,896) && PPC_LOAD_U32(world)==0x820300C4,"Original physics-data world creation failed");owners.Keep(world);
        Float(base,extents,8);Float(base,extents+4,.5f);Float(base,extents+8,8);Float(base,extents+12,0);
        const auto box=calls.Allocate(48);calls.Reset();c.r3.u64=box;c.r4.u64=extents;c.f1.f64=.04;__imp__sub_824522B0(c,base);owners.Keep(box);
        Float(base,extents,.5f);Float(base,extents+4,.5f);Float(base,extents+8,.5f);
        const auto cube=calls.Allocate(48);calls.Reset();c.r3.u64=cube;c.r4.u64=extents;c.f1.f64=.04;__imp__sub_824522B0(c,base);owners.Keep(cube);
        auto body=[&](uint32_t shape,float height,bool fixed){
            std::memset(PPC_RAW_ADDR(info),0,256);calls.Reset();c.r3.u64=info;__imp__sub_8249B068(c,base);
            PPC_STORE_U32(info+4,shape);Float(base,info+20,height);PPC_STORE_U8(info+180,fixed?5:1);
            // Keep this first solver proof active. Sleeping/island restoration
            // and native game callback state are separate integration work.
            PPC_STORE_U8(info+181,0);Float(base,info+160,0);Float(base,info+168,0);
            if(!fixed)Float(base,info+48,1.25f);
            const auto result=calls.Allocate(544);calls.Reset();c.r3.u64=result;c.r4.u64=info;__imp__sub_82474700(c,base);
            Need(c.r3.u32==result && PPC_LOAD_U32(result)==0x82030384,"Native test body constructor failed");owners.Keep(result);
            calls.Reset();c.r3.u64=world;__imp__sub_8246A1C0(c,base);
            calls.Reset();c.r3.u64=world;c.r4.u64=result;c.r5.u64=1;__imp__sub_8246A600(c,base);
            calls.Reset();c.r3.u64=world;__imp__sub_8246A1F8(c,base);
            Need(PPC_LOAD_U32(result+8)==world,"Test body was not admitted to its independent world");return result;
        };
        const auto floor=body(box,-.5f,true),ball=body(cube,1.5f,false);(void)floor;
        const auto simulation=PPC_LOAD_U32(world+8),collision=PPC_LOAD_U32(world+120);
        Need(Readable(base,simulation,44) && Readable(base,collision+80,16),"Native simulation/step-info layout unavailable");
        auto step=[&]{calls.Reset();c.r3.u64=world;c.f1.f64=1./30;__imp__sub_8246AD50(c,base);Need(!c.r3.u32,"Original native physics solver did not complete");};
        for(unsigned i=0;i<45;++i)step();
        REXLOG_INFO("Native physics checkpoint fixture settled: floor Y {}, moving box Y {} VY {} type {} deactivation counter {}",std::bit_cast<float>(PPC_LOAD_U32(floor+292)),std::bit_cast<float>(PPC_LOAD_U32(ball+292)),std::bit_cast<float>(PPC_LOAD_U32(ball+436)),PPC_LOAD_U8(ball+232),PPC_LOAD_U8(ball+233));
        REXLOG_INFO("Native physics checkpoint probe: capturing native component through reusable owner-bound checkpoint");
        NativeRigidBodyCheckpoint checkpoint;
        struct ReleaseCheckpoint {NativeRigidBodyCheckpoint& checkpoint;Calls& calls;~ReleaseCheckpoint(){checkpoint.Release(calls.ctx,calls.base);}}release{checkpoint,calls};
        std::string why;if(!checkpoint.Capture(ctx,base,world,1,why))throw std::runtime_error(why);
        const auto contacts=checkpoint.contacts();Need(contacts && checkpoint.bodies()==2,"Native solver fixture must retain both real bodies and saved contacts");
        const auto motion=Freeze(base,ball);
        std::array<FrozenMotion,12> baseline;for(auto& frame:baseline){step();frame=Freeze(base,ball);}
        const auto distance=std::bit_cast<float>(Word(baseline.back(),64))-std::bit_cast<float>(Word(motion,64));
        Need(std::isfinite(distance) && distance>.1f && baseline.back().bytes!=motion.bytes,"Native replay fixture did not actually move after capture");
        const auto before_reject=Freeze(base,ball);
        Need(!checkpoint.Restore(ctx,base,world,2,why) && Freeze(base,ball).bytes==before_reject.bytes,"A different world epoch must be rejected before changing native motion");
        const auto added=body(cube,30,true);
        Need(!checkpoint.Restore(ctx,base,world,1,why) && why.find("inventory")!=std::string::npos && Freeze(base,ball).bytes==before_reject.bytes,
            "A new native body must invalidate the old inventory before any motion is overwritten");
        calls.Reset();c.r3.u64=world;__imp__sub_8246A1C0(c,base);
        PPC_STORE_U8(info+512,0);calls.Reset();c.r3.u64=info+512;c.r4.u64=world;c.r5.u64=added;__imp__sub_8246A7C8(c,base);
        Need(PPC_LOAD_U8(info+512) && !PPC_LOAD_U32(added+8),"Original native body removal did not complete");
        calls.Reset();c.r3.u64=world;__imp__sub_8246A1F8(c,base);
        if(!checkpoint.Restore(ctx,base,world,1,why))throw std::runtime_error(why);
        unsigned exact=0;for(unsigned i=0;i<baseline.size();++i){step();const auto replayed=Freeze(base,ball);
            if(!SameKinematics(baseline[i],replayed)){REXLOG_ERROR("Native physics checkpoint replay mismatch at frame {}: Y expected {} actual {}; VY expected {} actual {}; {} saved contact streams",i,
                std::bit_cast<float>(Word(baseline[i],68)),std::bit_cast<float>(Word(replayed,68)),std::bit_cast<float>(Word(baseline[i],212)),std::bit_cast<float>(Word(replayed,212)),contacts);return false;}
            exact+=baseline[i].bytes==replayed.bytes;
        }
        Need(exact==baseline.size(),"Native physics replay did not reproduce every complete motion payload");
        REXLOG_INFO("Native physics checkpoint passed: independent original Havok world, real moving floor/box contacts, {} saved agent streams, distance {} after capture, 12 restored/replayed solver steps with bit-exact kinematics, {} complete motion payload matches; no game world or callback state restored",contacts,distance,exact);
        const auto keyframed=body(cube,30,false);
        calls.Reset();c.r3.u64=world;__imp__sub_8246A1C0(c,base);
        calls.Reset();c.r3.u64=keyframed;c.r4.u64=4;c.r5.u64=1;c.r6.u64=0;__imp__sub_82473C90(c,base);
        calls.Reset();c.r3.u64=world;__imp__sub_8246A1F8(c,base);
        const auto stored=StoredMotion(base,keyframed);
        Need(stored && Readable(base,stored,288),"Native keyframed conversion did not create stored dynamic motion");
        const auto original_refs=PPC_LOAD_U16(stored+6);
        NativeRigidBodyCheckpoint saved_checkpoint;
        ReleaseCheckpoint saved_release{saved_checkpoint,calls};
        if(!saved_checkpoint.Capture(ctx,base,world,1,why))throw std::runtime_error(why);
        Need(saved_checkpoint.stored_motions()==1 && PPC_LOAD_U16(stored+6)==original_refs+1,"Stored dynamic motion was not retained exactly once");
        const auto captured=Freeze(base,keyframed);
        std::array<uint8_t,8> retained_header{};std::memcpy(retained_header.data(),base+stored,8);
        step();Float(base,stored+208,9.5f);
        const auto ball_before_reject=Freeze(base,ball);
        {
            struct PointerGuard {uint8_t* base;uint32_t body,stored;~PointerGuard(){PPC_STORE_U32(body+504,stored);}}guard{base,keyframed,stored};
            PPC_STORE_U32(keyframed+504,0);
            Need(!saved_checkpoint.Restore(ctx,base,world,1,why) && Freeze(base,ball).bytes==ball_before_reject.bytes &&
                PPC_LOAD_U32(stored+208)==std::bit_cast<uint32_t>(9.5f),"Changed stored-motion identity did not reject before native writes");
        }
        if(!saved_checkpoint.Restore(ctx,base,world,1,why))throw std::runtime_error(why);
        Need(!std::memcmp(base+stored+8,captured.stored_bytes.data()+8,280) && !std::memcmp(base+stored,retained_header.data(),8),
            "Stored dynamic motion payload or preserved reference header did not restore");
        Need(saved_checkpoint.Release(ctx,base) && PPC_LOAD_U16(stored+6)==original_refs,"Stored-motion reference release was unbalanced");
        REXLOG_INFO("Native stored-motion checkpoint passed: original dynamic-to-keyframed conversion, retained 288-byte native object, mutated payload restored, all 280 payload bytes matched, header preserved, changed identity rejected before writes, balanced native references");
        // Exercise both native phantom classes with real broadphase enter/exit
        // events. A native referenced object serves as a listener counter: its
        // two temporary callbacks use original addReference, avoiding injected
        // code or a mock collision system. Restore its real vtable/refcount
        // before native destruction, and detach borrowed listener storage.
        const auto listener=calls.Allocate(24);calls.Reset();c.r3.u64=listener;__imp__sub_824D7BC0(c,base);owners.Keep(listener);
        const auto listener_table=PPC_LOAD_U32(listener),counter_table=info+768,counter_array=info+832;
        const auto listener_refs=PPC_LOAD_U16(listener+6);
        for(unsigned i=0;i<8;++i)PPC_STORE_U32(counter_table+i*4,0x8238C398);
        PPC_STORE_U32(counter_array,listener);PPC_STORE_U32(listener,counter_table);
        struct ListenerGuard {
            Calls& calls;uint32_t object,table;uint16_t refs;std::array<uint32_t,2> phantoms{};
            ~ListenerGuard(){auto* base=calls.base;for(auto p:phantoms)if(p){PPC_STORE_U32(p+136,0);PPC_STORE_U32(p+140,0);PPC_STORE_U32(p+144,0x80000000);}
                PPC_STORE_U32(object,table);while(PPC_LOAD_U16(object+6)>refs){calls.Reset();calls.ctx.r3.u64=object;__imp__sub_8238C448(calls.ctx,base);}}
        }listener_guard{calls,listener,listener_table,listener_refs};
        const auto aabb=info+576,transform=info+640;
        const auto x=std::bit_cast<float>(PPC_LOAD_U32(ball+288));
        Float(base,aabb,x-1);Float(base,aabb+4,.25f);Float(base,aabb+8,-1);Float(base,aabb+12,0);
        Float(base,aabb+16,x+1);Float(base,aabb+20,2);Float(base,aabb+24,1);Float(base,aabb+28,0);
        std::memset(base+transform,0,64);for(unsigned i:{0u,20u,40u})Float(base,transform+i,1);
        Float(base,transform+48,x);Float(base,transform+52,.75f);
        const auto aabb_phantom=calls.Allocate(208);calls.Reset();c.r3.u64=aabb_phantom;c.r4.u64=aabb;c.r5.u64=0;__imp__sub_82486600(c,base);owners.Keep(aabb_phantom);listener_guard.phantoms[0]=aabb_phantom;
        const auto shape_phantom=calls.Allocate(352);calls.Reset();c.r3.u64=shape_phantom;c.r4.u64=cube;c.r5.u64=transform;c.r6.u64=0;__imp__sub_824872A0(c,base);owners.Keep(shape_phantom);listener_guard.phantoms[1]=shape_phantom;
        for(const auto p:listener_guard.phantoms){
            PPC_STORE_U32(p+136,counter_array);PPC_STORE_U32(p+140,1);PPC_STORE_U32(p+144,0x80000001);
            calls.Reset();c.r3.u64=world;__imp__sub_8246A1C0(c,base);calls.Reset();c.r3.u64=world;c.r4.u64=p;__imp__sub_8246CA90(c,base);
            calls.Reset();c.r3.u64=world;__imp__sub_8246A1F8(c,base);
        }
        const auto original_aabb=FreezePhantom(base,aabb_phantom,world),original_shape=FreezePhantom(base,shape_phantom,world);
        Need(std::find(original_aabb.overlaps.begin(),original_aabb.overlaps.end(),ball+16)!=original_aabb.overlaps.end() &&
            std::find(original_shape.overlaps.begin(),original_shape.overlaps.end(),ball+16)!=original_shape.overlaps.end() && PPC_LOAD_U16(listener+6)>listener_refs,
            "Native phantom fixture did not generate real overlap/callback events");
        NativeRigidBodyCheckpoint phantom_checkpoint;ReleaseCheckpoint phantom_release{phantom_checkpoint,calls};
        if(!phantom_checkpoint.Capture(ctx,base,world,1,why))throw std::runtime_error(why);
        Float(base,aabb,200);Float(base,aabb+16,202);calls.Reset();c.r3.u64=aabb_phantom;c.r4.u64=aabb;__imp__sub_82485D78(c,base);
        Float(base,transform+48,250);calls.Reset();c.r3.u64=shape_phantom;c.r4.u64=transform;__imp__sub_82486B48(c,base);
        Need(FreezePhantom(base,aabb_phantom,world).overlaps.empty() && FreezePhantom(base,shape_phantom,world).overlaps.empty(),"Native phantom fixture did not leave its overlaps");
        calls.Reset();c.r3.u64=aabb_phantom;__imp__sub_82486588(c,base);calls.Reset();c.r3.u64=shape_phantom;__imp__sub_82487570(c,base);
        Need(!PPC_LOAD_U32(aabb_phantom+192) && !PPC_LOAD_U32(shape_phantom+336),"Native empty overlap storage was not released before reserve test");
        const auto callback_refs=PPC_LOAD_U16(listener+6);
        {
            // A valid null listener entry replaces the callback in place: the
            // array's pointer/count/capacity all remain unchanged. Reject this
            // ownership change before moving bodies or reconstructing arrays.
            struct CounterEntry {uint8_t* base;uint32_t array,listener;~CounterEntry(){PPC_STORE_U32(array,listener);}}entry{base,counter_array,listener};
            const auto before_reject=Freeze(base,ball);PPC_STORE_U32(counter_array,0);
            Need(!phantom_checkpoint.Restore(ctx,base,world,1,why) && Freeze(base,ball).bytes==before_reject.bytes &&
                !PPC_LOAD_U32(aabb_phantom+192) && !PPC_LOAD_U32(shape_phantom+336) && PPC_LOAD_U16(listener+6)==callback_refs,
                "In-place phantom listener replacement did not reject before native state writes");
        }
        if(!phantom_checkpoint.Restore(ctx,base,world,1,why))throw std::runtime_error(why);
        const auto restored_aabb=FreezePhantom(base,aabb_phantom,world),restored_shape=FreezePhantom(base,shape_phantom,world);
        Need(restored_aabb.pose==original_aabb.pose && restored_shape.pose==original_shape.pose && restored_aabb.overlaps==original_aabb.overlaps &&
            restored_shape.overlaps==original_shape.overlaps && restored_aabb.order_dirty==original_aabb.order_dirty && restored_shape.order_dirty==original_shape.order_dirty &&
            restored_aabb.listeners==original_aabb.listeners && restored_shape.listeners==original_shape.listeners && PPC_LOAD_U16(listener+6)==callback_refs,
            "Native phantom poses/ordered overlaps/listeners did not restore without duplicate callbacks");
        Need(phantom_checkpoint.Release(ctx,base),"Native phantom checkpoint reference release failed");
        REXLOG_INFO("Native phantom checkpoint passed: original AABB/simple-shape constructors and broadphase, real entering/leaving overlaps, freed arrays reallocated by native reserve, exact poses/order/dirty flags restored, listener replacement rejected before writes, listener callbacks suppressed during reconstruction and ownership restored");
        return CaptureActualLevelPhysics(ctx,base);
    }catch(const std::exception& error){REXLOG_ERROR("Native physics checkpoint probe failed: {}",error.what());return false;}
}
}

PPC_FUNC_IMPL(sub_824DF228) {
    const auto source=ctx.r4.u32,collision=ctx.r7.u32;
    em2::BeginNativeCheckpointAgentLoad(ctx,base);
    __imp__sub_824DF228(ctx,base);
    em2::ObserveNativeCheckpointAgentLoad(collision,source,ctx.r3.u32);
}
PPC_FUNC_IMPL(sub_8249F470) {
    const auto [manager,old_atom]=em2::NativeCheckpointContactAtomOwner(ctx,base);
    __imp__sub_8249F470(ctx,base);
    em2::ObserveNativeCheckpointContactAtom(manager,old_atom,ctx.r3.u32);
}
