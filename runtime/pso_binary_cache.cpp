#include <d3d12sdklayers.h>
#include "pso_binary_cache.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_util.h>
#include <windows.h>
#include <bcrypt.h>
#include <immintrin.h>
#include <array>
#include <algorithm>
#include <type_traits>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

REXCVAR_DEFINE_BOOL(em2_persistent_pso_cache,false,"GPU","Cache driver-compiled graphics pipelines; rejected blobs fall back to normal creation");
REXCVAR_DECLARE(bool,em2_persistent_shader_cache);
REXCVAR_DEFINE_BOOL(em2_benchmark_pso_cache,false,"Debug","Compare native, blob-only and full compiled-cache PSO creation in the hidden GPU diagnostic");
extern "C" HRESULT EM2CreateGraphicsPipelineStateCached(ID3D12Device*,const D3D12_GRAPHICS_PIPELINE_STATE_DESC*,void*,void**);
extern "C" ID3D12RootSignature* EM2OriginalCreateRootSignature(const rex::ui::d3d12::D3D12Provider&,const D3D12_ROOT_SIGNATURE_DESC&);
namespace {
using Digest=std::array<uint8_t,32>;
constexpr GUID root_digest_guid={0x98b231c2,0x270d,0x4c59,{0xb7,0x4c,0x93,0xcf,0x6a,0x90,0xe5,0x16}};
constexpr uint64_t max_blob=16*1024*1024,max_queue=64*1024*1024;
struct Counters {std::atomic<uint64_t> calls{0},keys{0},hits{0},accepted{0},rejected{0},saved{0},uncacheable{0},dropped{0};} counters;
class Sha {
    std::vector<uint8_t> bytes_;
public:
    Sha(){bytes_.reserve(4096);}
    void Bytes(const void* data,size_t size){
        if(size>ULONG_MAX || size>256*1024*1024ull-bytes_.size() || (size&&!data))throw std::runtime_error("PSO SHA input invalid");
        if(size){const auto* first=static_cast<const uint8_t*>(data);bytes_.insert(bytes_.end(),first,first+size);}
    }
    template<class T>void Value(T value){static_assert(std::is_trivially_copyable_v<T>);Bytes(&value,sizeof(value));}
    void Text(const char* text){if(!text){Value(UINT32_MAX);return;}const auto size=strnlen(text,1025);if(size>1024)throw std::runtime_error("PSO semantic too long");Value(uint32_t(size));Bytes(text,size);}
    Digest Finish(){
        // Windows 10+ (also required by D3D12) provides a shared algorithm
        // pseudo-handle. Do not reopen/close the provider for every cache key.
        struct Handles {BCRYPT_HASH_HANDLE hash=nullptr;~Handles(){if(hash)BCryptDestroyHash(hash);}} handles;
        if(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE,&handles.hash,nullptr,0,nullptr,0,0)<0 ||
           BCryptHashData(handles.hash,bytes_.data(),ULONG(bytes_.size()),0)<0)throw std::runtime_error("PSO SHA failed");
        Digest digest{};if(BCryptFinishHash(handles.hash,digest.data(),ULONG(digest.size()),0)<0)throw std::runtime_error("PSO SHA finish failed");return digest;
    }
};
Digest Hash(std::span<const uint8_t> bytes){Sha hash;hash.Bytes(bytes.data(),bytes.size());return hash.Finish();}
std::string Hex(const Digest& digest){constexpr char chars[]="0123456789abcdef";std::string out;out.reserve(64);for(auto value:digest){out+=chars[value>>4];out+=chars[value&15];}return out;}
Digest PipelineKey(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d,const Digest& root,LUID adapter) {
    Sha h;h.Value(uint32_t(1));h.Bytes(root.data(),root.size());h.Value(adapter.LowPart);h.Value(adapter.HighPart);
    for(const auto& shader:{d.VS,d.PS,d.DS,d.HS,d.GS}){if(shader.BytecodeLength>max_blob)throw std::runtime_error("PSO shader too large");h.Value(uint64_t(shader.BytecodeLength));h.Bytes(shader.pShaderBytecode,shader.BytecodeLength);}
    const auto& so=d.StreamOutput;if(so.NumEntries>512 || so.NumStrides>4 || (so.NumEntries&&!so.pSODeclaration) || (so.NumStrides&&!so.pBufferStrides))throw std::runtime_error("PSO stream output invalid");
    h.Value(so.NumEntries);for(UINT i=0;i<so.NumEntries;++i){const auto& e=so.pSODeclaration[i];h.Value(e.Stream);h.Text(e.SemanticName);h.Value(e.SemanticIndex);h.Value(e.StartComponent);h.Value(e.ComponentCount);h.Value(e.OutputSlot);}
    h.Value(so.NumStrides);for(UINT i=0;i<so.NumStrides;++i)h.Value(so.pBufferStrides[i]);h.Value(so.RasterizedStream);
    h.Value(d.BlendState.AlphaToCoverageEnable);h.Value(d.BlendState.IndependentBlendEnable);
    for(const auto& rt:d.BlendState.RenderTarget){h.Value(rt.BlendEnable);h.Value(rt.LogicOpEnable);h.Value(rt.SrcBlend);h.Value(rt.DestBlend);h.Value(rt.BlendOp);h.Value(rt.SrcBlendAlpha);h.Value(rt.DestBlendAlpha);h.Value(rt.BlendOpAlpha);h.Value(rt.LogicOp);h.Value(rt.RenderTargetWriteMask);}
    h.Value(d.SampleMask);const auto& r=d.RasterizerState;
    h.Value(r.FillMode);h.Value(r.CullMode);h.Value(r.FrontCounterClockwise);h.Value(r.DepthBias);h.Value(std::bit_cast<uint32_t>(r.DepthBiasClamp));h.Value(std::bit_cast<uint32_t>(r.SlopeScaledDepthBias));h.Value(r.DepthClipEnable);h.Value(r.MultisampleEnable);h.Value(r.AntialiasedLineEnable);h.Value(r.ForcedSampleCount);h.Value(r.ConservativeRaster);
    const auto& z=d.DepthStencilState;h.Value(z.DepthEnable);h.Value(z.DepthWriteMask);h.Value(z.DepthFunc);h.Value(z.StencilEnable);h.Value(z.StencilReadMask);h.Value(z.StencilWriteMask);
    for(const auto& face:{z.FrontFace,z.BackFace}){h.Value(face.StencilFailOp);h.Value(face.StencilDepthFailOp);h.Value(face.StencilPassOp);h.Value(face.StencilFunc);}
    if(d.InputLayout.NumElements>64 || (d.InputLayout.NumElements&&!d.InputLayout.pInputElementDescs))throw std::runtime_error("PSO input layout invalid");
    h.Value(d.InputLayout.NumElements);for(UINT i=0;i<d.InputLayout.NumElements;++i){const auto& e=d.InputLayout.pInputElementDescs[i];h.Text(e.SemanticName);h.Value(e.SemanticIndex);h.Value(e.Format);h.Value(e.InputSlot);h.Value(e.AlignedByteOffset);h.Value(e.InputSlotClass);h.Value(e.InstanceDataStepRate);}
    h.Value(d.IBStripCutValue);h.Value(d.PrimitiveTopologyType);h.Value(d.NumRenderTargets);for(auto format:d.RTVFormats)h.Value(format);h.Value(d.DSVFormat);h.Value(d.SampleDesc.Count);h.Value(d.SampleDesc.Quality);h.Value(d.NodeMask);h.Value(d.Flags);
    return h.Finish();
}
constexpr std::array<uint8_t,8> magic={'E','M','2','P','S','O','0','1'};
std::vector<uint8_t> Encode(const Digest& key,std::span<const uint8_t> bytes){
    if(bytes.empty() || bytes.size()>max_blob)throw std::runtime_error("PSO blob size invalid");
    std::vector<uint8_t> out(magic.begin(),magic.end());const uint64_t size=bytes.size();for(unsigned i=0;i<8;++i)out.push_back(uint8_t(size>>(8*i)));
    out.insert(out.end(),key.begin(),key.end());const auto digest=Hash(bytes);out.insert(out.end(),digest.begin(),digest.end());out.insert(out.end(),bytes.begin(),bytes.end());return out;
}
std::vector<uint8_t> Decode(const Digest& key,std::span<const uint8_t> bytes){
    constexpr size_t header=80;if(bytes.size()<header || !std::equal(magic.begin(),magic.end(),bytes.begin()) || !std::equal(key.begin(),key.end(),bytes.begin()+16))return {};
    uint64_t size=0;for(unsigned i=0;i<8;++i)size|=uint64_t(bytes[8+i])<<(8*i);
    if(!size || size>max_blob || size!=bytes.size()-header)return {};
    const auto digest=Hash(bytes.subspan(header));if(!std::equal(digest.begin(),digest.end(),bytes.begin()+48))return {};
    return {bytes.begin()+header,bytes.end()};
}
struct CreationResult {HRESULT result;bool attempted,accepted;};
template<class Create> CreationResult CreateWithFallback(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& original,std::vector<uint8_t>& cached,Create create) {
    if(cached.empty())return {create(original),false,false};
    auto temporary=original;temporary.CachedPSO={cached.data(),cached.size()};
    const auto result=create(temporary);if(SUCCEEDED(result))return {result,true,true};
    cached.clear();return {create(original),true,false};
}
thread_local bool pso_benchmark_running=false;
std::atomic<bool> pso_benchmark_done{false};
void BenchmarkPso(ID3D12Device*,const D3D12_GRAPHICS_PIPELINE_STATE_DESC&,const std::vector<uint8_t>&);
std::mutex active_mutex;
std::shared_ptr<em2::PsoBinaryStore> active_store;
}

class em2::PsoBinaryStore {
    struct Request {Digest key;std::vector<uint8_t> bytes;};
    std::filesystem::path root_;std::mutex mutex_;std::condition_variable ready_;std::deque<Request> requests_;
    uint64_t queued_=0;bool stopping_=false;std::thread worker_;std::atomic<uint64_t> serial_{0};
    void Write(const Request& request){
        const auto data=Encode(request.key,request.bytes);const auto file=root_/(Hex(request.key)+".pso");
        const auto temporary=root_/(Hex(request.key)+"."+std::to_string(GetCurrentProcessId())+"."+std::to_string(serial_++)+".tmp");
        HANDLE handle=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(handle==INVALID_HANDLE_VALUE){++counters.dropped;return;}
        DWORD written=0;const bool passed=WriteFile(handle,data.data(),DWORD(data.size()),&written,nullptr) && written==data.size();CloseHandle(handle);
        if(passed && MoveFileExW(temporary.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING))++counters.saved;
        else {++counters.dropped;DeleteFileW(temporary.c_str());}
    }
    void Run(){for(;;){Request request;{std::unique_lock lock(mutex_);ready_.wait(lock,[&]{return stopping_||!requests_.empty();});if(requests_.empty())return;request=std::move(requests_.front());requests_.pop_front();queued_-=request.bytes.size();}try{Write(request);}catch(...){++counters.dropped;}}}
public:
    explicit PsoBinaryStore(std::filesystem::path root):root_(std::move(root)){std::filesystem::create_directories(root_);worker_=std::thread([this]{Run();});}
    ~PsoBinaryStore(){Close();}
    void Close(){ {std::lock_guard lock(mutex_);stopping_=true;}ready_.notify_all();if(worker_.joinable())worker_.join();}
    std::vector<uint8_t> Load(const Digest& key){
        const auto file=root_/(Hex(key)+".pso");
        struct File {HANDLE value;~File(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}} input{
            CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)};
        if(input.value==INVALID_HANDLE_VALUE)return {};
        // Size and contents must refer to the same file even if the writer
        // atomically replaces its directory entry while this handle is open.
        LARGE_INTEGER size{};if(!GetFileSizeEx(input.value,&size))return {};
        if(size.QuadPart<80 || uint64_t(size.QuadPart)>max_blob+80){++counters.rejected;return {};}
        std::vector<uint8_t> bytes(size_t(size.QuadPart));DWORD read=0;
        if(!ReadFile(input.value,bytes.data(),DWORD(bytes.size()),&read,nullptr) || read!=bytes.size())return {};
        auto decoded=Decode(key,bytes);if(decoded.empty())++counters.rejected;else ++counters.hits;return decoded;
    }
    void Save(const Digest& key,std::span<const uint8_t> bytes){
        if(bytes.empty() || bytes.size()>max_blob){++counters.dropped;return;}
        std::lock_guard lock(mutex_);if(stopping_ || queued_+bytes.size()>max_queue || requests_.size()>=256){++counters.dropped;return;}
        Request request{key,{bytes.begin(),bytes.end()}};requests_.push_back(std::move(request));queued_+=bytes.size();ready_.notify_one();
    }
};

std::shared_ptr<em2::PsoBinaryStore> em2::StartPsoBinaryStore(const std::filesystem::path& root){
    if(!REXCVAR_GET(em2_persistent_pso_cache))return {};
    auto store=std::make_shared<PsoBinaryStore>(root/"compiled-pso-v1");
    std::lock_guard lock(active_mutex);if(active_store)throw std::runtime_error("A compiled PSO store is already active");active_store=store;
    REXLOG_INFO("Compiled graphics PSO cache active: {}",(root/"compiled-pso-v1").string());return store;
}
void em2::StopPsoBinaryStore(std::shared_ptr<PsoBinaryStore>& store){
    if(!store)return;{std::lock_guard lock(active_mutex);if(active_store==store)active_store.reset();}store->Close();store.reset();
    const auto s=ReadPsoBinaryStats();REXLOG_INFO("Compiled PSO cache totals: calls {}, keys {}, disk hits {}, accepted {}, rejected {}, saved {}, uncacheable {}, dropped {}",s.calls,s.keys,s.hits,s.accepted,s.rejected,s.saved,s.uncacheable,s.dropped);
}
em2::PsoBinaryStats em2::ReadPsoBinaryStats(){return {counters.calls.load(),counters.keys.load(),counters.hits.load(),counters.accepted.load(),counters.rejected.load(),counters.saved.load(),counters.uncacheable.load(),counters.dropped.load()};}

ID3D12RootSignature* rex::ui::d3d12::util::CreateRootSignature(const D3D12Provider& provider,const D3D12_ROOT_SIGNATURE_DESC& desc){
    // Root signatures outlive individual cache settings and renderer graphs.
    // Preserve their identity even while driver-blob caching is disabled so
    // enabling it later can cache already-created signatures without restart.
    auto* signature=EM2OriginalCreateRootSignature(provider,desc);if(!signature || !REXCVAR_GET(em2_persistent_shader_cache))return signature;
    const auto csr=_mm_getcsr();ID3DBlob* blob=nullptr;ID3DBlob* error=nullptr;
    try{if(SUCCEEDED(provider.SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error)) && blob){Sha h;h.Bytes(blob->GetBufferPointer(),blob->GetBufferSize());const auto digest=h.Finish();signature->SetPrivateData(root_digest_guid,UINT(digest.size()),digest.data());}}catch(...){}
    if(blob)blob->Release();if(error)error->Release();_mm_setcsr(csr);return signature;
}

namespace {
__declspec(noinline) HRESULT CreateCachedPipeline(ID3D12Device* device,const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,void** output){
    ++counters.calls;
    std::shared_ptr<em2::PsoBinaryStore> store;Digest key{};std::vector<uint8_t> cached;
    const auto entry_csr=_mm_getcsr();
    try {
        if(REXCVAR_GET(em2_persistent_pso_cache) && desc && !desc->CachedPSO.pCachedBlob && !desc->CachedPSO.CachedBlobSizeInBytes){
            {std::lock_guard lock(active_mutex);store=active_store;}
            Digest root{};UINT size=UINT(root.size());
            if(store && desc->pRootSignature && SUCCEEDED(desc->pRootSignature->GetPrivateData(root_digest_guid,&size,root.data())) && size==root.size()){
                key=PipelineKey(*desc,root,device->GetAdapterLuid());++counters.keys;cached=store->Load(key);
            }else store.reset();
        }
    }catch(...){store.reset();cached.clear();}
    _mm_setcsr(entry_csr);
    HRESULT result;
    if(desc){
        const auto created=CreateWithFallback(*desc,cached,[&](const auto& state){return device->CreateGraphicsPipelineState(&state,__uuidof(ID3D12PipelineState),output);});
        result=created.result;if(created.attempted){if(created.accepted)++counters.accepted;else ++counters.rejected;}
    }else result=device->CreateGraphicsPipelineState(desc,__uuidof(ID3D12PipelineState),output);
    const auto result_csr=_mm_getcsr();
    if(SUCCEEDED(result) && output && *output && store && cached.empty()){
        ID3DBlob* blob=nullptr;
        try{if(SUCCEEDED(static_cast<ID3D12PipelineState*>(*output)->GetCachedBlob(&blob)) && blob){store->Save(key,{static_cast<const uint8_t*>(blob->GetBufferPointer()),blob->GetBufferSize()});}else ++counters.uncacheable;}catch(...){++counters.dropped;}
        if(blob)blob->Release();
    }
    if(SUCCEEDED(result) && store && !cached.empty() && REXCVAR_GET(em2_benchmark_pso_cache) && !pso_benchmark_running && !pso_benchmark_done.exchange(true)){
        try{BenchmarkPso(device,*desc,cached);}catch(...){REXLOG_ERROR("PSO creation benchmark failed");}
    }
    _mm_setcsr(result_csr);return result;
}
}
#include "hdr_scene_cache.h"
#include "hdr_scene_target.h"
#include "material_capture.h"
#include "gpu_draw_profile.h"
#include "material_ambient_export.h"
extern "C" HRESULT EM2CreateGraphicsPipelineStateCached(ID3D12Device* device,const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,void*,void** output){
    const bool preserve_surface=desc && em2::CurrentMaterialSurfacePreserving(desc->PS);
    bool surface_program=false;auto original=desc?em2::CurrentMaterialOriginal(desc->PS,&surface_program):nullptr;
    bool material_capture=original && em2::MaterialAmbientPipeline::Supports(*desc);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC source{};if(desc)source=*desc;const auto input=source;
    std::unique_ptr<em2::MaterialLightingSurfacePipeline> surface;
    std::unique_ptr<em2::MaterialAmbientPipeline> ambient;
    std::unique_ptr<em2::MaterialAmbientClearPipeline> ambient_clear;
    std::unique_ptr<em2::MaterialSurfaceClearPipeline> surface_clear;
    const bool registered=em2::CurrentMaterialCapture();
    std::unique_ptr<em2::HdrScenePipeline> hdr;
    auto prepare=[&](bool capture,bool clear=true){
        surface.reset();ambient.reset();ambient_clear.reset();surface_clear.reset();hdr.reset();source=input;
        if(original && !capture){source.PS={original->data(),original->size()};source.CachedPSO={};}
        const auto* prepared=&source;
        if(em2::CurrentHdrScenePipeline() && source.NumRenderTargets && source.NumRenderTargets<=4 &&
           (source.RTVFormats[0]==DXGI_FORMAT_R8G8B8A8_UNORM || source.RTVFormats[0]==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)){
            hdr=em2::CurrentHdrSceneEncoding()==em2::HdrSceneEncoding::kNativeLinear?em2::HdrScenePipeline::CreateNativeLinear(source):em2::HdrScenePipeline::Create(source);
            if(!hdr && registered && !capture)hdr=em2::HdrScenePipeline::Create(source);
            if(!hdr)return false;prepared=&hdr->desc();
        }
        if(capture){if(surface_program){surface=em2::MaterialLightingSurfacePipeline::Create(*prepared,em2::CurrentLinearMaterialSurfaceCapture());if(!surface)return false;}else{ambient=em2::MaterialAmbientPipeline::Create(*prepared);if(!ambient)return false;}}
        else if(registered && clear && (!hdr || hdr->native_gamma_bypassed()) && em2::MaterialAmbientClearPipeline::Supports(*prepared))ambient_clear=em2::MaterialAmbientClearPipeline::Create(*prepared);
        if(clear && !surface && (ambient || ambient_clear) && em2::CurrentMaterialSurfaceCapture())surface_clear=em2::MaterialSurfaceClearPipeline::Create(ambient?ambient->desc():ambient_clear->desc(),preserve_surface);
        return true;
    };
    if(desc && !prepare(material_capture)){
        if(!original || !material_capture || !prepare(false))return E_INVALIDARG;
        material_capture=false;
    }
    if(original && !material_capture)em2::CountMaterialFallback();
    if(original)em2::WaitMaterialCaptureTestGate();
    if(surface_clear)desc=&surface_clear->desc();else if(surface)desc=&surface->desc();else if(ambient)desc=&ambient->desc();else if(ambient_clear)desc=&ambient_clear->desc();else if(hdr)desc=&hdr->desc();else if(desc)desc=&source;
    if(hdr)em2::CountHdrScenePipelineBuild(hdr->native_gamma_bypassed());
    // Keep the disabled/default path as a direct native tail call. The patched
    // SDK bridge executes vzeroupper before entering; supply its original IID.
    if(!registered){const auto result=!REXCVAR_GET(em2_persistent_pso_cache)?device->CreateGraphicsPipelineState(desc,__uuidof(ID3D12PipelineState),output):CreateCachedPipeline(device,desc,output);if(SUCCEEDED(result) && output && *output)em2::TagGpuProfilePipeline(static_cast<ID3D12PipelineState*>(*output),*desc);return result;}
    HRESULT result=!REXCVAR_GET(em2_persistent_pso_cache)?device->CreateGraphicsPipelineState(desc,__uuidof(ID3D12PipelineState),output):CreateCachedPipeline(device,desc,output);
    if(FAILED(result) && (surface || ambient || ambient_clear || surface_clear)){
        if(material_capture && original)em2::CountMaterialFallback();material_capture=false;
        if(prepare(false,false)){desc=hdr?&hdr->desc():&source;result=!REXCVAR_GET(em2_persistent_pso_cache)?device->CreateGraphicsPipelineState(desc,__uuidof(ID3D12PipelineState),output):CreateCachedPipeline(device,desc,output);}
    }
    if(SUCCEEDED(result) && output && *output){
        auto* pipeline=static_cast<ID3D12PipelineState*>(*output);
        em2::TagGpuProfilePipeline(pipeline,*desc);
        if(surface || ambient || ambient_clear){if((surface || surface_clear) && !em2::StampCurrentMaterialSurfacePipeline(pipeline))REXLOG_WARN("Surface capture PSO tag could not be stored");if(!em2::StampCurrentMaterialPipeline(pipeline))REXLOG_WARN("Material capture PSO tag could not be stored");if(ambient_clear)em2::CountMaterialClearPipeline();}
        else if(!source.NumRenderTargets || source.RTVFormats[0]==DXGI_FORMAT_UNKNOWN || !(source.BlendState.RenderTarget[0].RenderTargetWriteMask&7))em2::StampCurrentMaterialInertPipeline(pipeline);
    }
    if(material_capture && FAILED(result)){REXLOG_ERROR("Native material PSO HRESULT {:08X}",uint32_t(result));ID3D12InfoQueue* queue=nullptr;if(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue)))){const auto count=queue->GetNumStoredMessagesAllowedByRetrievalFilter();for(uint64_t i=count>8?count-8:0;i<count;++i){SIZE_T size=0;queue->GetMessage(i,nullptr,&size);std::vector<uint8_t> bytes(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());if(SUCCEEDED(queue->GetMessage(i,message,&size)))REXLOG_ERROR("Native material validation: {}",message->pDescription);}queue->Release();}}
    return result;
}
namespace {
void BenchmarkPso(ID3D12Device* device,const D3D12_GRAPHICS_PIPELINE_STATE_DESC& original,const std::vector<uint8_t>& blob){
    struct Scope {bool enabled=REXCVAR_GET(em2_persistent_pso_cache);DWORD_PTR affinity;Scope():affinity(SetThreadAffinityMask(GetCurrentThread(),DWORD_PTR(1)<<GetCurrentProcessorNumber())){pso_benchmark_running=true;}~Scope(){REXCVAR_SET(em2_persistent_pso_cache,enabled);pso_benchmark_running=false;if(affinity)SetThreadAffinityMask(GetCurrentThread(),affinity);}} scope;
    using Clock=std::chrono::steady_clock;constexpr unsigned rounds=12,iterations=32;
    auto cached=original;cached.CachedPSO={blob.data(),blob.size()};
    auto create=[&](unsigned mode){ID3D12PipelineState* state=nullptr;HRESULT result;
        if(mode>=2)result=EM2CreateGraphicsPipelineStateCached(device,&original,nullptr,reinterpret_cast<void**>(&state));
        else result=device->CreateGraphicsPipelineState(mode==1?&cached:&original,IID_PPV_ARGS(&state));
        const bool passed=SUCCEEDED(result)&&state;if(state)state->Release();if(!passed)throw std::runtime_error("PSO benchmark creation failed");
    };
    const auto deadline=Clock::now()+std::chrono::milliseconds(250);
    do{for(unsigned i=0;i<8;++i)create(0);}while(Clock::now()<deadline);
    std::array<std::array<double,rounds>,4> times{};
    for(unsigned round=0;round<rounds;++round)for(unsigned order=0;order<4;++order){
        const auto mode=(round+order)%4;REXCVAR_SET(em2_persistent_pso_cache,mode!=3);const auto start=Clock::now();for(unsigned i=0;i<iterations;++i)create(mode);
        times[mode][round]=std::chrono::duration<double,std::micro>(Clock::now()-start).count()/iterations;
    }
    for(auto& row:times)std::sort(row.begin(),row.end());
    Digest root{};UINT root_size=UINT(root.size());
    if(SUCCEEDED(original.pRootSignature->GetPrivateData(root_digest_guid,&root_size,root.data())) && root_size==root.size()) {
        const auto adapter=device->GetAdapterLuid();const auto key=PipelineKey(original,root,adapter);
        std::shared_ptr<em2::PsoBinaryStore> store;{std::lock_guard lock(active_mutex);store=active_store;}
        std::array<double,3> parts{};
        for(unsigned mode=0;mode<3;++mode){const auto start=Clock::now();for(unsigned i=0;i<128;++i){
            if(mode==0){const auto digest=PipelineKey(original,root,adapter);if(digest!=key)throw std::runtime_error("Unstable PSO key");}
            else if(mode==1){if(!store || store->Load(key)!=blob)throw std::runtime_error("PSO component read failed");}
            else{const auto digest=Hash(blob);if(digest==Digest{})throw std::runtime_error("PSO component hash failed");}
        }parts[mode]=std::chrono::duration<double,std::micro>(Clock::now()-start).count()/128;}
        REXLOG_INFO("PSO cache components: key {:.3f}, record read/validate {:.3f}, payload SHA {:.3f} us/call (128 warm synthetic iterations)",parts[0],parts[1],parts[2]);
    }
    REXLOG_INFO("PSO creation benchmark: native {:.3f}, blob-only {:.3f}, full-cache {:.3f}, disabled-gateway {:.3f} us/call; {:.3f}x/{:.3f}x/{:.3f}x (twelve rotating rounds x32, pinned {}, warm synthetic descriptor, includes release; not cold compilation or gameplay FPS)",times[0][6],times[1][6],times[2][6],times[3][6],times[0][6]/times[1][6],times[0][6]/times[2][6],times[0][6]/times[3][6],scope.affinity!=0);
}
}
bool em2::VerifyPsoBinaryGpuTest(){if(!REXCVAR_GET(em2_persistent_pso_cache))return true;const auto s=ReadPsoBinaryStats();REXLOG_INFO("Compiled PSO GPU verification: calls {}, keys {}, saved {}, disk hits {}, accepted {}, unavailable {}",s.calls,s.keys,s.saved,s.hits,s.accepted,s.uncacheable);return s.calls>=3 && s.keys>=2 && ((s.hits && s.accepted) || (!s.saved && s.uncacheable));}

bool em2::TestPsoBinaryCache(const std::filesystem::path& directory){
    try {
        const std::array<uint8_t,3> abc={'a','b','c'};
        if(Hex(Hash(abc))!="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")return false;
        Digest key{};std::vector<uint8_t> payload={0,1,2,3,255};auto encoded=Encode(key,payload);
        if(Decode(key,encoded)!=payload)return false;
        for(size_t i=0;i<encoded.size();++i){auto damaged=encoded;damaged[i]^=1;if(!Decode(key,damaged).empty())return false;}
        if(!Decode(key,std::span(encoded).first(79)).empty())return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};LUID adapter{};Digest root{};const auto first=PipelineKey(d,root,adapter);
        auto changed=d;changed.NodeMask=1;if(first==PipelineKey(changed,root,adapter))return false;
        changed=d;changed.RasterizerState.DepthBiasClamp=std::bit_cast<float>(0x80000000u);if(first==PipelineKey(changed,root,adapter))return false;
        root[0]=1;if(first==PipelineKey(d,root,adapter))return false;root[0]=0;
        adapter.LowPart=1;if(first==PipelineKey(d,root,adapter))return false;
        // Key construction excludes pointers/padding but includes all semantic
        // identities. Exercise representative blend/raster/depth/input/SO and
        // shader fields independently from any real GPU or driver cache.
        auto different=[&](const auto& state){return first!=PipelineKey(state,Digest{},LUID{});};
        for(unsigned field=0;field<15;++field){auto state=d;switch(field){
            case 0:state.BlendState.RenderTarget[7].RenderTargetWriteMask=1;break;
            case 1:state.BlendState.RenderTarget[0].SrcBlend=D3D12_BLEND_ONE;break;
            case 2:state.RasterizerState.CullMode=D3D12_CULL_MODE_BACK;break;
            case 3:state.RasterizerState.DepthBias=-1;break;
            case 4:state.DepthStencilState.BackFace.StencilFailOp=D3D12_STENCIL_OP_KEEP;break;
            case 5:state.DepthStencilState.StencilReadMask=1;break;
            case 6:state.SampleDesc.Count=1;break;
            case 7:state.SampleDesc.Quality=1;break;
            case 8:state.SampleMask=1;break;
            case 9:state.RTVFormats[7]=DXGI_FORMAT_R8G8B8A8_UNORM;break;
            case 10:state.DSVFormat=DXGI_FORMAT_D32_FLOAT;break;
            case 11:state.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;break;
            case 12:state.IBStripCutValue=D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;break;
            case 13:state.Flags=D3D12_PIPELINE_STATE_FLAG_TOOL_DEBUG;break;
            case 14:state.StreamOutput.RasterizedStream=1;break;}
            if(!different(state))return false;
        }
        std::array<uint8_t,4> program={1,2,3,4};changed=d;changed.VS={program.data(),program.size()};const auto shader_key=PipelineKey(changed,Digest{},LUID{});
        if(shader_key==first)return false;program[0]^=1;if(shader_key==PipelineKey(changed,Digest{},LUID{}))return false;
        char name1[]="POSITION",name2[]="POSITION";D3D12_INPUT_ELEMENT_DESC input1{},input2{};input1.SemanticName=name1;input2.SemanticName=name2;
        auto input_state=d;input_state.InputLayout={&input1,1};const auto input_key=PipelineKey(input_state,Digest{},LUID{});input_state.InputLayout.pInputElementDescs=&input2;
        if(input_key!=PipelineKey(input_state,Digest{},LUID{}))return false;input2.AlignedByteOffset=4;if(input_key==PipelineKey(input_state,Digest{},LUID{}))return false;
        changed=d;changed.pRootSignature=reinterpret_cast<ID3D12RootSignature*>(uintptr_t(0x1234));changed.CachedPSO={program.data(),program.size()};if(first!=PipelineKey(changed,Digest{},LUID{}))return false;
        // Driver acceptance/rejection is injected only through this pure helper.
        // The rejection case must clear the blob so a fresh successful pipeline
        // is saved in place of the rejected record, not retried every startup.
        for(unsigned scenario=0;scenario<4;++scenario){std::vector<uint8_t> cached=scenario?payload:std::vector<uint8_t>{};unsigned calls=0;bool valid=true;
            const auto creation=CreateWithFallback(d,cached,[&](const auto& state)->HRESULT{++calls;const bool supplied=state.CachedPSO.CachedBlobSizeInBytes!=0;
                valid&=supplied==(scenario!=0 && calls==1);if(scenario==3)return E_FAIL;if(scenario==2 && calls==1)return E_INVALIDARG;return S_OK;});
            if(!valid || calls!=(scenario>=2?2u:1u) || creation.attempted!=(scenario!=0) || creation.accepted!=(scenario==1) ||
               creation.result!=(scenario==3?E_FAIL:S_OK) || (scenario>=2&&!cached.empty()))return false;
        }
        const auto root_path=directory/"compiled-pso-test";PsoBinaryStore store(root_path);
        store.Save(key,payload);store.Close();if(store.Load(key)!=payload)return false;
        auto other=key;other[0]=1;store.Save(other,payload);if(!store.Load(other).empty())return false;
        {std::ofstream damaged(root_path/(Hex(key)+".pso"),std::ios::binary|std::ios::trunc);damaged.put('x');}
        if(!store.Load(key).empty())return false;
        {PsoBinaryStore repair(root_path);repair.Save(key,payload);repair.Close();if(repair.Load(key)!=payload)return false;}
        REXLOG_INFO("Compiled PSO CPU cache checks passed: record byte corruption/truncation, root/adapter/state/shader/input identity, pointer independence, cached/fresh/rejected/failing creation, queue drain, closed queue and corrupt-file repair");return true;
    }catch(const std::exception& error){REXLOG_ERROR("Compiled PSO cache CPU diagnostic failed: {}",error.what());return false;}
}
