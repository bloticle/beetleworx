#include "pc_shader_library.h"
#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <xxhash.h>
#include <windows.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <unordered_map>
#include <stdexcept>
#include <cstdio>
#include <algorithm>
#include <chrono>

REXCVAR_DECLARE(bool,dxbc_switch);
REXCVAR_DECLARE(bool,em2_profile_world_memory);
REXCVAR_DEFINE_BOOL(em2_pretranslated_shaders,true,"GPU","Reuse validated PC shader bytecode and resource bindings across renderer rebuilds and runs");
REXCVAR_DEFINE_BOOL(em2_skip_packed_shader_duplicates,true,"GPU","Skip loose shader records already represented in a validated shader pack");
REXCVAR_DEFINE_BOOL(em2_mapped_shader_packs,true,"GPU","Keep validated packed PC shaders in read-only mapped storage and decode requested records on demand");
extern "C" bool EM2OriginalTranslateShader(rex::graphics::ShaderTranslator*,rex::graphics::Shader::Translation*);
namespace {
using D=rex::graphics::DxbcShader;using T=rex::graphics::DxbcShaderTranslator;
using Translation=rex::graphics::Shader::Translation;
template<class Tag,auto Member>struct Access{friend auto Field(Tag){return Member;}};
#define EM2_SHADER_FIELD(tag,type,member) struct tag{friend auto Field(tag);};template struct Access<tag,&type::member>;
EM2_SHADER_FIELD(Vendor,T,vendor_id_)
EM2_SHADER_FIELD(Bindless,T,bindless_resources_used_)
EM2_SHADER_FIELD(Rov,T,edram_rov_used_)
EM2_SHADER_FIELD(Gamma,T,gamma_render_target_as_srgb_)
EM2_SHADER_FIELD(Msaa,T,msaa_2x_supported_)
EM2_SHADER_FIELD(ScaleX,T,draw_resolution_scale_x_)
EM2_SHADER_FIELD(ScaleY,T,draw_resolution_scale_y_)
EM2_SHADER_FIELD(SourceMap,T,emit_source_map_)
EM2_SHADER_FIELD(Binary,Translation,translated_binary_)
EM2_SHADER_FIELD(Valid,Translation,is_valid_)
EM2_SHADER_FIELD(Translated,Translation,is_translated_)
EM2_SHADER_FIELD(Errors,Translation,errors_)
EM2_SHADER_FIELD(Textures,D,texture_bindings_)
EM2_SHADER_FIELD(Samplers,D,sampler_bindings_)
EM2_SHADER_FIELD(Used,D,used_texture_mask_)
EM2_SHADER_FIELD(Setup,D,bindings_setup_entered_)
#undef EM2_SHADER_FIELD
constexpr size_t max_record=8*1024*1024,max_library=256*1024*1024,max_queue=32*1024*1024,max_entries=16384;
struct Counters {std::atomic<uint64_t>hits{0},misses{0},loaded{0},rejected{0},saved{0},dropped{0},file_reads{0},packed_duplicates_skipped{0};} counters;
std::array<std::mutex,64> shader_locks;
std::mutex default_mutex;std::weak_ptr<em2::PcShaderLibrary> default_library;
thread_local std::optional<std::shared_ptr<em2::PcShaderLibrary>> local_library;
using Bytes=std::vector<uint8_t>;
void U32(Bytes& out,uint32_t value){for(unsigned i=0;i<4;++i)out.push_back(uint8_t(value>>(8*i)));}
void U64(Bytes& out,uint64_t value){U32(out,uint32_t(value));U32(out,uint32_t(value>>32));}
struct Reader {
    std::span<const uint8_t> bytes;size_t at=0;
    uint32_t Word(){if(at>bytes.size() || bytes.size()-at<4)throw std::runtime_error("truncated shader record");uint32_t n=0;for(unsigned i=0;i<4;++i)n|=uint32_t(bytes[at++])<<(8*i);return n;}
    uint64_t Wide(){uint64_t lo=Word();return lo|(uint64_t(Word())<<32);}
    Bytes Block(){auto n=Word();if(n>max_record || at>bytes.size() || n>bytes.size()-at)throw std::runtime_error("invalid shader block");Bytes b(bytes.begin()+at,bytes.begin()+at+n);at+=n;return b;}
};
void Block(Bytes& out,std::span<const uint8_t> bytes){U32(out,uint32_t(bytes.size()));out.insert(out.end(),bytes.begin(),bytes.end());}
bool Dxbc(std::span<const uint8_t> bytes){
    if(bytes.size()<32 || bytes.size()>max_record || std::memcmp(bytes.data(),"DXBC",4))return false;
    Reader r{bytes};r.at=20;if(r.Word()!=1 || r.Word()!=bytes.size())return false;
    auto chunks=r.Word();if(chunks>64 || chunks>(bytes.size()-32)/4)return false;
    for(uint32_t i=0;i<chunks;++i){r.at=32+4*i;auto offset=r.Word();if(offset<32+4*chunks || offset>bytes.size()-8)return false;r.at=offset+4;if(r.Word()>bytes.size()-offset-8)return false;}
    return true;
}
Bytes Key(T& translator,Translation& translation){
    const auto& shader=translation.shader();Bytes out;out.reserve(48+shader.ucode_data().size()*4);
    U32(out,1);U32(out,uint32_t(shader.type()));U64(out,shader.ucode_data_hash());U64(out,translation.modification());
    U32(out,uint32_t(translator.*Field(Vendor{})));
    U32(out,uint32_t(translator.*Field(Bindless{}))|(uint32_t(translator.*Field(Rov{}))<<1)|(uint32_t(translator.*Field(Gamma{}))<<2)|(uint32_t(translator.*Field(Msaa{}))<<3)|(uint32_t(translator.*Field(SourceMap{}))<<4)|(uint32_t(REXCVAR_GET(dxbc_switch))<<5));
    U32(out,translator.*Field(ScaleX{}));U32(out,translator.*Field(ScaleY{}));
    U32(out,uint32_t(shader.ucode_data().size()));for(auto word:shader.ucode_data())U32(out,word);
    if(out.size()>1024*1024)throw std::runtime_error("oversized shader key");return out;
}
struct Record {Bytes key,binary;std::vector<D::TextureBinding> textures;std::vector<D::SamplerBinding> samplers;uint32_t used=0;
    size_t size()const{return key.size()+binary.size()+textures.size()*sizeof(D::TextureBinding)+samplers.size()*sizeof(D::SamplerBinding);}};
struct MappedPack {
    HANDLE file=INVALID_HANDLE_VALUE,mapping=nullptr;const uint8_t* data=nullptr;size_t size=0;
    MappedPack()=default;MappedPack(const MappedPack&)=delete;MappedPack& operator=(const MappedPack&)=delete;
    ~MappedPack(){if(data)UnmapViewOfFile(data);if(mapping)CloseHandle(mapping);if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);}
    static std::shared_ptr<MappedPack> Open(const std::filesystem::path& path){
        auto pack=std::make_shared<MappedPack>();
        // Exclude in-place writes/truncation while mapped. Atomic replacement
        // may publish a new file; this handle retains its validated old bytes.
        pack->file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        LARGE_INTEGER size{};
        if(pack->file==INVALID_HANDLE_VALUE || !GetFileSizeEx(pack->file,&size) || size.QuadPart<12 || uint64_t(size.QuadPart)>max_library+max_entries*128)throw std::runtime_error("shader pack extent/open invalid");
        pack->size=size_t(size.QuadPart);pack->mapping=CreateFileMappingW(pack->file,nullptr,PAGE_READONLY,0,0,nullptr);
        if(!pack->mapping)throw std::runtime_error("shader pack mapping failed");
        pack->data=static_cast<const uint8_t*>(MapViewOfFile(pack->mapping,FILE_MAP_READ,0,0,0));
        if(!pack->data)throw std::runtime_error("shader pack view failed");
        return pack;
    }
    std::span<const uint8_t> BytesAt(size_t offset,size_t count)const{if(offset>size || count>size-offset)throw std::runtime_error("shader pack slice invalid");return {data+offset,count};}
};
bool ReplacePack(const std::filesystem::path& temp,const std::filesystem::path& path){
    // Unlike MoveFileEx(REPLACE_EXISTING), ReplaceFile keeps mapped readers of
    // the old file valid on Windows. A new destination needs a plain move.
    if(ReplaceFileW(path.c_str(),temp.c_str(),nullptr,0,nullptr,nullptr))return true;
    const auto error=GetLastError();if(error!=ERROR_FILE_NOT_FOUND && error!=ERROR_PATH_NOT_FOUND)return false;
    // Do not overwrite a destination published by another writer in the gap.
    return MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE;
}
Bytes Encode(const Record& record){
    Bytes out;U64(out,0x3143424458324D45ull);Block(out,record.key);Block(out,record.binary);U32(out,uint32_t(record.textures.size()));
    for(const auto& t:record.textures){U32(out,t.bindless_descriptor_index);U32(out,t.fetch_constant);U32(out,uint32_t(t.dimension));U32(out,uint32_t(t.is_signed));}
    U32(out,uint32_t(record.samplers.size()));for(const auto& s:record.samplers){U32(out,s.bindless_descriptor_index);U32(out,s.fetch_constant);U32(out,uint32_t(s.mag_filter));U32(out,uint32_t(s.min_filter));U32(out,uint32_t(s.mip_filter));U32(out,uint32_t(s.aniso_filter));}
    U32(out,record.used);U64(out,XXH3_64bits(out.data(),out.size()));return out;
}
std::shared_ptr<Record> Decode(std::span<const uint8_t> bytes){
    if(bytes.size()<64 || bytes.size()>max_record)throw std::runtime_error("invalid shader record size");
    Reader checksum{bytes};checksum.at=bytes.size()-8;const auto expected=checksum.Wide();if(expected!=XXH3_64bits(bytes.data(),bytes.size()-8))throw std::runtime_error("shader checksum mismatch");
    Reader in{bytes.first(bytes.size()-8)};if(in.Wide()!=0x3143424458324D45ull)throw std::runtime_error("shader record version mismatch");
    auto record=std::make_shared<Record>();record->key=in.Block();record->binary=in.Block();
    Reader key{record->key};if(key.Word()!=1 || key.Word()>1)throw std::runtime_error("shader key version/stage mismatch");key.Wide();key.Wide();key.Word();if(key.Word()>63)throw std::runtime_error("shader flags invalid");const auto sx=key.Word(),sy=key.Word(),words=key.Word();
    if(!sx || !sy || sx>16 || sy>16 || !words || words>262144 || record->key.size()!=44+size_t(words)*4 || !Dxbc(record->binary))throw std::runtime_error("shader key/binary invalid");
    const auto textures=in.Word();if(textures>D::kMaxTextureBindings)throw std::runtime_error("shader texture count invalid");
    uint32_t used=0;for(uint32_t i=0;i<textures;++i){D::TextureBinding t;std::memset(&t,0,sizeof(t));t.bindless_descriptor_index=in.Word();t.fetch_constant=in.Word();auto dimension=in.Word(),signedness=in.Word();if(t.bindless_descriptor_index>=16384 || t.fetch_constant>=32 || dimension>3 || signedness>1)throw std::runtime_error("shader texture invalid");t.dimension=decltype(t.dimension)(dimension);t.is_signed=bool(signedness);used|=1u<<t.fetch_constant;record->textures.push_back(t);}
    const auto samplers=in.Word();if(samplers>D::kMaxSamplerBindings)throw std::runtime_error("shader sampler count invalid");
    for(uint32_t i=0;i<samplers;++i){D::SamplerBinding s{};s.bindless_descriptor_index=in.Word();s.fetch_constant=in.Word();auto mag=in.Word(),min=in.Word(),mip=in.Word(),aniso=in.Word();if(s.bindless_descriptor_index>=16384 || s.fetch_constant>=32 || mag>3 || min>3 || mip>3 || aniso>7)throw std::runtime_error("shader sampler invalid");s.mag_filter=decltype(s.mag_filter)(mag);s.min_filter=decltype(s.min_filter)(min);s.mip_filter=decltype(s.mip_filter)(mip);s.aniso_filter=decltype(s.aniso_filter)(aniso);record->samplers.push_back(s);}
    record->used=in.Word();if(record->used!=used || in.at!=in.bytes.size())throw std::runtime_error("shader metadata extent mismatch");return record;
}
std::shared_ptr<Record> Capture(Bytes key,Translation& translation,D& shader){
    if(!translation.is_valid() || !translation.is_translated() || !translation.errors().empty() || !Dxbc(translation.translated_binary()))return {};
    auto record=std::make_shared<Record>();record->key=std::move(key);record->binary=translation.translated_binary();record->textures=shader.GetTextureBindingsAfterTranslation();record->samplers=shader.GetSamplerBindingsAfterTranslation();record->used=shader.GetUsedTextureMaskAfterTranslation();return record;
}
bool Restore(const Record& record,Translation& translation,D& shader){
    // Prepare every allocation before publishing any fields or the native once
    // flag. The lock also serializes native translations of this shader object.
    auto binary=record.binary;auto textures=record.textures;auto samplers=record.samplers;
    if(!((shader.*Field(Setup{})).test_and_set(std::memory_order_relaxed))){
        (shader.*Field(Textures{})).swap(textures);(shader.*Field(Samplers{})).swap(samplers);shader.*Field(Used{})=record.used;
    }else{
        const auto& current=shader.GetTextureBindingsAfterTranslation();const auto& samples=shader.GetSamplerBindingsAfterTranslation();
        if(current.size()!=textures.size() || samples.size()!=samplers.size() || shader.GetUsedTextureMaskAfterTranslation()!=record.used)return false;
        // Stable native texture layout includes zeroed padding; sampler fields
        // are compared individually because their padding is not specified.
        if(!current.empty() && std::memcmp(current.data(),textures.data(),current.size()*sizeof(current[0])))return false;
        for(size_t i=0;i<samples.size();++i){const auto&a=samples[i];const auto&b=samplers[i];if(a.bindless_descriptor_index!=b.bindless_descriptor_index || a.fetch_constant!=b.fetch_constant || a.mag_filter!=b.mag_filter || a.min_filter!=b.min_filter || a.mip_filter!=b.mip_filter || a.aniso_filter!=b.aniso_filter)return false;}
    }
    (translation.*Field(Binary{})).swap(binary);(translation.*Field(Errors{})).clear();translation.set_host_disassembly({});translation.*Field(Valid{})=true;translation.*Field(Translated{})=true;return true;
}
}
class em2::PcShaderLibrary {
    friend bool em2::TestPcShaderLibraryLoad(const std::filesystem::path&,const std::filesystem::path&);
    std::filesystem::path root_;std::mutex mutex_;std::condition_variable changed_;bool closing_=false,busy_=false;
    struct Entry {
        std::shared_ptr<const Record> decoded;std::shared_ptr<const MappedPack> pack;size_t offset=0,size=0;Bytes key;
        const Bytes& Key()const{return decoded?decoded->key:key;}
        std::shared_ptr<const Record> Get(){
            if(!decoded){auto record=Decode(pack->BytesAt(offset,size));if(record->key!=key)throw std::runtime_error("shader pack index key changed");decoded=std::move(record);Bytes{}.swap(key);pack.reset();}
            return decoded;
        }
    };
    std::unordered_multimap<uint64_t,Entry> records_;size_t bytes_=0,queued_=0;
    std::deque<std::shared_ptr<const Record>> queue_;std::thread writer_;
    void Write(){
        for(;;){std::shared_ptr<const Record> record;
            {std::unique_lock lock(mutex_);changed_.wait(lock,[&]{return closing_ || !queue_.empty();});if(queue_.empty() && closing_)return;record=queue_.front();queue_.pop_front();queued_-=record->size();busy_=true;}
            try {const auto hash=XXH3_64bits(record->key.data(),record->key.size());char name[32];std::snprintf(name,sizeof(name),"%016llx.em2dxbc",static_cast<unsigned long long>(hash));auto path=root_/name,temp=path;temp+=L".tmp";auto bytes=Encode(*record);
                std::ofstream file(temp,std::ios::binary|std::ios::trunc);file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());file.flush();const bool valid=bool(file);file.close();
                if(!valid || !MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("shader library write failed");++counters.saved;
            }catch(...){++counters.dropped;}
            {std::lock_guard lock(mutex_);busy_=false;}changed_.notify_all();
        }
    }
    bool LoadPack(const std::filesystem::path& path){
        if(!std::filesystem::exists(path))return false;
        try{
            if(REXCVAR_GET(em2_mapped_shader_packs)){
                auto pack=MappedPack::Open(path);if(std::memcmp(pack->data,"EM2DXPK1",8))throw std::runtime_error("shader pack header invalid");
                Reader reader{{pack->data,pack->size}};reader.at=8;const auto count=reader.Word();if(count>max_entries)throw std::runtime_error("shader pack count invalid");
                for(uint32_t i=0;i<count;++i){const auto size=reader.Word();const auto offset=reader.at;auto record=Decode(pack->BytesAt(offset,size));reader.at+=size;if(Add(record,false,pack,offset,size))++counters.loaded;}
                if(reader.at!=pack->size)throw std::runtime_error("shader pack trailing data");return true;
            }
            if(std::filesystem::file_size(path)>max_library+max_entries*128)throw std::runtime_error("shader pack exceeds budget");
            std::ifstream file(path,std::ios::binary);char magic[8];if(!file.read(magic,8) || std::memcmp(magic,"EM2DXPK1",8))throw std::runtime_error("shader pack header invalid");
            auto word=[&](){std::array<uint8_t,4> data;if(!file.read(reinterpret_cast<char*>(data.data()),4))throw std::runtime_error("shader pack truncated");Reader r{data};return r.Word();};
            const auto count=word();if(count>max_entries)throw std::runtime_error("shader pack count invalid");
            for(uint32_t i=0;i<count;++i){auto size=word();if(size<64 || size>max_record)throw std::runtime_error("shader pack record size invalid");Bytes bytes(size);if(!file.read(reinterpret_cast<char*>(bytes.data()),size))throw std::runtime_error("shader pack record truncated");if(Add(Decode(bytes),false))++counters.loaded;}
            if(file.peek()!=std::char_traits<char>::eof())throw std::runtime_error("shader pack trailing data");return true;
        }catch(...){++counters.rejected;return false;}
    }
    void Load(const std::filesystem::path& directory,bool seed=false){
        if(!std::filesystem::is_directory(directory))return;size_t scanned=0;
        const bool packed=LoadPack(directory/"library.em2dxbcpack");
        if(packed && seed)return;
        for(const auto& entry:std::filesystem::directory_iterator(directory)){
            if(entry.path().extension()!=L".em2dxbc")continue;if(++scanned>max_entries || bytes_>=max_library)break;
            // Persisted filenames are the hash of the full translation key.
            // A validated pack already has precedence for the same key. Read
            // unknown filenames normally so new runtime variants still load.
            if(packed && REXCVAR_GET(em2_skip_packed_shader_duplicates)){
                const auto name=entry.path().stem().native();uint64_t hash=0;bool valid=name.size()==16;
                if(valid)for(auto character:name){
                    unsigned value;
                    if(character>=L'0' && character<=L'9')value=unsigned(character-L'0');
                    else if(character>=L'a' && character<=L'f')value=unsigned(character-L'a')+10;
                    else if(character>=L'A' && character<=L'F')value=unsigned(character-L'A')+10;
                    else{valid=false;break;}
                    hash=(hash<<4)|value;
                }
                if(valid && records_.contains(hash)){++counters.packed_duplicates_skipped;continue;}
            }
            try{auto size=entry.file_size();if(size>max_record || size<64)throw std::runtime_error("shader file size invalid");Bytes bytes(size);++counters.file_reads;std::ifstream file(entry.path(),std::ios::binary);if(!file.read(reinterpret_cast<char*>(bytes.data()),size))throw std::runtime_error("shader file read failed");auto record=Decode(bytes);if(Add(record,false))++counters.loaded;}catch(...){++counters.rejected;}
        }
    }
public:
    explicit PcShaderLibrary(const std::filesystem::path& root,const std::filesystem::path& seed):root_(root/"translated-dxbc-v1"){
        std::filesystem::create_directories(root_);Load(root_);if(!seed.empty())Load(seed/"translated-dxbc-v1",true);
        if(REXCVAR_GET(em2_profile_world_memory)){uint64_t resident=0,decoded=0,keys=0;for(const auto& [hash,entry]:records_){if(entry.decoded){++resident;decoded+=entry.decoded->size();}keys+=entry.key.capacity();}REXLOG_INFO("PC shader library residency: available records {} decoded records {} decoded logical bytes {} indexed key capacity {} mapped storage {}; indexed records validated and available on demand",records_.size(),resident,decoded,keys,REXCVAR_GET(em2_mapped_shader_packs));}
        writer_=std::thread([this]{Write();});
    }
    ~PcShaderLibrary(){ {std::lock_guard lock(mutex_);closing_=true;}changed_.notify_all();if(writer_.joinable())writer_.join();}
    void Flush(){std::unique_lock lock(mutex_);changed_.wait(lock,[&]{return queue_.empty() && !busy_;});}
    bool SavePack(const std::filesystem::path& output){
        try{Flush();std::vector<Entry> records;
            {std::lock_guard lock(mutex_);for(const auto& [hash,record]:records_)records.push_back(record);}
            std::sort(records.begin(),records.end(),[](const auto&a,const auto&b){return a.Key()<b.Key();});
            const auto directory=output/"translated-dxbc-v1";std::filesystem::create_directories(directory);auto path=directory/"library.em2dxbcpack",temp=path;temp+=L".tmp";
            std::ofstream file(temp,std::ios::binary|std::ios::trunc);file.write("EM2DXPK1",8);
            auto word=[&](uint32_t n){Bytes b;U32(b,n);file.write(reinterpret_cast<const char*>(b.data()),4);};word(uint32_t(records.size()));
            for(const auto& record:records){Bytes encoded;std::span<const uint8_t> bytes;if(record.decoded){encoded=Encode(*record.decoded);bytes=encoded;}else bytes=record.pack->BytesAt(record.offset,record.size);if(bytes.size()>max_record)return false;word(uint32_t(bytes.size()));file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
            file.flush();file.close();return bool(file) && ReplacePack(temp,path);
        }catch(...){return false;}
    }
    std::shared_ptr<const Record> Find(const Bytes& key){std::lock_guard lock(mutex_);auto [first,last]=records_.equal_range(XXH3_64bits(key.data(),key.size()));for(;first!=last;++first)if(first->second.Key()==key)return first->second.Get();return {};}
    bool Add(std::shared_ptr<const Record> record,bool persist,std::shared_ptr<const MappedPack> pack={},size_t offset=0,size_t size=0){
        std::lock_guard lock(mutex_);const auto hash=XXH3_64bits(record->key.data(),record->key.size());auto [first,last]=records_.equal_range(hash);for(;first!=last;++first)if(first->second.Key()==record->key)return false;
        if(records_.size()>=max_entries || record->size()>max_library-bytes_){++counters.dropped;return false;}
        records_.emplace(hash,Entry{pack?std::shared_ptr<const Record>{}:record,pack,offset,size,pack?record->key:Bytes{}});bytes_+=record->size();
        if(persist){if(record->size()<=max_queue-queued_){queue_.push_back(std::move(record));queued_+=queue_.back()->size();changed_.notify_one();}else ++counters.dropped;}return true;
    }
};
std::shared_ptr<em2::PcShaderLibrary> em2::OpenPcShaderLibrary(const std::filesystem::path& root,const std::filesystem::path& seed){return std::make_shared<PcShaderLibrary>(root,seed);}
void em2::UsePcShaderLibrary(const std::shared_ptr<PcShaderLibrary>& library){std::lock_guard lock(default_mutex);default_library=library;}
void em2::FlushPcShaderLibrary(const std::shared_ptr<PcShaderLibrary>& library){if(library)library->Flush();}
bool em2::SavePcShaderLibraryPack(const std::shared_ptr<PcShaderLibrary>& library,const std::filesystem::path& output){return library && library->SavePack(output);}
em2::PcShaderLibraryStats em2::GetPcShaderLibraryStats(){return {counters.hits.load(),counters.misses.load(),counters.loaded.load(),counters.rejected.load(),counters.saved.load(),counters.dropped.load(),counters.file_reads.load(),counters.packed_duplicates_skipped.load()};}
bool em2::TestPcShaderLibraryLoad(const std::filesystem::path& source,const std::filesystem::path& output){
    if(!std::filesystem::is_regular_file(source/"translated-dxbc-v1"/"library.em2dxbcpack"))return false;
    struct Restore {bool previous=REXCVAR_GET(em2_skip_packed_shader_duplicates),mapped=REXCVAR_GET(em2_mapped_shader_packs);~Restore(){REXCVAR_SET(em2_skip_packed_shader_duplicates,previous);REXCVAR_SET(em2_mapped_shader_packs,mapped);}} restore;
    std::filesystem::create_directories(output);std::ofstream report(output/"shader-load-results.json");
    report<<"{\"scope\":\"Same-process cache-load comparison; all decoded record bytes compared exactly. OS cache order can affect timing. No gameplay FPS claim.\",\"phases\":[";
    std::vector<Bytes> expected;unsigned phase=0;bool success=true;
    for(unsigned configuration=0;configuration<4;++configuration){
        const bool skip=(configuration&2)!=0,mapped=(configuration&1)!=0;
        REXCVAR_SET(em2_skip_packed_shader_duplicates,skip);
        REXCVAR_SET(em2_mapped_shader_packs,mapped);
        REXLOG_INFO("PC shader load comparison phase {}: skip packed duplicates {} mapped storage {}",phase,skip,mapped);
        const auto before=GetPcShaderLibraryStats();const auto started=std::chrono::steady_clock::now();
        auto library=OpenPcShaderLibrary(source);
        const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        const auto after=GetPcShaderLibraryStats();std::vector<Bytes> records;
        size_t resident=0;
        {std::lock_guard lock(library->mutex_);for(auto& [hash,record]:library->records_){resident+=bool(record.decoded);records.push_back(Encode(*record.Get()));}}
        std::sort(records.begin(),records.end());
        const bool identical=!phase || records==expected;
        if(!phase)expected=std::move(records);
        success=success && identical && !expected.empty();
        if(phase)report<<',';
        report<<"{\"skip_duplicates\":"<<(skip?"true":"false")<<",\"mapped\":"<<(mapped?"true":"false")<<",\"milliseconds\":"<<ms<<",\"resident_records_before_comparison\":"<<resident<<",\"record_count\":"<<expected.size()<<",\"loose_file_reads\":"<<after.file_reads-before.file_reads<<",\"duplicates_skipped\":"<<after.packed_duplicates_skipped-before.packed_duplicates_skipped<<",\"identical\":"<<(identical?"true":"false")<<'}';report.flush();
        REXLOG_INFO("PC shader load phase {}: {:.2f}ms, {} loose reads, {} duplicates skipped, exact record equality {}",phase,ms,after.file_reads-before.file_reads,after.packed_duplicates_skipped-before.packed_duplicates_skipped,identical);
        ++phase;
    }
    // Exercise the mapped file's lifetime independently of the decoded cache:
    // in-place writes must fail, but atomic replacement keeps old readers valid.
    bool lifetime=false;
    try{
        if(expected.size()<2)throw std::runtime_error("lifetime fixture needs distinct shader records");
        const auto directory=output/"mapped-lifetime"/"translated-dxbc-v1";std::filesystem::create_directories(directory);
        const auto path=directory/"library.em2dxbcpack",temp=directory/"replacement.tmp";
        auto write=[&](const std::filesystem::path& target,const Bytes& record){std::ofstream file(target,std::ios::binary|std::ios::trunc);Bytes header;header.insert(header.end(),{'E','M','2','D','X','P','K','1'});U32(header,1);U32(header,uint32_t(record.size()));file.write(reinterpret_cast<const char*>(header.data()),header.size());file.write(reinterpret_cast<const char*>(record.data()),record.size());file.close();if(!file)throw std::runtime_error("lifetime fixture write failed");};
        write(path,expected.front());auto old=MappedPack::Open(path);
        HANDLE overwrite=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(overwrite!=INVALID_HANDLE_VALUE){CloseHandle(overwrite);throw std::runtime_error("mapped pack permitted in-place writes");}
        REXCVAR_SET(em2_mapped_shader_packs,true);auto library=OpenPcShaderLibrary(output/"mapped-lifetime");
        if(!library->SavePack(output/"mapped-lifetime"))throw std::runtime_error("mapped-source export/replacement failed");
        write(temp,expected.back());if(!ReplacePack(temp,path))throw std::runtime_error("mapped pack replacement failed");
        auto first=Decode(expected.front()),last=Decode(expected.back());auto replacement=OpenPcShaderLibrary(output/"mapped-lifetime");
        auto original_record=library->Find(first->key),replacement_record=replacement->Find(last->key);
        if(!original_record || !replacement_record || Encode(*Decode(old->BytesAt(16,expected.front().size())))!=expected.front() || Encode(*original_record)!=expected.front() || Encode(*replacement_record)!=expected.back() || replacement->Find(first->key))throw std::runtime_error("mapped reader changed across replacement");
        auto bad=expected.back();bad.back()^=1;write(temp,bad);if(!ReplacePack(temp,path))throw std::runtime_error("bad fixture replacement failed");
        const auto rejected=GetPcShaderLibraryStats().rejected;auto corrupt=OpenPcShaderLibrary(output/"mapped-lifetime");
        if(corrupt->Find(last->key) || GetPcShaderLibraryStats().rejected<=rejected)throw std::runtime_error("corrupt mapped record accepted");
        lifetime=true;
    }catch(const std::exception& error){REXLOG_ERROR("Mapped shader pack lifetime regression: {}",error.what());}
    success=success && lifetime;
    report<<"],\"mapped_lifetime_and_corruption\":"<<(lifetime?"true":"false")<<",\"passed\":"<<(success?"true":"false")<<"}\n";return success && report.good();
}
em2::PcShaderLibraryScope::PcShaderLibraryScope(std::shared_ptr<PcShaderLibrary> library):had_previous_(local_library.has_value()){if(had_previous_)previous_=*local_library;local_library=std::move(library);}
em2::PcShaderLibraryScope::~PcShaderLibraryScope(){if(had_previous_)local_library=std::move(previous_);else local_library.reset();}
bool rex::graphics::ShaderTranslator::TranslateAnalyzedShader(Shader::Translation& translation){
    std::shared_ptr<em2::PcShaderLibrary> library;
    if(REXCVAR_GET(em2_pretranslated_shaders)){if(local_library)library=*local_library;else{std::lock_guard lock(default_mutex);library=default_library.lock();}}
    auto* translator=library?dynamic_cast<T*>(this):nullptr;auto* shader=translator?dynamic_cast<D*>(&translation.shader()):nullptr;
    if(!shader || !shader->is_ucode_analyzed())return EM2OriginalTranslateShader(this,&translation);
    std::lock_guard serial(shader_locks[(reinterpret_cast<uintptr_t>(shader)>>4)%shader_locks.size()]);std::optional<Bytes> key;
    try{key=Key(*translator,translation);if(auto record=library->Find(*key);record && Restore(*record,translation,*shader)){++counters.hits;return true;}}catch(...){++counters.dropped;}
    ++counters.misses;const bool success=EM2OriginalTranslateShader(this,&translation);
    if(success && key)try{if(auto record=Capture(std::move(*key),translation,*shader))library->Add(std::move(record),true);}catch(...){++counters.dropped;}
    return success;
}
