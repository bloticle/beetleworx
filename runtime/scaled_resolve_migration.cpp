#include "built_shaders.h"
#include "scaled_resolve_migration.h"
#include "native_pixel_fixture.h"
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/logging.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace {
#include "scaled_resolve_reference.h"
// The SDK's scaled 16/32/64/128-bit texture loaders interleave 16-byte groups
// (8-byte groups for its 8-bit loader):
// native_group * (sx*sy) + phase_x*sy + phase_y. Within each group texels are
// sequential. Preserve opaque texel bits, including depth/stencil and NaNs;
// nearest sampling deliberately performs no color or floating-point conversion.

bool Valid(const em2::ScaledResolveMigration& p) {
    auto scale=[](uint32_t v){return v>=1 && v<=7;};
    return scale(p.source_x) && scale(p.source_y) && scale(p.destination_x) && scale(p.destination_y) &&
        (p.bytes_per_texel==1 || p.bytes_per_texel==2 || p.bytes_per_texel==4 || p.bytes_per_texel==8 || p.bytes_per_texel==16) &&
        p.native_bytes && !(p.native_bytes&(p.bytes_per_texel==1?7:15)) &&
        uint64_t(p.native_bytes)*p.source_x*p.source_y<=UINT32_MAX &&
        uint64_t(p.native_bytes)*p.destination_x*p.destination_y<=UINT32_MAX;
}
}
bool em2::ScaledResolveMigrator::Initialize(ID3D12Device* device) {
    if(!device)return false;
    
    ComPtr<ID3DBlob> errors,serialized;
    const auto* code=em2::GetBuiltShader(em2::BuiltShader::ScaledResolve);if(!code)return false;
    std::array<D3D12_ROOT_PARAMETER,3> parameters{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants={0,0,11};
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC description{3,parameters.data(),0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    if(FAILED(D3D12SerializeRootSignature(&description,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors)) ||
       FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root))))return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};pso.pRootSignature=root.Get();
    pso.CS={code->GetBufferPointer(),code->GetBufferSize()};
    if(FAILED(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&pipeline))))return false;
    root_=std::move(root);pipeline_=std::move(pipeline);return true;
}
bool em2::ScaledResolveMigrator::Record(ID3D12GraphicsCommandList* list,ID3D12Resource* source,uint64_t source_offset,
    ID3D12Resource* destination,uint64_t destination_offset,const ScaledResolveMigration& p) const {
    return RecordStorage(list,source,source_offset,destination,destination_offset,p,0);
}
bool em2::ScaledResolveMigrator::RecordEdram(ID3D12GraphicsCommandList* list,ID3D12Resource* source,uint64_t source_offset,
    ID3D12Resource* destination,uint64_t destination_offset,const EdramScaleMigration& p) const {
    if(!p.tile_count || p.tile_count>2048)return false;
    const ScaledResolveMigration storage{p.source_x,p.source_y,p.destination_x,p.destination_y,
        p.pixels_are_64_bit?8u:4u,p.tile_count*5120};
    return RecordStorage(list,source,source_offset,destination,destination_offset,storage,p.pixels_are_64_bit?2:1,0,p.samples_x,p.samples_y);
}
bool em2::ScaledResolveMigrator::RecordEdramLinear(ID3D12GraphicsCommandList* list,ID3D12Resource* source,uint64_t source_offset,
    ID3D12Resource* destination,const EdramScaleMigration& p,uint32_t first_tile) const {
    if(!p.tile_count || p.tile_count>2048 || first_tile>2048-p.tile_count)return false;
    const ScaledResolveMigration storage{p.source_x,p.source_y,p.destination_x,p.destination_y,
        p.pixels_are_64_bit?8u:4u,p.tile_count*5120};
    return RecordStorage(list,source,source_offset,destination,0,storage,p.pixels_are_64_bit?4:3,first_tile,p.samples_x,p.samples_y);
}
bool em2::ScaledResolveMigrator::RecordStorage(ID3D12GraphicsCommandList* list,ID3D12Resource* source,uint64_t source_offset,
    ID3D12Resource* destination,uint64_t destination_offset,const ScaledResolveMigration& p,uint32_t layout,uint32_t first_tile,uint32_t samples_x,uint32_t samples_y) const {
    if(!root_ || !pipeline_ || !list || !source || !destination || source==destination ||
       (source_offset&15) || (destination_offset&15) || !Valid(p) ||
       (samples_x!=1 && samples_x!=2) || (samples_y!=1 && samples_y!=2) || samples_x>samples_y)return false;
    const auto a=source->GetDesc(),b=destination->GetDesc();
    const uint64_t source_bytes=uint64_t(p.native_bytes)*p.source_x*p.source_y;
    const uint64_t destination_bytes=uint64_t(p.native_bytes)*p.destination_x*p.destination_y;
    if(a.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER || b.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER ||
       (a.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) || !(b.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
       source_offset>a.Width || source_bytes>a.Width-source_offset ||
       destination_offset>b.Width || (layout>=3?uint64_t(2048*5120)*p.destination_x*p.destination_y:destination_bytes)>b.Width-destination_offset)return false;
    const uint32_t count=uint32_t(destination_bytes/4);
    const uint32_t groups=(count+63)/64,columns=std::min(groups,65535u),rows=(groups+columns-1)/columns;
    const std::array<uint32_t,11> constants={p.source_x,p.source_y,p.destination_x,p.destination_y,p.bytes_per_texel,count,columns*64,layout,first_tile,samples_x,samples_y};
    list->SetComputeRootSignature(root_.Get());list->SetPipelineState(pipeline_.Get());
    list->SetComputeRoot32BitConstants(0,11,constants.data(),0);
    list->SetComputeRootShaderResourceView(1,source->GetGPUVirtualAddress()+source_offset);
    list->SetComputeRootUnorderedAccessView(2,destination->GetGPUVirtualAddress()+destination_offset);
    list->Dispatch(columns,rows,1);return true;
}

bool em2::TestScaledResolveMigration(rex::ui::d3d12::D3D12Provider& provider) {
    using namespace pixel_test;
    auto* device=provider.GetDevice();auto* queue=provider.GetDirectQueue();
    auto migrator=std::make_shared<ScaledResolveMigrator>();if(!migrator->Initialize(device))return false;
    unsigned tested=0;
    const std::array<std::array<uint32_t,4>,8> transitions={{{1,1,2,2},{2,2,3,3},{3,3,1,1},{3,3,2,2},
        {2,2,1,1},{1,1,3,3},{1,2,3,1},{3,1,2,3}}};
    for(const auto& scales:transitions)for(uint32_t bpp:{1u,2u,4u,8u,16u}) {
        const ScaledResolveMigration p{scales[0],scales[1],scales[2],scales[3],bpp,16*257};
        const size_t source_bytes=size_t(p.native_bytes)*p.source_x*p.source_y;
        const size_t dest_bytes=size_t(p.native_bytes)*p.destination_x*p.destination_y;
        std::vector<uint8_t> source(source_bytes+32,0xA7),expected(dest_bytes+32,0x5D);
        // Independent texel-space oracle: traverse each native group as an
        // expanded 2D image, then encode its phase/texel storage separately.
        const uint32_t unit=bpp==1?8:16,lanes=unit/bpp,group_count=p.native_bytes/unit;
        auto offset=[&](unsigned group,unsigned x,unsigned y,unsigned sx,unsigned sy,unsigned byte) {
            return 16+size_t(group)*unit*sx*sy+((x/lanes)*sy+y)*unit+(x%lanes)*bpp+byte;
        };
        for(unsigned group=0;group<group_count;++group)for(unsigned y=0;y<p.source_y;++y)
            for(unsigned x=0;x<lanes*p.source_x;++x)for(unsigned byte=0;byte<bpp;++byte) {
                uint32_t hash=(group*0x45D9F3Bu)^(x*0x119DE1F3u)^(y*0x27D4EB2Du)^(byte*0x9E3779B9u);
                hash^=hash>>16;source[offset(group,x,y,p.source_x,p.source_y,byte)]=uint8_t(hash);
            }
        for(unsigned group=0;group<group_count;++group)for(unsigned y=0;y<p.destination_y;++y)
            for(unsigned x=0;x<lanes*p.destination_x;++x)for(unsigned byte=0;byte<bpp;++byte)
                expected[offset(group,x,y,p.destination_x,p.destination_y,byte)]=source[offset(group,
                    (2*x+1)*p.source_x/(2*p.destination_x),(2*y+1)*p.source_y/(2*p.destination_y),p.source_x,p.source_y,byte)];
        auto f=std::make_unique<Fixture>();
        if(!f->event || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f->allocator))) ||
           FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f->allocator.Get(),nullptr,IID_PPV_ARGS(&f->list))) ||
           FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f->fence))) ||
           !Buffer(device,source.size()+expected.size(),D3D12_HEAP_TYPE_UPLOAD,&f->upload) ||
           !Buffer(device,expected.size(),D3D12_HEAP_TYPE_READBACK,&f->readback))return false;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        auto desc=f->readback->GetDesc();desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&f->target))))return false;
        void* mapped=nullptr;D3D12_RANGE none{0,0};
        if(FAILED(f->upload->Map(0,&none,&mapped)))return false;
        std::memcpy(mapped,source.data(),source.size());std::memset(static_cast<uint8_t*>(mapped)+source.size(),0x5D,expected.size());
        f->upload->Unmap(0,nullptr);
        f->list->CopyBufferRegion(f->target.Get(),0,f->upload.Get(),source.size(),expected.size());
        Barrier(f->list.Get(),f->target.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto bad=p;bad.bytes_per_texel=3;
        if(migrator->Record(f->list.Get(),f->upload.Get(),16,f->target.Get(),16,bad) ||
           migrator->Record(f->list.Get(),f->upload.Get(),1,f->target.Get(),16,p) ||
           migrator->Record(f->list.Get(),f->upload.Get(),16,f->target.Get(),expected.size(),p) ||
           !migrator->Record(f->list.Get(),f->upload.Get(),16,f->target.Get(),16,p))return false;
        Barrier(f->list.Get(),f->target.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        f->list->CopyBufferRegion(f->readback.Get(),0,f->target.Get(),0,expected.size());
        // Decode a real tiled 32x32 texture using the SDK's untouched compiled
        // scaled loader. This validates the migration's storage convention
        // against native code, beyond the separate texel-space CPU oracle.
        const uint32_t width=32*p.destination_x,height=32*p.destination_y;
        const uint64_t native_output_bytes=uint64_t(width)*height*4+32;
        if(bpp==4) {
            D3D12_DESCRIPTOR_HEAP_DESC views{};views.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            views.NumDescriptors=2;views.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if(FAILED(device->CreateDescriptorHeap(&views,IID_PPV_ARGS(&f->views))))return false;
            auto native_desc=desc;native_desc.Width=native_output_bytes;
            if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&native_desc,D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,IID_PPV_ARGS(&f->textures[0]))) ||
               !Buffer(device,native_output_bytes,D3D12_HEAP_TYPE_READBACK,&f->textures[1]))return false;
            f->list->CopyBufferRegion(f->textures[0].Get(),0,f->upload.Get(),source.size(),native_output_bytes);
            Barrier(f->list.Get(),f->textures[0].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Barrier(f->list.Get(),f->target.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            const D3D12_DESCRIPTOR_RANGE ranges[2]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0}};
            std::array<D3D12_ROOT_PARAMETER,3> roots{};
            roots[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;roots[0].Constants={0,0,10};
            for(unsigned i=0;i<2;++i){roots[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;roots[i+1].DescriptorTable={1,&ranges[i]};}
            D3D12_ROOT_SIGNATURE_DESC root_desc{3,roots.data(),0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
            ComPtr<ID3DBlob> serialized,errors;
            if(FAILED(D3D12SerializeRootSignature(&root_desc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors)) ||
               FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&f->root))))return false;
            D3D12_COMPUTE_PIPELINE_STATE_DESC native_pso{};native_pso.pRootSignature=f->root.Get();
            native_pso.CS={texture_load_32bpb_scaled_cs,sizeof(texture_load_32bpb_scaled_cs)};
            if(FAILED(device->CreateComputePipelineState(&native_pso,IID_PPV_ARGS(&f->pipelines[0]))))return false;
            auto cpu=f->views->GetCPUDescriptorHandleForHeapStart();auto gpu=f->views->GetGPUDescriptorHandleForHeapStart();
            const auto stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R32G32B32A32_UINT;
            srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Buffer.NumElements=uint32_t(expected.size()/16);device->CreateShaderResourceView(f->target.Get(),&srv,cpu);
            cpu.ptr+=stride;D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R32G32B32A32_UINT;
            uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;uav.Buffer.NumElements=uint32_t(native_output_bytes/16);
            device->CreateUnorderedAccessView(f->textures[0].Get(),nullptr,&uav,cpu);
            ID3D12DescriptorHeap* heaps[]={f->views.Get()};f->list->SetDescriptorHeaps(1,heaps);
            f->list->SetComputeRootSignature(f->root.Get());f->list->SetPipelineState(f->pipelines[0].Get());
            const std::array<uint32_t,10> constants={1u|(p.destination_x<<4)|(p.destination_y<<7),16,32,32,width,height,1,16,width*4,height};
            f->list->SetComputeRoot32BitConstants(0,10,constants.data(),0);
            f->list->SetComputeRootDescriptorTable(1,gpu);gpu.ptr+=stride;f->list->SetComputeRootDescriptorTable(2,gpu);
            f->list->Dispatch((width+31)/32,(height+31)/32,1);
            Barrier(f->list.Get(),f->textures[0].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
            f->list->CopyBufferRegion(f->textures[1].Get(),0,f->textures[0].Get(),0,native_output_bytes);
        }
        if(FAILED(f->list->Close()))return false;
        ID3D12CommandList* lists[]={f->list.Get()};queue->ExecuteCommandLists(1,lists);
        if(FAILED(queue->Signal(f->fence.Get(),1)) || FAILED(f->fence->SetEventOnCompletion(1,f->event)) ||
           WaitForSingleObject(f->event,10000)!=WAIT_OBJECT_0) {
            // Keep all in-flight objects alive until process exit on a driver
            // failure. The diagnostic cannot safely free them on a timeout.
            using Pending=std::pair<std::unique_ptr<Fixture>,std::shared_ptr<ScaledResolveMigrator>>;
            static auto* pending=new std::vector<Pending>;
            pending->emplace_back(std::move(f),migrator);return false;
        }
        D3D12_RANGE range{0,expected.size()};if(FAILED(f->readback->Map(0,&range,&mapped)))return false;
        const bool equal=!std::memcmp(mapped,expected.data(),expected.size());f->readback->Unmap(0,&none);
        if(!equal){REXLOG_ERROR("Scaled resolve migration mismatch: {}x{} -> {}x{}, {} bytes/texel",p.source_x,p.source_y,p.destination_x,p.destination_y,bpp);return false;}
        if(bpp==4) {
            D3D12_RANGE native_range{0,size_t(native_output_bytes)};
            if(FAILED(f->textures[1]->Map(0,&native_range,&mapped)))return false;
            const auto* bytes=static_cast<const uint8_t*>(mapped);bool native_equal=true;
            for(unsigned i=0;i<16;++i)native_equal&=bytes[i]==0x5D && bytes[native_output_bytes-16+i]==0x5D;
            for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
                const auto native_address=rex::graphics::texture_util::GetTiledOffset2D(int32_t((x/(4*p.destination_x))*4),int32_t(y/p.destination_y),32,2);
                const auto address=16+native_address*p.destination_x*p.destination_y+
                    (((x/4)%p.destination_x)*p.destination_y+y%p.destination_y)*16+(x%4)*4;
                native_equal&=!std::memcmp(bytes+16+(y*width+x)*4,expected.data()+address,4);
            }
            f->textures[1]->Unmap(0,&none);
            if(!native_equal){REXLOG_ERROR("SDK scaled texture loader rejected migrated layout {}x{} -> {}x{}",p.source_x,p.source_y,p.destination_x,p.destination_y);return false;}
        }
        ++tested;
    }
    REXLOG_INFO("Scaled resolve GPU migration passed: {} format/scale cases, 8 real SDK tiled-texture decodes, opaque texel bits, asymmetric scales, range/alignment rejection and untouched guards; not yet wired to live render scale",tested);
    return true;
}
