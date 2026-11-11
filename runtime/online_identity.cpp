#include "online_identity.h"
#include <windows.h>
#include <bcrypt.h>
#include <rex/logging.h>
#include <rex/sha256.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <span>
#include <stdexcept>
#include <vector>

namespace em2::net {
namespace {
namespace fs=std::filesystem;
using Hash=std::array<uint8_t,32>;
class Sha {
public:
    Sha() {
        if(BCryptOpenAlgorithmProvider(&algorithm_,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) throw std::runtime_error("SHA-256 provider unavailable");
        if(BCryptCreateHash(algorithm_,&hash_,nullptr,0,nullptr,0,0)<0) {
            BCryptCloseAlgorithmProvider(algorithm_,0);algorithm_=nullptr;throw std::runtime_error("SHA-256 allocation failed");
        }
    }
    ~Sha(){if(hash_) BCryptDestroyHash(hash_);if(algorithm_) BCryptCloseAlgorithmProvider(algorithm_,0);}
    Sha(const Sha&)=delete;Sha& operator=(const Sha&)=delete;
    void Add(std::span<const uint8_t> bytes) {
        if(bytes.size()>ULONG_MAX || BCryptHashData(hash_,const_cast<PUCHAR>(bytes.data()),ULONG(bytes.size()),0)<0) throw std::runtime_error("SHA-256 update failed");
    }
    void Text(std::string_view value){Add({reinterpret_cast<const uint8_t*>(value.data()),value.size()});}
    void Number(uint64_t value,unsigned count){std::array<uint8_t,8> b{};for(unsigned i=0;i<count;++i)b[i]=uint8_t(value>>((count-i-1)*8));Add(std::span(b).first(count));}
    Hash Finish(){Hash result{};if(BCryptFinishHash(hash_,result.data(),ULONG(result.size()),0)<0)throw std::runtime_error("SHA-256 finish failed");return result;}
private:
    BCRYPT_ALG_HANDLE algorithm_=nullptr;BCRYPT_HASH_HANDLE hash_=nullptr;
};
void Cancel(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("Identity verification cancelled");}
std::string Key(const fs::path& path) {
    const auto u8=path.generic_u8string();std::string key(reinterpret_cast<const char*>(u8.data()),u8.size());
    for(auto& c:key) if(c>='A' && c<='Z') c=char(c-'A'+'a');
    if(key.empty() || key.size()>4096)throw std::runtime_error("Invalid identity path length");return key;
}
struct File {fs::path path;std::string key;uint64_t size;fs::file_time_type time;};
std::vector<File> Inventory(const fs::path& root,bool content,std::stop_token stop) {
    std::vector<File> files;std::vector<fs::path> ancestry;size_t path_bytes=0;
    auto visit=[&](auto&& self,const fs::path& folder,const fs::path& relative)->void {
        Cancel(stop);const auto canonical=fs::canonical(folder);
        if(ancestry.size()>=64 || std::find(ancestry.begin(),ancestry.end(),canonical)!=ancestry.end())throw std::runtime_error("Content directory cycle/depth limit");
        ancestry.push_back(canonical);
        for(const auto& entry:fs::directory_iterator(folder)) {
            Cancel(stop);const auto local=relative/entry.path().filename();const auto key=Key(local);
            // PC shader binaries and the Xbox dashboard updater do not define
            // the simulated world. Avoid opening thousands of local rendering
            // artifacts during connection setup; original game shaders and all
            // gameplay assets still receive full byte-level verification.
            if(content && (key=="source-game-directory.txt" ||
                key=="pc-shader-library" || key=="$systemupdate"))continue;
            if(entry.is_directory())self(self,entry.path(),local);
            else if(entry.is_regular_file()) {
                path_bytes+=key.size();
                if(files.size()>=250000 || path_bytes>32*1024*1024)throw std::runtime_error("Identity inventory limit");
                files.push_back({entry.path(),key,entry.file_size(),entry.last_write_time()});
            } else throw std::runtime_error("Unsupported content entry: "+key);
        }
        ancestry.pop_back();
    };
    visit(visit,root,{});
    std::sort(files.begin(),files.end(),[](const File& a,const File& b){return a.key<b.key;});
    if(files.empty())throw std::runtime_error("Empty identity directory");
    for(size_t i=1;i<files.size();++i)if(files[i].key==files[i-1].key)throw std::runtime_error("Ambiguous case-insensitive content path");
    return files;
}
bool LocalGraphicsLine(std::string_view line) {
    for(auto key:{"ShadowMapPlayerSize=","ShadowMapEnvSize=","SphericalHarmonicLightingRayCast=",
                  "SphericalHarmonicLightingRayCastDistanceThreshold=","BucketMemoryUse_GPU="})
        if(line.starts_with(key))return true;
    return false;
}
Hash HashFile(const File& file,bool config,std::stop_token stop,InstallationIdentity& stats) {
    Cancel(stop);
    Sha hash;uint64_t read=0;
    if(config) {
        std::ifstream input(file.path,std::ios::binary);if(!input)throw std::runtime_error("Cannot read identity file: "+file.key);
        if(file.size>4*1024*1024)throw std::runtime_error("Config identity size limit");
        std::string line;
        while(std::getline(input,line)) {
            Cancel(stop);read+=line.size()+(!input.eof()?1:0);
            if(!line.empty() && line.back()=='\r')line.pop_back();
            if(LocalGraphicsLine(line))line=line.substr(0,line.find('=')+1)+"<local-render-option>";
            hash.Text(line);hash.Text("\n");
        }
        if(input.bad())throw std::runtime_error("Config identity read failed");
    } else {
        // Buffered iostream reads can split a large request into small OS reads.
        // Keep hashing identical bytes but issue bounded sequential reads
        // directly; small shader files also need only their actual buffer size.
        const auto handle=CreateFileW(file.path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
        if(handle==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot read identity file: "+file.key);
        struct Close {HANDLE handle;~Close(){CloseHandle(handle);}} close{handle};
        std::vector<uint8_t> block(size_t(std::min<uint64_t>(1024*1024,std::max<uint64_t>(file.size,1))));
        for(;;) {
            Cancel(stop);DWORD count=0;
            if(!ReadFile(handle,block.data(),DWORD(block.size()),&count,nullptr))throw std::runtime_error("Identity file read failed: "+file.key);
            if(!count)break;
            read+=count;hash.Add(std::span(block).first(count));
        }
    }
    if(read!=file.size || fs::file_size(file.path)!=file.size || fs::last_write_time(file.path)!=file.time)throw std::runtime_error("Identity file changed while reading: "+file.key);
    ++stats.files;stats.bytes+=read;stats.read_bytes+=read;return hash.Finish();
}
Hash HashTree(const fs::path& root,bool content,std::stop_token stop,InstallationIdentity& stats) {
    const auto files=Inventory(root,content,stop);Sha tree;tree.Text(content?"EM2-content-v1":"EM2-checkpoint-files-v1");tree.Number(files.size(),8);
    for(const auto& file:files) {
        const auto digest=HashFile(file,content && file.key=="configfiles.ini",stop,stats);
        tree.Number(file.key.size(),4);tree.Text(file.key);tree.Add(digest);
    }
    const auto after=Inventory(root,content,stop);
    if(after.size()!=files.size())throw std::runtime_error("Identity directory changed during verification");
    for(size_t i=0;i<files.size();++i)if(files[i].key!=after[i].key || files[i].size!=after[i].size || files[i].time!=after[i].time)throw std::runtime_error("Identity directory changed during verification");
    return tree.Finish();
}

using NativeMetadata=std::array<uint8_t,56>;
std::optional<NativeMetadata> NativeFileMetadata(const File& file) {
    const auto handle=CreateFileW(file.path.c_str(),GENERIC_READ,FILE_SHARE_READ,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return {};
    struct Close {HANDLE handle;~Close(){CloseHandle(handle);}} close{handle};
    FILE_ID_INFO id{};FILE_BASIC_INFO times{};LARGE_INTEGER size{};wchar_t filesystem[16]{};
    // No metadata reuse on remote/unsupported filesystems. ChangeTime closes
    // the same-size write + restored modification-time hole on local NTFS.
    if(!GetFinalPathNameByHandleW(handle,nullptr,0,VOLUME_NAME_GUID|FILE_NAME_OPENED) ||
       !GetVolumeInformationByHandleW(handle,nullptr,0,nullptr,nullptr,nullptr,filesystem,DWORD(std::size(filesystem))) ||
       wcscmp(filesystem,L"NTFS")!=0 ||
       !GetFileInformationByHandleEx(handle,FileIdInfo,&id,sizeof(id)) ||
       !GetFileInformationByHandleEx(handle,FileBasicInfo,&times,sizeof(times)) ||
       !GetFileSizeEx(handle,&size) || size.QuadPart<0 || uint64_t(size.QuadPart)!=file.size || !times.ChangeTime.QuadPart)return {};
    NativeMetadata bytes{};size_t offset=0;
    auto number=[&](uint64_t value){for(unsigned i=0;i<8;++i)bytes[offset++]=uint8_t(value>>((7-i)*8));};
    number(id.VolumeSerialNumber);std::copy_n(id.FileId.Identifier,16,bytes.begin()+offset);offset+=16;
    number(file.size);number(times.CreationTime.QuadPart);number(times.LastWriteTime.QuadPart);number(times.ChangeTime.QuadPart);
    return bytes;
}
std::optional<Hash> NativeFileProof(const File& file){
    const auto metadata=NativeFileMetadata(file);if(!metadata)return {};
    Sha proof;proof.Text("EM2-native-file-metadata-v1");proof.Add(*metadata);return proof.Finish();
}
std::optional<Hash> ContentCacheKey(const std::vector<File>& files,std::stop_token stop,InstallationIdentity* physical=nullptr) {
    Sha key;key.Text("EM2-content-cache-v1");key.Number(files.size(),8);
    for(const auto& file:files){
        Cancel(stop);key.Number(file.key.size(),4);key.Text(file.key);
        if(file.key=="configfiles.ini"){
            InstallationIdentity ignored;key.Add(HashFile(file,true,stop,ignored));
            if(physical)physical->read_bytes+=ignored.read_bytes;continue;
        }
        const auto metadata=NativeFileMetadata(file);if(!metadata)return {};key.Add(*metadata);
    }
    return key.Finish();
}

using CacheRecord=std::array<uint8_t,96>;
CacheRecord MakeCacheRecord(const Hash& key,const Hash& digest) {
    CacheRecord bytes{};
    std::copy(key.begin(),key.end(),bytes.begin());
    std::copy(digest.begin(),digest.end(),bytes.begin()+32);
    Sha integrity;integrity.Add(std::span(bytes).first(64));
    const auto checksum=integrity.Finish();
    std::copy(checksum.begin(),checksum.end(),bytes.begin()+64);
    return bytes;
}
std::optional<Hash> ReadContentCache(const fs::path& path,const Hash& key) {
    std::ifstream input(path,std::ios::binary);
    CacheRecord bytes{};
    if(!input.read(reinterpret_cast<char*>(bytes.data()),bytes.size()) ||
       input.peek()!=std::char_traits<char>::eof() ||
       !std::equal(key.begin(),key.end(),bytes.begin()))return {};
    Hash digest{};
    std::copy_n(bytes.begin()+32,digest.size(),digest.begin());
    if(bytes!=MakeCacheRecord(key,digest))return {};
    return digest;
}
void WriteContentCache(const fs::path& path,const Hash& key,const Hash& digest) {
    static std::atomic<uint64_t> sequence=0;
    auto temporary=path;
    temporary+=L"."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(++sequence)+L".tmp";
    const auto bytes=MakeCacheRecord(key,digest);
    const auto handle=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return;
    DWORD written=0;
    const bool complete=WriteFile(handle,bytes.data(),DWORD(bytes.size()),&written,nullptr) &&
        written==bytes.size() && FlushFileBuffers(handle);
    CloseHandle(handle);
    if(!complete || !MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        DeleteFileW(temporary.c_str());
}
struct NativeFileCacheEntry {std::string key;Hash proof{},digest{};};
constexpr size_t kFileCacheLimit=64*1024*1024;
constexpr std::array<uint8_t,8> kFileCacheMagic{'E','M','2','N','F','C',1,0};
fs::path NativeFileCachePath(const fs::path& whole){auto path=whole;path+=L".files-v1";return path;}
std::vector<NativeFileCacheEntry> ReadNativeFileCache(const fs::path& path,std::stop_token stop){
    Cancel(stop);std::error_code error;const auto size=fs::file_size(path,error);
    if(error || size<44 || size>kFileCacheLimit)return {};
    std::ifstream input(path,std::ios::binary);std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if(!input.read(reinterpret_cast<char*>(bytes.data()),bytes.size()) || input.peek()!=std::char_traits<char>::eof() ||
       !std::equal(kFileCacheMagic.begin(),kFileCacheMagic.end(),bytes.begin()))return {};
    Sha checksum;checksum.Text("EM2-native-file-cache-v1");checksum.Add(std::span(bytes).first(bytes.size()-32));
    const auto integrity=checksum.Finish();if(!std::equal(integrity.begin(),integrity.end(),bytes.end()-32))return {};
    const auto end=bytes.size()-32;size_t offset=8;
    auto number=[&](){uint32_t value=0;for(unsigned i=0;i<4;++i)value=(value<<8)|bytes[offset++];return value;};
    const auto count=number();if(count>250000 || count>(end-12)/69)return {};
    std::vector<NativeFileCacheEntry> records;records.reserve(count);size_t path_bytes=0;
    for(unsigned i=0;i<count;++i){
        Cancel(stop);if(end-offset<4)return {};const auto length=number();
        if(!length || length>4096 || end-offset<size_t(length)+64 || (path_bytes+=length)>32*1024*1024)return {};
        NativeFileCacheEntry record;record.key.assign(reinterpret_cast<const char*>(bytes.data()+offset),length);offset+=length;
        if(!records.empty() && records.back().key>=record.key)return {};
        std::copy_n(bytes.begin()+offset,32,record.proof.begin());offset+=32;
        std::copy_n(bytes.begin()+offset,32,record.digest.begin());offset+=32;records.push_back(std::move(record));
    }
    if(offset!=end)return {};return records;
}
void WriteNativeFileCache(const fs::path& path,const std::vector<NativeFileCacheEntry>& records,std::stop_token stop){
    if(records.size()>250000)return;
    std::vector<uint8_t> bytes(kFileCacheMagic.begin(),kFileCacheMagic.end());
    auto number=[&](uint32_t value){for(unsigned i=0;i<4;++i)bytes.push_back(uint8_t(value>>((3-i)*8)));};
    number(uint32_t(records.size()));
    for(const auto& record:records){
        Cancel(stop);if(record.key.empty() || record.key.size()>4096 || bytes.size()+record.key.size()+100>kFileCacheLimit)return;
        number(uint32_t(record.key.size()));bytes.insert(bytes.end(),record.key.begin(),record.key.end());
        bytes.insert(bytes.end(),record.proof.begin(),record.proof.end());bytes.insert(bytes.end(),record.digest.begin(),record.digest.end());
    }
    Sha checksum;checksum.Text("EM2-native-file-cache-v1");checksum.Add(bytes);const auto integrity=checksum.Finish();bytes.insert(bytes.end(),integrity.begin(),integrity.end());
    static std::atomic<uint64_t> sequence=0;auto temporary=path;
    temporary+=L"."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(++sequence)+L".tmp";
    const auto handle=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(handle==INVALID_HANDLE_VALUE)return;
    DWORD written=0;const bool complete=WriteFile(handle,bytes.data(),DWORD(bytes.size()),&written,nullptr) && written==bytes.size() && FlushFileBuffers(handle);
    CloseHandle(handle);
    if(stop.stop_requested() || !complete || !MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))DeleteFileW(temporary.c_str());
    Cancel(stop);
}
Hash HashContentWithFileCache(const fs::path& root,const fs::path& path,std::stop_token stop,InstallationIdentity& stats){
    const auto files=Inventory(root,true,stop);const auto cached=ReadNativeFileCache(path,stop);
    std::vector<std::optional<Hash>> proofs;proofs.reserve(files.size());
    std::vector<NativeFileCacheEntry> refreshed;refreshed.reserve(files.size());
    Sha tree;tree.Text("EM2-content-v1");tree.Number(files.size(),8);
    for(const auto& file:files){
        Cancel(stop);const auto proof=NativeFileProof(file);proofs.push_back(proof);Hash digest{};
        const auto prior=std::lower_bound(cached.begin(),cached.end(),file.key,[](const auto& record,const std::string& key){return record.key<key;});
        if(proof && prior!=cached.end() && prior->key==file.key && prior->proof==*proof){
            digest=prior->digest;++stats.files;stats.bytes+=file.size;++stats.reused_files;
        }else digest=HashFile(file,file.key=="configfiles.ini",stop,stats);
        if(proof)refreshed.push_back({file.key,*proof,digest});
        tree.Number(file.key.size(),4);tree.Text(file.key);tree.Add(digest);
    }
    const auto after=Inventory(root,true,stop);
    if(after.size()!=files.size())throw std::runtime_error("Identity directory changed during verification");
    for(size_t i=0;i<files.size();++i){
        Cancel(stop);if(files[i].key!=after[i].key || files[i].size!=after[i].size || files[i].time!=after[i].time ||
            NativeFileProof(after[i])!=proofs[i])throw std::runtime_error("Identity file metadata changed during verification: "+files[i].key);
    }
    const auto digest=tree.Finish();WriteNativeFileCache(path,refreshed,stop);return digest;
}

}
IdentityResult FingerprintProgram(const fs::path& executable,const fs::path& content,std::stop_token stop,const fs::path& cache) {
    try {
        InstallationIdentity identity;
        const File build{executable,"beetleworx.exe",fs::file_size(executable),fs::last_write_time(executable)};
        identity.build=HashFile(build,false,stop,identity);
        const auto files=cache.empty()?std::vector<File>{}:Inventory(content,true,stop);
        const auto key=cache.empty()?std::optional<Hash>{}:ContentCacheKey(files,stop,&identity);
        const auto cached=key?ReadContentCache(cache,*key):std::optional<Hash>{};
        if(cached){
            identity.content=*cached;
            identity.reused_content_digest=true;
            identity.files+=files.size();
            for(const auto& file:files)identity.bytes+=file.size;
        }else{
            identity.content=cache.empty()?HashTree(content,true,stop,identity):HashContentWithFileCache(content,NativeFileCachePath(cache),stop,identity);
            if(key && ContentCacheKey(Inventory(content,true,stop),stop,&identity)==key)
                WriteContentCache(cache,*key,identity.content);
        }
        Cancel(stop);return {identity,{}};
    } catch(const std::exception& error){return {{},error.what()};}
}
IdentityResult FingerprintInstallation(const fs::path& executable,const fs::path& content,const fs::path& checkpoint,std::stop_token stop) {
    auto result=FingerprintProgram(executable,content,stop);if(!result.value)return result;
    try {result.value->checkpoint=HashTree(checkpoint,false,stop,*result.value);Cancel(stop);return result;}
    catch(const std::exception& error){return {{},error.what()};}
}
bool TestInstallationIdentity(const fs::path& root) {
    auto check=[](bool ok,const char* text){if(!ok)REXLOG_ERROR("Installation identity test: {}",text);return ok;};
    auto write=[](const fs::path& file,std::string_view data){fs::create_directories(file.parent_path());std::ofstream out(file,std::ios::binary);out.write(data.data(),data.size());if(!out)throw std::runtime_error("Identity fixture write failed");};
    try {
        const auto a=root/"a",b=root/"b";
        for(const auto& place:{a,b}) {
            write(place/"game.exe","abc");
            write(place/"content"/"cmdline.txt","mainmenu.start -Set DisablePersistence=false");
            write(place/"content"/"configfiles.ini","ShadowMapPlayerSize=512\r\nGravity=9.81\r\n");
            write(place/"content"/"source-game-directory.txt",place.string());
            write(place/"save"/"save.dat","checkpoint");
        }
        const std::string large(1024*1024+137,'x');
        write(a/"content"/"PackFiles"/"TEST.pak",large);write(b/"content"/"packfiles"/"test.pak",large);
        auto fingerprint=[&](const fs::path& place){return FingerprintInstallation(place/"game.exe",place/"content",place/"save");};
        const auto first=fingerprint(a),second=fingerprint(b);
        if(!check(first.value && second.value && first.value->build==second.value->build && first.value->content==second.value->content &&
            first.value->checkpoint==second.value->checkpoint,"relocation, case-insensitive relative paths, multi-block files and local source markers"))return false;
        std::string hex;for(auto byte:first.value->build){constexpr char digits[]="0123456789abcdef";hex+=digits[byte>>4];hex+=digits[byte&15];}
        if(!check(hex==rex::util::sha256("abc"),"streamed executable SHA-256 matches independent SDK implementation"))return false;
        const auto cache=b/"identity-cache.bin";
        auto cached=[&]{return FingerprintProgram(b/"game.exe",b/"content",{},cache);};
        const bool cache_supported=bool(ContentCacheKey(Inventory(b/"content",true,{}),{}));
        const auto cold=cached(),warm=cached();
        if(!check(cold.value && warm.value && !cold.value->reused_content_digest &&
            warm.value->reused_content_digest==cache_supported && warm.value->content==second.value->content &&
            warm.value->bytes==cold.value->bytes && warm.value->files==cold.value->files,
            "cold and warm content checks retain identical protocol identity and statistics"))return false;
        if(cache_supported){
            write(cache,"invalid cache");
            const auto repaired=cached(),reloaded=cached();
            if(!check(repaired.value && !repaired.value->reused_content_digest &&
                reloaded.value && reloaded.value->reused_content_digest && reloaded.value->content==cold.value->content,
                "corrupt identity cache is recomputed and replaced"))return false;
        }
        if(cache_supported){
            const auto area=root/"file-cache",tree=area/"content",sidecar=area/"identity.bin";
            const std::string config="Gravity=9.81\n",small_payload="small1";
            write(area/"game.exe","abc");write(tree/"configfiles.ini",config);
            write(tree/"packfiles"/"large.pak",large);write(tree/"small.bin",small_payload);
            auto verify=[&](bool use_cache){return FingerprintProgram(area/"game.exe",tree,{},use_cache?sidecar:fs::path{});};
            const auto initial=verify(true);if(!check(initial.value && !initial.value->reused_files,"per-file sidecar starts from verified full bytes"))return false;
            write(tree/"small.bin","small2");
            const auto partial=verify(true),independent=verify(false);
            const uint64_t minimal_reads=3+small_payload.size()+config.size()*2;
            if(!check(partial.value && independent.value && !partial.value->reused_content_digest && partial.value->reused_files==2 &&
                partial.value->read_bytes==minimal_reads && partial.value->content==independent.value->content &&
                partial.value->bytes==independent.value->bytes && partial.value->files==independent.value->files,
                "small changed file hashes only its bytes; unchanged large packfile/config reuse retains exact content-v1 digest"))return false;
            if(!check(CreateHardLinkW((area/"small-link.bin").c_str(),(tree/"small.bin").c_str(),nullptr)!=0,"NTFS metadata-only hardlink fixture"))return false;
            const auto metadata_only=verify(true);
            if(!check(metadata_only.value && !metadata_only.value->reused_content_digest && metadata_only.value->reused_files==2 &&
                metadata_only.value->read_bytes==minimal_reads && metadata_only.value->content==partial.value->content,
                "one hardlink ChangeTime invalidates only that small file rather than every unchanged packfile"))return false;
            auto records=ReadNativeFileCache(NativeFileCachePath(sidecar),{});
            for(auto& record:records){record.proof={};record.digest={};}
            WriteNativeFileCache(NativeFileCachePath(sidecar),records,{});write(sidecar,"force whole-cache miss");
            const auto forged=verify(true);
            if(!check(forged.value && !forged.value->reused_files && forged.value->content==partial.value->content && forged.value->read_bytes>large.size(),
                "checksummed records with forged metadata cannot reuse caller-supplied digests"))return false;
            auto native_cache=NativeFileCachePath(sidecar);
            {std::fstream bytes(native_cache,std::ios::binary|std::ios::in|std::ios::out);bytes.seekg(20);const auto old=bytes.get();bytes.seekp(20);bytes.put(char(old^1));}
            write(sidecar,"force whole-cache miss");const auto corrupt=verify(true);
            if(!check(corrupt.value && !corrupt.value->reused_files && corrupt.value->content==partial.value->content,
                "damaged file-cache integrity falls back to complete byte verification"))return false;
            write(native_cache,"EM2NFC");write(sidecar,"force whole-cache miss");const auto truncated=verify(true);
            if(!check(truncated.value && !truncated.value->reused_files && truncated.value->content==partial.value->content,
                "truncated file cache falls back without returning an unverified identity"))return false;
            const auto pak=tree/"packfiles"/"large.pak";const auto old_time=fs::last_write_time(pak);
            auto modified=large;modified.back()='z';write(pak,modified);fs::last_write_time(pak,old_time);
            const auto changed_metadata=verify(true),changed_full=verify(false);
            if(!check(changed_metadata.value && changed_full.value && !changed_metadata.value->reused_content_digest &&
                changed_metadata.value->reused_files==2 && changed_metadata.value->read_bytes==3+large.size()+config.size()*2 &&
                changed_metadata.value->content!=partial.value->content && changed_metadata.value->content==changed_full.value->content,
                "same-size packfile write with restored mtime forces new full digest through native ChangeTime"))return false;
        }
        write(b/"content"/"pc-shader-library"/"local.em2dxbc","local compiled shader");
        write(b/"content"/"$SystemUpdate"/"dashboard.bin","unused console updater");
        const auto local_artifacts=fingerprint(b);
        if(!check(local_artifacts.value && local_artifacts.value->content==first.value->content &&
            local_artifacts.value->files==first.value->files && local_artifacts.value->bytes==first.value->bytes,
            "PC shader library and console updater are not opened or hashed"))return false;
        write(a/"content"/"Shaders"/"original.shader","authored shader");
        const auto authored_shader=fingerprint(a);
        if(!check(authored_shader.value && authored_shader.value->content!=first.value->content,
            "original game shader assets remain verified"))return false;
        fs::remove(a/"content"/"Shaders"/"original.shader");
        write(b/"content"/"configfiles.ini","ShadowMapPlayerSize=2048\nGravity=9.81\n");
        const auto graphics=fingerprint(b);
        if(!check(graphics.value && graphics.value->content==first.value->content,"local graphics quality and CRLF do not block compatibility"))return false;
        const auto graphics_cached=cached();
        if(!check(graphics_cached.value && graphics_cached.value->reused_content_digest==cache_supported &&
            graphics_cached.value->content==graphics.value->content,"cache key respects graphics normalization"))return false;
        write(b/"content"/"configfiles.ini","ShadowMapPlayerSize=2048\nGravity=1.62\n");
        const auto physics=fingerprint(b);
        if(!check(physics.value && physics.value->content!=first.value->content,"gameplay configuration changes alter compatibility"))return false;
        const auto physics_cached=cached();
        if(!check(physics_cached.value && !physics_cached.value->reused_content_digest && physics_cached.value->content==physics.value->content,
            "gameplay config edits invalidate cached identity"))return false;
        const auto path=b/"content"/"packfiles"/"test.pak";const auto timestamp=fs::last_write_time(path);
        auto changed=large;changed.back()='y';write(path,changed);fs::last_write_time(path,timestamp);
        const auto asset=fingerprint(b);
        if(!check(asset.value && physics.value && asset.value->content!=physics.value->content,"same-size asset edits with preserved timestamps are detected"))return false;
        const auto asset_cached=cached();
        if(!check(asset_cached.value && !asset_cached.value->reused_content_digest && asset_cached.value->content==asset.value->content,
            "cache rejects same-size writes after restoring the original modification time"))return false;
        write(b/"save"/"save.dat","different checkpoint");write(b/"game.exe","abd");
        const auto altered=fingerprint(b);
        if(!check(altered.value && altered.value->checkpoint!=first.value->checkpoint && altered.value->build!=first.value->build,"checkpoint and executable changes detected independently"))return false;
        std::stop_source stop;stop.request_stop();const auto cancelled=FingerprintInstallation(a/"game.exe",a/"content",a/"save",stop.get_token());
        if(!check(!cancelled.value && !cancelled.error.empty() && !FingerprintInstallation(a/"missing.exe",a/"content",a/"save").value,"cancellation/missing file never returns a usable identity"))return false;
        REXLOG_INFO("Installation identity tests passed: streamed SHA-256, relocation/case, multi-block content, graphics normalization, per-file NTFS cache reuse/integrity/metadata-only edits, gameplay config, preserved-metadata edits, checkpoint/build changes and cancellation");return true;
    }catch(const std::exception& error){REXLOG_ERROR("Identity fixture failure: {}",error.what());return false;}
}
}
