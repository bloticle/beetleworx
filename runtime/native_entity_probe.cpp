#include "native_entity_probe.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/sha256.h>
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <immintrin.h>
#include <set>
#include <string>
#include <unordered_set>

REXCVAR_DEFINE_BOOL(em2_trace_native_entities,false,"Debug","Diagnostic native entity/position fingerprints; not full world verification");
REXCVAR_DEFINE_STRING(em2_native_entity_directory,"","Debug","Optional private binary entity snapshots, first valid tick and every 30 ticks");
namespace {
constexpr size_t max_entities=65536,max_buckets=1048576;
bool Readable(uint8_t* base,uint32_t address,size_t size){
    return base && address>=4096 && size && uint64_t(address)+size<=0x100000000ull &&
        em2::GuestReadBatch::Readable(PPC_RAW_ADDR(address),size);
}
std::string Encode(const em2::NativeEntitySnapshot& snapshot){
    std::string bytes("EM2ENT1\0",8);
    auto put=[&](uint32_t word){for(unsigned shift=32;shift;shift-=8)bytes.push_back(char(word>>(shift-8)));};
    put(uint32_t(snapshot.records.size()));
    for(const auto& record:snapshot.records){
        for(auto word:record.id)put(word);
        put(record.handle);put(record.vtable);
        bytes.append(reinterpret_cast<const char*>(record.component_mask.data()),record.component_mask.size());
        put(record.has_position?1:0);
        for(auto word:record.position_bits)put(word);
    }
    return bytes;
}
}
bool em2::ReadNativeEntitySnapshot(uint8_t* base,NativeEntitySnapshot& out,const char*& error,uint32_t& address){
    GuestReadBatch inspection;
    auto fail=[&](const char* reason,uint32_t where){error=reason;address=where;return false;};
    if(!Readable(base,0x833DA7E4,4) || !Readable(base,0x833DBC3C,1))return fail("globals",0x833DA7E4);
    NativeEntitySnapshot sample;sample.registry=PPC_LOAD_U32(0x833DA7E4);
    if(!Readable(base,sample.registry,24))return fail("registry",sample.registry);
    const auto registry=sample.registry,buckets=PPC_LOAD_U32(registry+16);
    sample.header_word_12=PPC_LOAD_U32(registry+12);sample.bucket_count=PPC_LOAD_U32(registry+20);
    if(PPC_LOAD_U32(registry+4)!=0x827B58D0 || PPC_LOAD_U32(registry+8)!=0x82A69948 ||
       !sample.bucket_count || sample.bucket_count>max_buckets)return fail("registry layout",registry);
    if(!Readable(base,buckets,size_t(sample.bucket_count)*4))return fail("buckets",buckets);
    // 827CCF40 increments the native component registration counter here.
    // Only registered IDs are meaningful. Bytes beyond this count must not
    // be interpreted as component slots or included in the fingerprint.
    if(!Readable(base,0x833DBAF0,1))return fail("component count",0x833DBAF0);
    sample.component_count=PPC_LOAD_U8(0x833DBAF0);
    const auto position_type=PPC_LOAD_U8(0x833DBC3C);
    if(!sample.component_count || position_type>=sample.component_count)return fail("component registration",0x833DBAF0);
    std::unordered_set<uint32_t> nodes,handles;
    std::set<std::array<uint32_t,4>> ids;
    for(uint32_t bucket=0;bucket<sample.bucket_count;++bucket){
        auto node=PPC_LOAD_U32(buckets+bucket*4);
        while(node){
            if(sample.records.size()>=max_entities || !nodes.insert(node).second || !Readable(base,node,12))return fail("node chain",node);
            NativeEntityRecord record;record.handle=PPC_LOAD_U32(node);
            const auto entity=PPC_LOAD_U32(node+4);
            if(!record.handle || !handles.insert(record.handle).second || (entity&3) || !Readable(base,entity,96))return fail("entity",entity);
            record.debug_address=entity;
            record.vtable=PPC_LOAD_U32(entity);
            if(record.vtable<0x82000000 || record.vtable>=0x83400000)return fail("entity vtable",entity);
            // The retail BEntity base constructor 825184B8 initializes its
            // four-word identity at +4. Derived entities retain this interface.
            for(unsigned i=0;i<4;++i)record.id[i]=PPC_LOAD_U32(entity+4+i*4);
            if(std::all_of(record.id.begin(),record.id.end(),[](auto v){return !v;}) || !ids.insert(record.id).second)return fail("entity identity",entity);
            const auto types=PPC_LOAD_U32(entity+88),components=PPC_LOAD_U32(entity+36);
            if(!Readable(base,types,sample.component_count))return fail("component types",entity+88);
            for(unsigned type=0;type<sample.component_count;++type)if(PPC_LOAD_U8(types+type)!=255)record.component_mask[type/8]|=uint8_t(1u<<(type%8));
            const auto slot=PPC_LOAD_U8(types+position_type);
            if(slot!=255){
                if(!Readable(base,components,unsigned(slot)*4+4))return fail("position slot",components);
                const auto entry=PPC_LOAD_U32(components+unsigned(slot)*4);
                if(entry<8 || !Readable(base,entry-8,184))return fail("position component",entry);
                record.has_position=true;
                for(unsigned axis=0;axis<3;++axis){
                    const auto bits=PPC_LOAD_U32(entry-8+172+axis*4);
                    if((bits&0x7f800000)==0x7f800000)return fail("nonfinite position",entry-8+172+axis*4);
                    record.position_bits[axis]=bits;
                }
            }
            sample.records.push_back(record);node=PPC_LOAD_U32(node+8);
        }
    }
    if(PPC_LOAD_U32(0x833DA7E4)!=registry || PPC_LOAD_U32(registry+12)!=sample.header_word_12 ||
       PPC_LOAD_U32(registry+16)!=buckets || PPC_LOAD_U32(registry+20)!=sample.bucket_count || PPC_LOAD_U8(0x833DBAF0)!=sample.component_count)return fail("registry changed",registry);
    std::sort(sample.records.begin(),sample.records.end(),[](const auto& a,const auto& b){return a.id<b.id;});
    out=std::move(sample);error=nullptr;address=0;return true;
}
void em2::TraceNativeEntitySnapshot(uint8_t* base,uint64_t tick){
    if(!REXCVAR_GET(em2_trace_native_entities))return;
    const auto start=std::chrono::steady_clock::now();NativeEntitySnapshot snapshot;const char* error=nullptr;uint32_t address=0;
    if(!ReadNativeEntitySnapshot(base,snapshot,error,address)){
        REXLOG_INFO("Native entities v1: tick {}, invalid '{}' at {:08X}",tick,error?error:"unknown",address);return;
    }
    const auto bytes=Encode(snapshot),digest=rex::util::sha256(bytes);
    const auto positioned=std::count_if(snapshot.records.begin(),snapshot.records.end(),[](const auto& record){return record.has_position;});
    const auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
    REXLOG_INFO("Native entities v1: tick {}, count {}, header12 {:08X}, components {}, positioned {}, buckets {}, us {}, digest {}",tick,snapshot.records.size(),snapshot.header_word_12,snapshot.component_count,positioned,snapshot.bucket_count,us,digest);
    // Binary samples are optional diagnostics. Never write into the normal
    // save/profile directory implicitly, or log every entity through the logger.
    const auto directory=REXCVAR_GET(em2_native_entity_directory);if(directory.empty())return;
    thread_local bool recorded=false;thread_local uint64_t previous=UINT64_MAX;thread_local uint32_t previous_registry=0;
    if(tick<previous || previous_registry!=snapshot.registry)recorded=false;
    previous=tick;previous_registry=snapshot.registry;
    if(recorded && tick%30)return;
    try{
        const auto path=std::filesystem::path(directory);std::filesystem::create_directories(path);
        std::ofstream file(path/(std::to_string(tick)+".bin"),std::ios::binary);file.write(bytes.data(),bytes.size());
        if(!file)throw std::runtime_error("entity snapshot write failed");
        if(!recorded){
            GuestReadBatch names_inspection;
            auto name=[&](uint32_t pointer){std::string text;for(unsigned i=0;i<128;++i){if(uint64_t(pointer)+i>UINT32_MAX || !Readable(base,pointer+i,1))return std::string{};const auto c=PPC_LOAD_U8(pointer+i);if(!c)return text;if(c<32 || c>=127)return std::string{};text.push_back(char(c));}return std::string{};};
            std::ofstream labels(path/"names.tsv");labels<<"id\thandle\taddress\tname\n";
            for(const auto& record:snapshot.records){std::string id;for(auto word:record.id)id+=fmt::format("{:08X}",word);const auto field=record.debug_address+20;labels<<id<<'\t'<<record.handle<<'\t'<<fmt::format("{:08X}",record.debug_address)<<'\t'<<(Readable(base,field,4)?name(PPC_LOAD_U32(field)):std::string{})<<'\n';}
            std::ofstream types_file(path/"types.tsv");types_file<<"id\tname\n";
            if(Readable(base,0x833DBAF4,4)){
                const auto registry=PPC_LOAD_U32(0x833DBAF4);
                if(Readable(base,registry,24)){
                    const auto buckets=PPC_LOAD_U32(registry+16),count=PPC_LOAD_U32(registry+20);
                    if(count && count<=4096 && Readable(base,buckets,size_t(count)*4)){
                        std::unordered_set<uint32_t> visited;
                        for(unsigned bucket=0;bucket<count;++bucket){auto node=PPC_LOAD_U32(buckets+bucket*4);
                            while(node && visited.size()<1024 && visited.insert(node).second && Readable(base,node,12)){types_file<<unsigned(PPC_LOAD_U8(node+4))<<'\t'<<name(PPC_LOAD_U32(node))<<'\n';node=PPC_LOAD_U32(node+8);}
                        }
                    }
                }
            }
        }
        recorded=true;
    }catch(const std::exception& ex){REXLOG_WARN("Native entity diagnostic write failed: {}",ex.what());}
}
bool em2::TestNativeEntityProbe(){
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));if(!base)return false;
    struct Release{void* p;unsigned csr=_mm_getcsr();~Release(){_mm_setcsr(csr);VirtualFree(p,0,MEM_RELEASE);}}release{base};
    if(!VirtualAlloc(base,0x10000,MEM_COMMIT,PAGE_READWRITE) || !VirtualAlloc(base+0x833D0000,0x10000,MEM_COMMIT,PAGE_READWRITE))return false;
    PPC_STORE_U32(0x833DA7E4,0x2000);PPC_STORE_U8(0x833DBC3C,7);PPC_STORE_U8(0x833DBAF0,8);
    PPC_STORE_U32(0x2004,0x827B58D0);PPC_STORE_U32(0x2008,0x82A69948);PPC_STORE_U32(0x200c,2);PPC_STORE_U32(0x2010,0x2800);PPC_STORE_U32(0x2014,1);PPC_STORE_U32(0x2800,0x3000);
    for(unsigned i=0;i<2;++i){
        const auto node=0x3000+i*16,entity=0x4000+i*0x1000,types=0x6000+i*0x100;
        PPC_STORE_U32(node,10+i);PPC_STORE_U32(node+4,entity);PPC_STORE_U32(node+8,i?0:0x3010);
        PPC_STORE_U32(entity,0x8208D908);for(unsigned j=0;j<4;++j)PPC_STORE_U32(entity+4+j*4,1+j+i*4);
        PPC_STORE_U32(entity+88,types);std::memset(base+types,255,256);
    }
    PPC_STORE_U8(0x6007,0);PPC_STORE_U32(0x4024,0x7000);PPC_STORE_U32(0x7000,0x8008);
    for(unsigned i=0;i<3;++i)PPC_STORE_U32(0x8000+172+i*4,0x3f800000+i*0x800000);
    auto check=[](bool ok,const char* what){if(!ok)REXLOG_ERROR("Native entity probe: {}",what);return ok;};
    const auto csr=_mm_getcsr();NativeEntitySnapshot first;const char* error=nullptr;uint32_t address=0;
    std::vector<uint8_t> original(base,base+0x10000);
    if(!check(ReadNativeEntitySnapshot(base,first,error,address) && first.records.size()==2 && first.records[0].has_position && !first.records[1].has_position && _mm_getcsr()==csr && !std::memcmp(base,original.data(),original.size()),"bounded read preserves memory/FP and records position presence"))return false;
    const auto encoded=Encode(first);if(!check(encoded.size()==12+72*2 && encoded.substr(0,7)=="EM2ENT1","versioned big-endian record width"))return false;
    // Unregistered bytes must not masquerade as real component membership.
    PPC_STORE_U8(0x6000+200,0);PPC_STORE_U8(0x6100+200,0);
    NativeEntitySnapshot bounded;if(!check(ReadNativeEntitySnapshot(base,bounded,error,address) && Encode(bounded)==encoded,"unregistered allocator bytes do not affect membership"))return false;
    // A valid short table can end immediately before an inaccessible page.
    std::memcpy(base+0x9ff8,base+0x6000,8);PPC_STORE_U32(0x4058,0x9ff8);
    DWORD boundary_old=0;if(!VirtualProtect(base+0xa000,4096,PAGE_NOACCESS,&boundary_old))return false;
    const bool boundary_ok=ReadNativeEntitySnapshot(base,bounded,error,address) && Encode(bounded)==encoded;
    DWORD boundary_ignore=0;VirtualProtect(base+0xa000,4096,boundary_old,&boundary_ignore);PPC_STORE_U32(0x4058,0x6000);
    if(!check(boundary_ok,"registered span at a real guard boundary remains readable"))return false;

    PPC_STORE_U32(0x2800,0x3010);PPC_STORE_U32(0x3018,0x3000);PPC_STORE_U32(0x3008,0);
    NativeEntitySnapshot reordered;if(!check(ReadNativeEntitySnapshot(base,reordered,error,address) && Encode(reordered)==encoded,"bucket/node iteration order does not affect canonical content"))return false;
    PPC_STORE_U32(0x3008,0x3010);
    if(!check(!ReadNativeEntitySnapshot(base,reordered,error,address) && Encode(reordered)==encoded,"cycles reject without partial publication"))return false;PPC_STORE_U32(0x3008,0);
    PPC_STORE_U32(0x8000+172,0x7f800001);
    if(!check(!ReadNativeEntitySnapshot(base,reordered,error,address) && _mm_getcsr()==csr && Encode(reordered)==encoded,"signaling NaNs reject without floating exceptions or partial publication"))return false;PPC_STORE_U32(0x8000+172,0x3f800000);
    DWORD old=0;if(!VirtualProtect(base+0x6000,4096,PAGE_NOACCESS,&old))return false;
    const bool inaccessible=!ReadNativeEntitySnapshot(base,reordered,error,address) && Encode(reordered)==encoded;
    DWORD ignored=0;VirtualProtect(base+0x6000,4096,old,&ignored);if(!check(inaccessible,"protected type memory rejects safely"))return false;
    REXLOG_INFO("Native entity probe tests passed: canonical order, field presence, immutable reads, real protection, cycles, NaNs and no partial publication");return true;
}
