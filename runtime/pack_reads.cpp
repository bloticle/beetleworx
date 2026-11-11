#include "pack_reads.h"
#include <rex/filesystem.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <windows.h>
#include <atomic>
#include <memory>
#include <cstring>
#include <cwctype>
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <thread>
#include <vector>
#include <immintrin.h>

REXCVAR_DEFINE_BOOL(em2_mapped_pack_reads,false,"CPU","Map repeatedly read, read-only game packs for small reads; bulk reads retain native I/O");
REXCVAR_DEFINE_BOOL(em2_benchmark_pack_reads,false,"Debug","Benchmark read-only pack I/O in the same-executable regression mode");
namespace rex::filesystem {
std::unique_ptr<FileHandle> EM2OriginalOpenExisting(const std::filesystem::path&,uint32_t);
}
namespace em2 {bool CopyMappedPack(void*,const void*,size_t);}
namespace {
std::atomic<std::shared_ptr<const std::wstring>> content_root;
constexpr size_t small_read=64*1024;
constexpr uint32_t mapping_threshold=64;
class PackReadHandle final:public rex::filesystem::FileHandle {
public:
    explicit PackReadHandle(std::unique_ptr<FileHandle>& native):FileHandle(native->path()),native_(std::move(native)){}
    ~PackReadHandle() override {
        if(auto* view=view_.load(std::memory_order_relaxed))UnmapViewOfFile(view);
        if(mapping_)CloseHandle(mapping_);
        if(file_!=INVALID_HANDLE_VALUE)CloseHandle(file_);
    }
    bool Read(size_t offset,void* output,size_t length,size_t* read) override {
        // Preserve native partial EOF, zero-length, oversized and bulk behavior.
        if(length && length<=small_read) {
            auto* view=view_.load(std::memory_order_acquire);
            if(!view && !unavailable_.load(std::memory_order_relaxed) &&
               reads_.fetch_add(1,std::memory_order_relaxed)==mapping_threshold-1) {
                TryMap();view=view_.load(std::memory_order_acquire);
            }
            if(view && offset<=size_ && length<=size_-offset && em2::CopyMappedPack(output,view+offset,length)) {
                *read=length;return true;
            }
        }
        return native_->Read(offset,output,length,read);
    }
    bool Write(size_t offset,const void* input,size_t length,size_t* written) override{return native_->Write(offset,input,length,written);}
    bool SetLength(size_t length) override{return native_->SetLength(length);}
    void Flush() override{native_->Flush();}
    bool mapped()const{return view_.load(std::memory_order_acquire)!=nullptr;}
    const uint8_t* TestView()const{return view_.load(std::memory_order_acquire);}
private:
    void TryMap() {
        // Deny write sharing: an existing writer makes mapping unavailable,
        // and mapped immutable content cannot be changed/truncated beneath us.
        file_=CreateFileW(path_.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        LARGE_INTEGER size{};
        if(file_!=INVALID_HANDLE_VALUE && GetFileSizeEx(file_,&size) && size.QuadPart>=64*1024 && uint64_t(size.QuadPart)<=SIZE_MAX) {
            mapping_=CreateFileMappingW(file_,nullptr,PAGE_READONLY,0,0,nullptr);
            if(mapping_) {
                auto* view=static_cast<const uint8_t*>(MapViewOfFile(mapping_,FILE_MAP_READ,0,0,0));
                if(view){size_=size_t(size.QuadPart);view_.store(view,std::memory_order_release);return;}
            }
        }
        if(mapping_){CloseHandle(mapping_);mapping_=nullptr;}
        if(file_!=INVALID_HANDLE_VALUE){CloseHandle(file_);file_=INVALID_HANDLE_VALUE;}
        unavailable_.store(true,std::memory_order_relaxed);
    }
    std::unique_ptr<FileHandle> native_;
    HANDLE file_=INVALID_HANDLE_VALUE,mapping_=nullptr;
    size_t size_=0;
    std::atomic<const uint8_t*> view_{nullptr};
    std::atomic<uint32_t> reads_{0};
    std::atomic<bool> unavailable_{false};
};
bool Eligible(const std::filesystem::path& path,uint32_t access) {
    using A=rex::filesystem::FileAccess;
    if(!(access&(A::kGenericRead|A::kFileReadData)) || access&(A::kGenericWrite|A::kGenericAll|A::kFileWriteData|A::kFileAppendData))return false;
    const auto root=content_root.load(std::memory_order_acquire);if(!root)return false;
    const auto full=path.lexically_normal().native();
    if(full.size()<=root->size() || CompareStringOrdinal(full.data(),int(root->size()),root->data(),int(root->size()),TRUE)!=CSTR_EQUAL)return false;
    const auto extension=path.extension().native();
    return extension.size()==4 && CompareStringOrdinal(extension.data(),4,L".pak",4,TRUE)==CSTR_EQUAL;
}
}
void em2::ConfigurePackReads(const std::filesystem::path& root) {
    if(root.empty()){content_root.store({});return;}
    auto full=std::filesystem::absolute(root).lexically_normal();auto text=full.native();
    if(text.starts_with(L"\\\\") || GetDriveTypeW(full.root_path().c_str())==DRIVE_REMOTE){content_root.store({});return;}
    if(!text.ends_with(L'\\') && !text.ends_with(L'/'))text+=L'\\';
    content_root.store(std::make_shared<const std::wstring>(std::move(text)),std::memory_order_release);
}
namespace {
__declspec(noinline) std::unique_ptr<rex::filesystem::FileHandle> OpenPackExisting(const std::filesystem::path& path,uint32_t access) {
    auto native=rex::filesystem::EM2OriginalOpenExisting(path,access);
    try{if(native && Eligible(path,access))return std::make_unique<PackReadHandle>(native);}catch(...){}
    return native;
}
}
std::unique_ptr<rex::filesystem::FileHandle> rex::filesystem::FileHandle::OpenExisting(const std::filesystem::path& path,uint32_t access) {
    if(!REXCVAR_GET(em2_mapped_pack_reads))return EM2OriginalOpenExisting(path,access);
    return OpenPackExisting(path,access);
}

bool em2::TestPackReads(const std::filesystem::path& directory) {
    using rex::filesystem::FileHandle;using rex::filesystem::FileAccess;
    struct Restore {
        bool enabled=REXCVAR_GET(em2_mapped_pack_reads);
        std::shared_ptr<const std::wstring> root=content_root.load();
        ~Restore(){REXCVAR_SET(em2_mapped_pack_reads,enabled);content_root.store(root);}
    } restore;
    std::filesystem::create_directories(directory);
    const auto file=std::filesystem::absolute(directory/"fixture.pak");
    std::vector<uint8_t> expected(2*1024*1024+37);
    uint32_t random=0xE210;for(auto& byte:expected){random=random*1664525+1013904223;byte=uint8_t(random>>24);}
    {std::ofstream out(file,std::ios::binary);out.write(reinterpret_cast<const char*>(expected.data()),expected.size());if(!out)return false;}
    ConfigurePackReads(directory);
    if(!Eligible(file,FileAccess::kGenericRead))return false;
    ConfigurePackReads({});if(Eligible(file,FileAccess::kGenericRead))return false;
    ConfigurePackReads(directory);
    REXCVAR_SET(em2_mapped_pack_reads,false);auto native=FileHandle::OpenExisting(file,FileAccess::kGenericRead);
    REXCVAR_SET(em2_mapped_pack_reads,true);auto mapped=FileHandle::OpenExisting(file,FileAccess::kGenericRead);
    auto* cache=dynamic_cast<PackReadHandle*>(mapped.get());
    if(!native || !cache || cache->mapped())return false;
    std::vector<uint8_t> reference(256*1024),actual(reference.size());unsigned comparisons=0;
    auto compare=[&](size_t offset,size_t length) {
        std::fill(reference.begin(),reference.end(),0xA5);actual=reference;
        size_t a=99,b=99;const auto csr=_mm_getcsr();
        const bool x=native->Read(offset,reference.data(),length,&a),y=mapped->Read(offset,actual.data(),length,&b);
        ++comparisons;return x==y && a==b && reference==actual && _mm_getcsr()==csr;
    };
    for(unsigned i=0;i<mapping_threshold-1;++i)if(!compare(i*17,32) || cache->mapped())return false;
    if(!compare(123,32) || !cache->mapped())return false;
    for(size_t length:{0u,1u,3u,4u,31u,64u,255u,4096u,65536u,65537u,131072u})
        for(size_t offset:{size_t(0),size_t(1),size_t(4093),expected.size()-1,expected.size(),expected.size()+1,SIZE_MAX-3})
            if(!compare(offset,length))return false;
    void* guard=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_NOACCESS);
    if(!guard)return false;
    size_t a=88,b=88;const bool x=native->Read(0,guard,32,&a),y=mapped->Read(0,guard,32,&b);VirtualFree(guard,0,MEM_RELEASE);
    if(x!=y || a!=b)return false;
    DWORD old_protection=0,ignored=0;
    if(!VirtualProtect(const_cast<uint8_t*>(cache->TestView()),4096,PAGE_NOACCESS,&old_protection))return false;
    const bool source_fallback=compare(0,32);
    const bool restored=VirtualProtect(const_cast<uint8_t*>(cache->TestView()),4096,old_protection,&ignored)!=FALSE;
    if(!source_fallback || !restored)return false;
    size_t native_written=8,mapped_written=8;const uint8_t forbidden=0x77;
    if(native->Write(0,&forbidden,1,&native_written)!=mapped->Write(0,&forbidden,1,&mapped_written) ||
       native_written!=mapped_written || native->SetLength(expected.size())!=mapped->SetLength(expected.size()))return false;
    native->Flush();mapped->Flush();
    std::atomic<bool> concurrent{true};std::array<std::thread,4> workers;
    for(unsigned worker=0;worker<workers.size();++worker)workers[worker]=std::thread([&,worker]{
        std::array<uint8_t,513> bytes{};
        for(unsigned i=0;i<1024;++i){const size_t offset=(i*871+worker*191)%(expected.size()-bytes.size());size_t read=0;
            if(!mapped->Read(offset,bytes.data(),bytes.size(),&read) || read!=bytes.size() || !std::equal(bytes.begin(),bytes.end(),expected.begin()+offset))concurrent=false;}
    });
    for(auto& worker:workers)worker.join();if(!concurrent)return false;
    {
        // Preserve the SDK's low-level read-data access mode as well as its
        // generic-read mode; do not gain permissions through the second handle.
        auto data_native=rex::filesystem::EM2OriginalOpenExisting(file,FileAccess::kFileReadData);
        auto data_mapped=FileHandle::OpenExisting(file,FileAccess::kFileReadData);
        if(!data_native || !data_mapped)return false;
        std::array<uint8_t,32> first{},second{};
        for(unsigned i=0;i<64;++i){size_t an=0,bn=0;
            if(data_native->Read(i,first.data(),first.size(),&an)!=data_mapped->Read(i,second.data(),second.size(),&bn) || an!=bn || first!=second)return false;}
    }
    mapped.reset(); // Release immutable-content lock before write-fallback tests.
    auto writer=FileHandle::OpenExisting(file,FileAccess::kGenericRead|FileAccess::kGenericWrite);
    auto fallback=FileHandle::OpenExisting(file,FileAccess::kGenericRead);
    if(!writer || dynamic_cast<PackReadHandle*>(writer.get()) || !dynamic_cast<PackReadHandle*>(fallback.get()))return false;
    for(unsigned i=0;i<128;++i){size_t read=0;if(!fallback->Read(0,actual.data(),64,&read) || read!=64)return false;}
    if(static_cast<PackReadHandle*>(fallback.get())->mapped())return false; // Existing writer rejects a mapping.
    const uint8_t changed=0x19;size_t written=0,read=0;
    if(!writer->Write(0,&changed,1,&written) || written!=1 || !fallback->Read(0,actual.data(),1,&read) || read!=1 || actual[0]!=changed)return false;
    fallback.reset();writer.reset();
    if(FileHandle::OpenExisting(directory/"missing.pak",FileAccess::kGenericRead))return false;
    if(Eligible(file,FileAccess::kFileWriteData) || Eligible(file,FileAccess::kGenericAll) ||
       Eligible(file.parent_path()/"save.dat",FileAccess::kGenericRead) ||
       Eligible(file.parent_path()/".."/"outside.pak",FileAccess::kGenericRead))return false;
    REXLOG_INFO("Read-only pack I/O tests passed: {} complete read/output/EOF comparisons, guarded source/destination native fallback, 4096 concurrent reads, write/resize/flush delegation, lazy mapping and writer/access/path isolation; SDK ignore_thread_affinities={}",comparisons,rex::cvar::GetFlagByName("ignore_thread_affinities"));
    if(REXCVAR_GET(em2_benchmark_pack_reads)) {
        using Clock=std::chrono::steady_clock;
        struct Affinity {DWORD_PTR old=SetThreadAffinityMask(GetCurrentThread(),DWORD_PTR(1)<<GetCurrentProcessorNumber());~Affinity(){if(old)SetThreadAffinityMask(GetCurrentThread(),old);}} affinity;
        for(unsigned length:{32u,256u,4096u,131072u})for(unsigned requests:{16u,64u,256u,2048u}) {
            auto run=[&](bool enabled){
                REXCVAR_SET(em2_mapped_pack_reads,enabled);std::vector<uint8_t> buffer(length);uint64_t checksum=0;
                const auto start=Clock::now();
                {auto handle=FileHandle::OpenExisting(file,FileAccess::kGenericRead);if(!handle)throw std::runtime_error("Pack benchmark open failed");
                for(unsigned i=0;i<requests;++i){size_t got=0;const size_t offset=(i*193ull)%(expected.size()-length);
                    if(!handle->Read(offset,buffer.data(),length,&got) || got!=length)throw std::runtime_error("Pack benchmark read failed");
                    checksum+=buffer.front()+buffer.back();}}
                return std::pair{std::chrono::duration<double,std::micro>(Clock::now()-start).count(),checksum};
            };
            run(false);run(true);std::array<std::array<double,5>,2> times{};
            for(unsigned round=0;round<5;++round){const auto first=run(round%2!=0),second=run(round%2==0);
                if(first.second!=second.second)return false;times[round%2][round]=first.first;times[1-round%2][round]=second.first;}
            for(auto& row:times)std::sort(row.begin(),row.end());
            REXLOG_INFO("Pack read benchmark: {} reads x {} bytes, native {:.3f}, mapped {:.3f} us/open-read-close workload, {:.3f}x (five alternating rounds; warm synthetic file, includes lazy mapping/setup; not cold loading or FPS)",requests,length,times[0][2],times[1][2],times[0][2]/times[1][2]);
        }
    }
    return true;
}
