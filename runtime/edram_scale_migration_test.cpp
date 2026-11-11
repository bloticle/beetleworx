#include "scaled_resolve_migration.h"
#include "native_pixel_fixture.h"
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/graphics/util/draw.h>
#include <rex/logging.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <vector>
namespace {
#include "edram_migration_reference.h"
using namespace em2::pixel_test;
constexpr uint32_t tile_bytes=5120;
struct NativeClear {
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>,4> pipelines;
    bool Initialize(ID3D12Device* device) {
        const D3D12_DESCRIPTOR_RANGE range={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0};
        std::array<D3D12_ROOT_PARAMETER,2> parameters{};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[0].Constants={0,0,4};
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[1].DescriptorTable={1,&range};
        D3D12_ROOT_SIGNATURE_DESC description{2,parameters.data(),0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> serialized,errors;
        if(FAILED(D3D12SerializeRootSignature(&description,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors)) ||
           FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root))))return false;
        const std::array<D3D12_SHADER_BYTECODE,4> code={{{resolve_clear_32bpp_cs,sizeof(resolve_clear_32bpp_cs)},
            {resolve_clear_32bpp_scaled_cs,sizeof(resolve_clear_32bpp_scaled_cs)},
            {resolve_clear_64bpp_cs,sizeof(resolve_clear_64bpp_cs)},
            {resolve_clear_64bpp_scaled_cs,sizeof(resolve_clear_64bpp_scaled_cs)}}};
        for(unsigned i=0;i<code.size();++i) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};pso.pRootSignature=root.Get();pso.CS=code[i];
            if(FAILED(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&pipelines[i]))))return false;
        }
        return true;
    }
    void Clear(ID3D12GraphicsCommandList* list,D3D12_GPU_DESCRIPTOR_HANDLE view,uint32_t sx,uint32_t sy,
        bool wide,bool depth,unsigned msaa,unsigned tile,unsigned rectangle) const {
        namespace g=rex::graphics;
        g::draw_util::ResolveEdramInfo info;
        info.pitch_tiles=1;info.base_tiles=tile;
        info.msaa_samples=static_cast<g::xenos::MsaaSamples>(msaa);info.is_depth=depth;info.format_is_64bpp=wide;
        g::draw_util::ResolveCoordinateInfo coordinates;
        coordinates.edram_offset_x_div_8=rectangle==0?1:0;
        coordinates.edram_offset_y_div_8=rectangle==2?1:0;
        coordinates.width_div_8=1;coordinates.draw_resolution_scale_x=sx;coordinates.draw_resolution_scale_y=sy;
        const uint32_t first=0x7FC12345u^(tile*0x11223344u)^(rectangle*0xC1D2E3F4u);
        const uint32_t second=0x80000001u^(tile*0x17439265u)^(rectangle*0x10203040u);
        const std::array<uint32_t,4> constants={first,second,info.packed,coordinates.packed};
        list->SetComputeRootSignature(root.Get());list->SetPipelineState(pipelines[(wide?2:0)+((sx>1 || sy>1)?1:0)].Get());
        list->SetComputeRoot32BitConstants(0,4,constants.data(),0);list->SetComputeRootDescriptorTable(1,view);
        // Each thread clears eight samples horizontally. The native shader
        // bounds X, but not Y; our 8-pixel-high regions dispatch exact rows.
        const uint32_t msaa_x=msaa==2?2:1,msaa_y=msaa?2:1;
        list->Dispatch((sx*msaa_x+7)/8,sy*msaa_y,1);
    }
};
bool Wide(unsigned kind,unsigned tile,unsigned total){return kind==2 || (kind==3 && tile>=total/2);}
bool Depth(unsigned kind,unsigned tile){return kind==1 || (kind==3 && tile==1);}
void UavBarrier(ID3D12GraphicsCommandList* list,ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;barrier.UAV.pResource=resource;
    list->ResourceBarrier(1,&barrier);
}
uint32_t Pattern(unsigned tile,unsigned x,unsigned y,unsigned component) {
    uint32_t value=0x7FC12345u^(tile*0x45D9F3Bu)^(x*0x119DE1F3u)^(y*0x27D4EB2Du)^(component*0x9E3779B9u);
    value^=value>>16;value*=0x85EBCA6Bu;return value^(value>>13);
}
}
bool em2::TestEdramScaleMigration(rex::ui::d3d12::D3D12Provider& provider) {
    auto* device=provider.GetDevice();auto* queue=provider.GetDirectQueue();
    auto migrator=std::make_shared<ScaledResolveMigrator>();
    NativeClear native;if(!migrator->Initialize(device) || !native.Initialize(device))return false;
    const std::array<std::array<uint32_t,4>,8> transitions={{{1,1,2,2},{2,2,3,3},{3,3,1,1},{3,3,2,2},
        {2,2,1,1},{1,1,3,3},{1,2,3,1},{3,1,2,3}}};
    unsigned tested=0;
    for(unsigned full=0;full<2;++full)for(const auto& scales:transitions)for(unsigned kind=0;kind<4;++kind)for(unsigned msaa=0;msaa<3;++msaa) {
        // One complete 10 MiB native EDRAM image exercises both maximum tile
        // count and the second dispatch dimension, with mixed pixel widths.
        if(full && (scales!=transitions[1] || kind!=3 || msaa!=2))continue;
        const uint32_t tiles=full?2048:4;
        const auto sx=scales[0],sy=scales[1],dx=scales[2],dy=scales[3];
        const size_t source_bytes=tiles*tile_bytes*sx*sy,dest_bytes=tiles*tile_bytes*dx*dy;
        std::vector<uint32_t> source(source_bytes/4+8,0xA7A7A7A7),expected(dest_bytes/4+8,0x5D5D5D5D);
        for(unsigned tile=0;tile<tiles;++tile) {
            const unsigned words=Wide(kind,tile,tiles)?2:1,width=80*sx/words,height=16*sy;
            // A logical sample image with independent opaque words. Encode it
            // into the source, then independently sample that image for the
            // expected destination. This detects channel-pair mixing and phase
            // errors in addition to the native clear-kernel comparisons below.
            for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)for(unsigned c=0;c<words;++c)
                source[4+tile*1280*sx*sy+y*80*sx+x*words+c]=Pattern(tile,x,y,c);
            for(unsigned y=0;y<16*dy;++y)for(unsigned x=0;x<80*dx/words;++x)for(unsigned c=0;c<words;++c)
                expected[4+tile*1280*dx*dy+y*80*dx+x*words+c]=Pattern(tile,
                    ((2*(x/(msaa==2?2:1))+1)*sx/(2*dx))*(msaa==2?2:1)+x%(msaa==2?2:1),
                    ((2*(y/(msaa?2:1))+1)*sy/(2*dy))*(msaa?2:1)+y%(msaa?2:1),c);
        }
        auto f=std::make_unique<Fixture>();
        if(!f->event || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f->allocator))) ||
           FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f->allocator.Get(),nullptr,IID_PPV_ARGS(&f->list))) ||
           FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f->fence))) ||
           !Buffer(device,source_bytes+32+dest_bytes+32,D3D12_HEAP_TYPE_UPLOAD,&f->upload) ||
           !Buffer(device,2*(dest_bytes+32),D3D12_HEAP_TYPE_READBACK,&f->readback))return false;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=source_bytes+32;
        desc.Height=desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
        desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&f->textures[0]))))return false;
        desc.Width=dest_bytes+32;
        for(auto* output:{f->target.ReleaseAndGetAddressOf(),f->textures[1].ReleaseAndGetAddressOf()})
            if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(output))))return false;
        void* mapped=nullptr;D3D12_RANGE none{0,0};
        if(FAILED(f->upload->Map(0,&none,&mapped)))return false;
        std::memcpy(mapped,source.data(),source_bytes+32);
        std::memcpy(static_cast<uint8_t*>(mapped)+source_bytes+32,expected.data(),dest_bytes+32);
        f->upload->Unmap(0,nullptr);
        f->list->CopyBufferRegion(f->textures[0].Get(),0,f->upload.Get(),0,source_bytes+32);
        f->list->CopyBufferRegion(f->target.Get(),0,f->upload.Get(),source_bytes+32,dest_bytes+32);
        f->list->CopyBufferRegion(f->textures[1].Get(),0,f->upload.Get(),source_bytes+32,dest_bytes+32);
        for(auto* resource:{f->textures[0].Get(),f->target.Get(),f->textures[1].Get()})
            Barrier(f->list.Get(),resource,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        D3D12_DESCRIPTOR_HEAP_DESC views{};views.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        views.NumDescriptors=2;views.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if(FAILED(device->CreateDescriptorHeap(&views,IID_PPV_ARGS(&f->views))))return false;
        auto cpu=f->views->GetCPUDescriptorHandleForHeapStart();
        const auto stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for(unsigned which=0;which<2;++which) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{};view.Format=DXGI_FORMAT_R32G32B32A32_UINT;
            view.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;view.Buffer.FirstElement=1;
            view.Buffer.NumElements=uint32_t((which?dest_bytes:source_bytes)/16);
            device->CreateUnorderedAccessView(f->textures[which].Get(),nullptr,&view,cpu);cpu.ptr+=stride;
        }
        ID3D12DescriptorHeap* heaps[]={f->views.Get()};f->list->SetDescriptorHeaps(1,heaps);
        auto gpu=f->views->GetGPUDescriptorHandleForHeapStart();
        for(unsigned which=0;which<2;++which) {
            for(unsigned tile=0;tile<tiles;++tile)for(unsigned rectangle=0;rectangle<(msaa?2u:3u);++rectangle) {
                if(full && tile>1 && tile!=1023 && tile!=1024 && tile<2046)continue;
                native.Clear(f->list.Get(),gpu,which?dx:sx,which?dy:sy,Wide(kind,tile,tiles),Depth(kind,tile),msaa,tile,rectangle);
                UavBarrier(f->list.Get(),f->textures[which].Get());
            }
            gpu.ptr+=stride;
        }
        Barrier(f->list.Get(),f->textures[0].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const auto source_area=sx*sy,dest_area=dx*dy;
        EdramScaleMigration invalid{sx,sy,dx,dy,0,false};
        if(migrator->RecordEdram(f->list.Get(),f->textures[0].Get(),16,f->target.Get(),16,invalid))return false;
        invalid.tile_count=2049;
        if(migrator->RecordEdram(f->list.Get(),f->textures[0].Get(),16,f->target.Get(),16,invalid))return false;
        for(unsigned begin=0;begin<tiles;) {
            unsigned end=begin+1;while(end<tiles && Wide(kind,end,tiles)==Wide(kind,begin,tiles))++end;
            const EdramScaleMigration migration{sx,sy,dx,dy,end-begin,Wide(kind,begin,tiles),msaa==2?2u:1u,msaa?2u:1u};
            if(!migrator->RecordEdram(f->list.Get(),f->textures[0].Get(),16+begin*tile_bytes*source_area,
                f->target.Get(),16+begin*tile_bytes*dest_area,migration))return false;
            UavBarrier(f->list.Get(),f->target.Get());begin=end;
        }
        for(auto* resource:{f->target.Get(),f->textures[1].Get()})
            Barrier(f->list.Get(),resource,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        f->list->CopyBufferRegion(f->readback.Get(),0,f->target.Get(),0,dest_bytes+32);
        f->list->CopyBufferRegion(f->readback.Get(),dest_bytes+32,f->textures[1].Get(),0,dest_bytes+32);
        if(FAILED(f->list->Close()))return false;
        ID3D12CommandList* lists[]={f->list.Get()};queue->ExecuteCommandLists(1,lists);
        if(FAILED(queue->Signal(f->fence.Get(),1)) || FAILED(f->fence->SetEventOnCompletion(1,f->event)) ||
           WaitForSingleObject(f->event,10000)!=WAIT_OBJECT_0) {
            // Retain every submitted object, including shader/root owners.
            f->root=native.root;for(unsigned i=0;i<4;++i)f->pipelines[i]=native.pipelines[i];
            using Pending=std::pair<std::unique_ptr<Fixture>,std::shared_ptr<ScaledResolveMigrator>>;
            static auto* pending=new std::vector<Pending>;pending->emplace_back(std::move(f),migrator);return false;
        }
        D3D12_RANGE range{0,2*(dest_bytes+32)};if(FAILED(f->readback->Map(0,&range,&mapped)))return false;
        const auto* actual=static_cast<const uint8_t*>(mapped);
        bool equal=!std::memcmp(actual,actual+dest_bytes+32,dest_bytes+32);
        for(unsigned i=0;i<16;++i)equal&=actual[i]==0x5D && actual[dest_bytes+16+i]==0x5D;
        f->readback->Unmap(0,&none);
        if(!equal){REXLOG_ERROR("EDRAM scale migration mismatch: {}x{} -> {}x{} kind={} msaa={}",sx,sy,dx,dy,kind,msaa);return false;}
        ++tested;
    }
    REXLOG_INFO("EDRAM GPU scale migration passed: {} cases, native scaled/unscaled clear kernels, 32/64-bit pixels, depth bank swap, 1x/2x/4x MSAA, mixed tile ranges, full 2048-tile image and 2D dispatch, opaque words and guards; live cache migration remains unfinished",tested);
    return true;
}
