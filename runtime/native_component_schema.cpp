#include "native_component_schema.h"
#include "native_component_checkpoint.h"
#include "native_entity_lifetime.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <map>
#include <array>
#include <set>
#include <stdexcept>
namespace em2 {namespace {
bool Readable(uint8_t* base,uint32_t at,size_t size){return base && at>=4096 && size && uint64_t(at)+size<=0x100000000ull && GuestReadBatch::Readable(base+at,size);}
void Need(bool yes,const char* why){if(!yes)throw std::runtime_error(why);}
struct Host {DWORD error=GetLastError();unsigned csr=_mm_getcsr();~Host(){SetLastError(error);_mm_setcsr(csr);}};
// These exact leaf patterns occur in original component RTTI/property-count
// getters. Decode original instructions as data; no guest function executes.
// In particular an interface's +8 method may use a hidden return buffer and
// must never be invoked as a primary-object RTTI getter.
uint32_t BranchTarget(uint32_t at,uint32_t instruction){const auto displacement=(int32_t(instruction<<6)>>6)&~3;return(instruction&2)?uint32_t(displacement):at+uint32_t(displacement);}
bool MetadataLeafImpl(uint8_t* base,uint32_t function,uint32_t& value,unsigned depth,std::set<uint32_t>& visited){
    if(depth>=8)return false;
    for(unsigned hop=0;hop<4;++hop){
        if(function<0x822F0000 || function>=0x83346908 || (function&3) || !Readable(base,function,12) || !visited.insert(function).second)return false;
        const auto a=PPC_LOAD_U32(function),b=PPC_LOAD_U32(function+4),c=PPC_LOAD_U32(function+8);
        if((a>>26)==18 && !(a&1)){
            function=BranchTarget(function,a);continue;
        }
        if((a&0xFFFF0000)==0x38600000 && b==0x4E800020){value=uint32_t(int32_t(int16_t(a)));return true;}
        // Original inherited property-count wrapper: save frame, call the
        // parent's pure count, add this class's declarations, return.
        if(a==0x7D8802A6 && b==0x9181FFF8 && c==0x9421FFA0 && Readable(base,function,36) &&
           (PPC_LOAD_U32(function+12)>>26)==18 && (PPC_LOAD_U32(function+12)&3)==1 &&
           (PPC_LOAD_U32(function+16)&0xFFFF0000)==0x38630000 && PPC_LOAD_U32(function+20)==0x38210060 &&
           PPC_LOAD_U32(function+24)==0x8181FFF8 && PPC_LOAD_U32(function+28)==0x7D8803A6 && PPC_LOAD_U32(function+32)==0x4E800020){
            uint32_t parent=0;if(!MetadataLeafImpl(base,BranchTarget(function+12,PPC_LOAD_U32(function+12)),parent,depth+1,visited))return false;
            const auto sum=int64_t(parent)+int16_t(PPC_LOAD_U32(function+16));if(sum<0 || sum>4096)return false;value=uint32_t(sum);return true;
        }
        if((a>>26)!=15 || ((a>>16)&31)!=0 || c!=0x4E800020)return false;
        const auto reg=(a>>21)&31;const auto high=uint32_t(uint16_t(a))<<16;
        if(((b>>21)&31)!=3 || ((b>>16)&31)!=reg)return false;
        const auto address=high+uint32_t(int32_t(int16_t(b)));
        if((b>>26)==14){value=address;return true;}
        if((b>>26)==32 && Readable(base,address,4)){value=PPC_LOAD_U32(address);return true;}
        return false;
    }
    return false;
}
bool MetadataLeaf(uint8_t* base,uint32_t function,uint32_t& value){std::set<uint32_t> visited;return MetadataLeafImpl(base,function,value,0,visited);}
bool IndexedDescriptors(uint8_t* base,uint32_t function,uint32_t index,uint32_t& descriptor){
    if(!Readable(base,function,20))return false;
    const auto a=PPC_LOAD_U32(function),c=PPC_LOAD_U32(function+8);
    if((a&0xFFFF0000)!=0x3D600000 || PPC_LOAD_U32(function+4)!=0x548A103A || (c&0xFFFF0000)!=0x392B0000 ||
       PPC_LOAD_U32(function+12)!=0x7C6A482E || PPC_LOAD_U32(function+16)!=0x4E800020)return false;
    const auto array=(uint32_t(uint16_t(a))<<16)+uint32_t(int32_t(int16_t(c)));
    const auto slot=uint64_t(array)+uint64_t(index)*4;if(slot>UINT32_MAX || !Readable(base,uint32_t(slot),4))return false;
    descriptor=PPC_LOAD_U32(uint32_t(slot));return true;
}
bool DescriptorAt(uint8_t* base,uint32_t function,uint32_t index,uint32_t& descriptor,unsigned depth=0){
    if(depth>=8 || function<0x822F0000 || function>=0x83346908 || (function&3) || !Readable(base,function,12))return false;
    if(IndexedDescriptors(base,function,index,descriptor))return true;
    const auto a=PPC_LOAD_U32(function);
    // Mickey's own declarations precede inherited basic-motion declarations.
    if((a&0xFFFF0000)==0x2B040000 && PPC_LOAD_U32(function+4)==0x40980018 && Readable(base,function,36)){
        const auto own=uint16_t(a);const auto adjust=PPC_LOAD_U32(function+28),branch=PPC_LOAD_U32(function+32);
        if((adjust&0xFFFF0000)!=0x38840000 || int16_t(adjust)!=-int(own) || (branch>>26)!=18 || (branch&1))return false;
        return index<own?IndexedDescriptors(base,function+8,index,descriptor):DescriptorAt(base,BranchTarget(function+32,branch),index-own,descriptor,depth+1);
    }
    // Light's inherited scene-graph declarations precede its own table.
    constexpr std::array<uint32_t,7> prefix{0x7D8802A6,0x9181FFF8,0xFBC1FFE8,0xFBE1FFF0,0x9421FF90,0x7C7F1B78,0x7C9E2378};
    if(!Readable(base,function,104))return false;
    for(unsigned i=0;i<prefix.size();++i)if(PPC_LOAD_U32(function+i*4)!=prefix[i])return false;
    const auto count_call=PPC_LOAD_U32(function+28),descriptor_call=PPC_LOAD_U32(function+48),repeat=PPC_LOAD_U32(function+56);
    if((count_call>>26)!=18 || (count_call&3)!=1 || (descriptor_call>>26)!=18 || (descriptor_call&3)!=1 ||
       BranchTarget(function+28,count_call)!=BranchTarget(function+56,repeat) || (repeat>>26)!=18 || (repeat&3)!=1 ||
       PPC_LOAD_U32(function+32)!=0x7F1E1840 || PPC_LOAD_U32(function+36)!=0x7FE3FB78 || PPC_LOAD_U32(function+40)!=0x40980010 ||
       PPC_LOAD_U32(function+44)!=0x7FC4F378 || PPC_LOAD_U32(function+52)!=0x4800001C || PPC_LOAD_U32(function+60)!=0x7D63F050 ||
       PPC_LOAD_U32(function+68)!=0x5569103A || PPC_LOAD_U32(function+76)!=0x7C69402E ||
       PPC_LOAD_U32(function+80)!=0x38210070 || PPC_LOAD_U32(function+84)!=0x8181FFF8 || PPC_LOAD_U32(function+88)!=0x7D8803A6 ||
       PPC_LOAD_U32(function+92)!=0xEBC1FFE8 || PPC_LOAD_U32(function+96)!=0xEBE1FFF0 || PPC_LOAD_U32(function+100)!=0x4E800020)return false;
    const auto high=PPC_LOAD_U32(function+64),low=PPC_LOAD_U32(function+72);
    if((high&0xFFFF0000)!=0x3D400000 || (low&0xFFFF0000)!=0x390A0000)return false;
    uint32_t parent_count=0;if(!MetadataLeaf(base,BranchTarget(function+28,count_call),parent_count) || parent_count>4096)return false;
    if(index<parent_count)return DescriptorAt(base,BranchTarget(function+48,descriptor_call),index,descriptor,depth+1);
    const auto array=(uint32_t(uint16_t(high))<<16)+uint32_t(int32_t(int16_t(low)));
    const auto slot=uint64_t(array)+uint64_t(index-parent_count)*4;if(slot>UINT32_MAX || !Readable(base,uint32_t(slot),4))return false;
    descriptor=PPC_LOAD_U32(uint32_t(slot));return true;
}
std::string Text(uint8_t* base,uint32_t at){
    std::string result;if(!at)return result;
    for(unsigned i=0;i<256;++i){if(uint64_t(at)+i>UINT32_MAX || !Readable(base,at+i,1))return {};
        const auto ch=PPC_LOAD_U8(at+i);if(!ch)return result;if(ch<32 || ch>126)return {};result.push_back(char(ch));}
    return {};
}
}
bool ReadNativeComponentSchemas(uint8_t* base,const NativeEntityOwnership& expected,std::vector<NativeComponentSchema>& out,std::string& error){
    Host host;GuestReadBatch reads;
    try{
        uint64_t revision=0;Need(expected.lifetime_revision && SampleNativeEntityLifetimeRevision(revision) && revision==expected.lifetime_revision,"Native component schema ownership fence changed");
        std::map<std::pair<uint32_t,uint32_t>,NativeComponentSchema> classes;size_t declarations=0;
        for(const auto& entity:expected.entities)for(const auto& component:entity.components){
            const auto face=component[0];Need(face>=8 && Readable(base,face-8,12) && PPC_LOAD_U32(face)==component[1],"Native component interface was replaced");
            const auto object=face-8,primary=PPC_LOAD_U32(object);
            Need(Readable(base,primary,104),"Native primary component vtable unavailable");
            auto [it,inserted]=classes.try_emplace({primary,component[1]});auto& schema=it->second;
            if(inserted){
                Need(classes.size()<=1024,"Native component class-variant bound exceeded");
                schema.primary_table=primary;schema.interface_table=component[1];schema.example=object;
                schema.type_getter=PPC_LOAD_U32(primary+8);schema.count_getter=PPC_LOAD_U32(primary+92);schema.descriptor_getter=PPC_LOAD_U32(primary+96);
                uint32_t value=0;
                if(MetadataLeaf(base,schema.type_getter,value) && Readable(base,value,8)){schema.type=value;schema.name=Text(base,PPC_LOAD_U32(value));}
                if(MetadataLeaf(base,schema.count_getter,value) && value<=4096)schema.properties=value;
                if(schema.properties){
                    declarations+=*schema.properties;Need(declarations<=65536,"Native declaration schema exceeds bounds");
                    for(unsigned i=0;i<*schema.properties;++i){NativePropertySchema p;p.index=i;
                        if(DescriptorAt(base,schema.descriptor_getter,i,p.descriptor) && Readable(base,p.descriptor,20)){
                            p.table=PPC_LOAD_U32(p.descriptor);p.name=Text(base,PPC_LOAD_U32(p.descriptor+4));p.setter=PPC_LOAD_U32(p.descriptor+12);p.getter=PPC_LOAD_U32(p.descriptor+16);
                            if(Readable(base,p.table,28)){
                                uint32_t type=0,flag=0;
                                if(MetadataLeaf(base,PPC_LOAD_U32(p.table+20),type) && Readable(base,type,4))p.type=Text(base,PPC_LOAD_U32(type));
                                if(MetadataLeaf(base,PPC_LOAD_U32(p.table+24),flag) && flag<=1)p.array=flag!=0;
                                if(MetadataLeaf(base,PPC_LOAD_U32(p.table+8),flag) && flag<=1)p.direct=flag!=0;
                            }
                        }
                        schema.declarations.push_back(std::move(p));
                    }
                }
            }
            ++schema.instances;
        }
        NativeEntityOwnership current;const char* why=nullptr;uint32_t where=0;
        Need(ReadNativeEntityOwnership(base,current,why,where) && current==expected,"Native component inventory changed during schema discovery");
        std::vector<NativeComponentSchema> ready;ready.reserve(classes.size());for(auto& [key,value]:classes)ready.push_back(std::move(value));
        out=std::move(ready);error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
bool ReadNativeComponentPropertyCaches(uint8_t* base,const NativeEntityOwnership& expected,std::vector<NativeComponentPropertyCache>& out,std::string& error){
    Host host;GuestReadBatch reads;
    try{
        NativeEntityOwnership current;const char* why=nullptr;uint32_t where=0;
        Need(ReadNativeEntityOwnership(base,current,why,where) && current==expected,"Property cache owner changed before inventory");
        std::vector<NativeComponentPropertyCache> ready;size_t total=0;
        for(const auto& entity:expected.entities)for(const auto& component:entity.components){
            NativeComponentPropertyCache cache;cache.object=component[0]-8;
            Need(Readable(base,cache.object,48),"Property cache header unavailable");
            cache.data=PPC_LOAD_U32(cache.object+36);cache.count=PPC_LOAD_U32(cache.object+40);cache.capacity=PPC_LOAD_U32(cache.object+44);
            Need(cache.count<=cache.capacity && cache.capacity<=65536 && (total+=cache.count)<=262144,"Property cache bounds invalid");
            Need(!cache.count || Readable(base,cache.data,size_t(cache.count)*4),"Property cache allocation unavailable");
            for(uint32_t i=0;i<cache.count;++i){NativeCachedPropertyBox box;box.address=PPC_LOAD_U32(cache.data+4*i);
                Need(Readable(base,box.address,4),"Cached property box unavailable");box.table=PPC_LOAD_U32(box.address);
                Need(Readable(base,box.table,48),"Cached property box virtual table unavailable");
                auto& p=box.property;
                if(MetadataLeaf(base,PPC_LOAD_U32(box.table+44),p.descriptor) && Readable(base,p.descriptor,20)){
                    p.table=PPC_LOAD_U32(p.descriptor);p.name=Text(base,PPC_LOAD_U32(p.descriptor+4));p.setter=PPC_LOAD_U32(p.descriptor+12);p.getter=PPC_LOAD_U32(p.descriptor+16);
                    if(Readable(base,p.table,28)){uint32_t type=0,flag=0;
                        if(MetadataLeaf(base,PPC_LOAD_U32(p.table+20),type) && Readable(base,type,4))p.type=Text(base,PPC_LOAD_U32(type));
                        if(MetadataLeaf(base,PPC_LOAD_U32(p.table+24),flag) && flag<=1)p.array=flag!=0;
                        if(MetadataLeaf(base,PPC_LOAD_U32(p.table+8),flag) && flag<=1)p.direct=flag!=0;
                    }
                }
                cache.boxes.push_back(std::move(box));
            }
            ready.push_back(std::move(cache));
        }
        Need(ReadNativeEntityOwnership(base,current,why,where) && current==expected,"Property cache owner changed during inventory");
        out=std::move(ready);error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
namespace {
bool VerifyNativeMetadata(PPCContext& ctx,uint8_t* base,std::span<const NativeComponentSchema> schemas,std::string& error){
    Host host;
    struct Calls {
        PPCContext& ctx;PPCContext saved;uint8_t* base;uint32_t low=0;std::array<uint8_t,16384> stack{};
        Calls(PPCContext& c,uint8_t* b):ctx(c),saved(c),base(b){Need(c.r1.u32>=32768 && Readable(base,c.r1.u32-16384,16384),"Metadata parity needs a native owner stack");low=c.r1.u32-16384;std::memcpy(stack.data(),base+low,stack.size());}
        ~Calls(){std::memcpy(base+low,stack.data(),stack.size());ctx=saved;}
        uint32_t Invoke(uint32_t function,uint32_t object,uint32_t index=0){GuestReadBatch::CallbackBoundary boundary;ctx=saved;ctx.r1.u64=saved.r1.u32-8192;ctx.r3.u64=object;ctx.r4.u64=index;PPC_CALL_INDIRECT_FUNC(function);return ctx.r3.u32;}
    };
    try{
        Calls calls(ctx,base);unsigned types=0,counts=0,descriptors=0,flags=0;
        for(const auto& s:schemas){
            if(s.type){Need(calls.Invoke(s.type_getter,s.example)==s.type,"Native component RTTI differs from decoded metadata");++types;}
            if(s.properties){Need(calls.Invoke(s.count_getter,s.example)==*s.properties,"Native component count differs from decoded metadata");++counts;}
            for(const auto& p:s.declarations)if(p.descriptor && p.table){
                Need(calls.Invoke(s.descriptor_getter,s.example,p.index)==p.descriptor,"Native component descriptor differs from decoded metadata");++descriptors;
                if(!Readable(base,p.table,28))continue;
                for(unsigned offset:{8u,20u,24u}){const auto function=PPC_LOAD_U32(p.table+offset);uint32_t decoded=0;
                    if(MetadataLeaf(base,function,decoded)){Need(calls.Invoke(function,p.descriptor)==decoded,"Native property type/flags differ from decoded metadata");++flags;}
                }
            }
        }
        REXLOG_INFO("Native component metadata parity passed: {} RTTI, {} counts, {} descriptor indices and {} type/flag results match original compiled virtual methods; no guessed ABI or component data writes",types,counts,descriptors,flags);
        error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
}
bool ProbeNativeComponentSchemas(PPCContext& ctx,uint8_t* base,const NativeEntityOwnership& ownership){
    Host host;
    std::vector<NativeComponentSchema> schemas;std::string error;
    if(!ReadNativeComponentSchemas(base,ownership,schemas,error)){REXLOG_ERROR("Native component schema inventory rejected: {}",error);return false;}
    unsigned known_names=0,known_counts=0,known_descriptors=0;uint64_t declarations=0;std::map<std::string,unsigned> types;
    for(const auto& s:schemas){known_names+=!s.name.empty();known_counts+=s.properties.has_value();if(s.properties)declarations+=*s.properties;
        REXLOG_INFO("Native component schema: '{}' primary {:08X} interface {:08X}, {} instances, example {:08X}, RTTI {:08X}, properties {}, getters {:08X}/{:08X}/{:08X}",
            s.name,s.primary_table,s.interface_table,s.instances,s.example,s.type,s.properties?std::to_string(*s.properties):"unknown",s.type_getter,s.count_getter,s.descriptor_getter);
        const bool show=s.name=="HealthComponent" || s.name=="JPSTransformationComponent" || s.name=="BasicMotionControllerComponent" || s.name=="MickeyPlayerMotionControllerComponent" || s.name=="SplineFollowerComponent";
        for(const auto& p:s.declarations){known_descriptors+=!p.name.empty() && !p.type.empty();const auto first=++types[p.type]==1;
            if(first && !p.type.empty() && Readable(base,p.table,8))REXLOG_INFO("Native component type sample: '{}' table {:08X}, box factory {:08X}, setter {:08X}, getter {:08X}",p.type,p.table,PPC_LOAD_U32(p.table+4),p.setter,p.getter);
            if(show)REXLOG_INFO("Native component declaration: '{}' [{}] '{}' type '{}' array {} direct {}, descriptor {:08X} getters/setters {:08X}/{:08X}",
                s.name,p.index,p.name,p.type,p.array?(*p.array?"true":"false"):"unknown",p.direct?(*p.direct?"true":"false"):"unknown",p.descriptor,p.getter,p.setter);
        }
    }
    for(const auto& [type,count]:types)REXLOG_INFO("Native component declaration type: '{}' {}",type,count);
    REXLOG_INFO("Native component schema discovery: {} class variants, {} names/{} property counts decoded, {} declarations/{} named typed descriptors across classes; schema inventory only",schemas.size(),known_names,known_counts,declarations,known_descriptors);
    if(!VerifyNativeMetadata(ctx,base,schemas,error)){REXLOG_ERROR("Native component metadata parity rejected: {}",error);return false;}
    NativeComponentFieldCheckpoint fields;
    if(!fields.Capture(base,ownership,schemas,1,error) || !fields.TestRoundTrip(base,1,error)){REXLOG_ERROR("Native reflected field component probe rejected: {}",error);return false;}
    return true;
}
}
