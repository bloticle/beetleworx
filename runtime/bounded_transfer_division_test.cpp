#include "bounded_transfer_division.h"
#include "native_pixel_fixture.h"
#include "rex_render_target/thirdparty/dxbc/DXBCChecksum.h"
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/logging.h>
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <cstring>
#include <fstream>
#include <span>
#include <vector>

namespace em2 {
namespace {
std::vector<uint32_t> DivisionOracleShader(std::span<const uint8_t> source) {
  namespace d = rex::graphics::dxbc;
  std::vector<uint32_t> program{d::VersionToken(d::ProgramType::kComputeShader, 5, 1), 0};
  d::Statistics statistics{};
  d::Assembler a(program, statistics);
  a.OpDclGlobalFlags(d::kGlobalFlagEnableRawAndStructuredBuffers | d::kGlobalFlagAllResourcesBound);
  a.OpDclUnorderedAccessViewRaw(0, d::Src::U(d::Src::Dcl, 0, 0, 0));
  a.OpDclInput(d::Dest::VThreadID(0b0001));
  a.OpDclTemps(4);
  a.OpDclThreadGroup(64, 1, 1);
  a.OpIShL(d::Dest::R(3, 0b0001), d::Src::VThreadID(d::Src::kXXXX), d::Src::LU(4));
  a.OpAnd(d::Dest::R(1, 0b1000), d::Src::VThreadID(d::Src::kXXXX), d::Src::LU(2047));
  a.OpUShR(d::Dest::R(2, 0b0001), d::Src::VThreadID(d::Src::kXXXX), d::Src::LU(11));
  // Both operations execute on the physical GPU with exactly the same input.
  a.OpUDiv(d::Dest::R(0, 0b0001), d::Dest::R(0, 0b0010),
            d::Src::R(1, d::Src::kWWWW), d::Src::R(2, d::Src::kXXXX));
  EmitBoundedTransferDivision(a);
  a.OpMov(d::Dest::R(0, 0b0100), d::Src::R(1, d::Src::kWWWW));
  a.OpMov(d::Dest::R(0, 0b1000), d::Src::R(2, d::Src::kXXXX));
  a.OpStoreRaw(d::Dest::U(0, 0), d::Src::R(3, d::Src::kXXXX), d::Src::R(0));
  a.OpRet();
  program[1] = uint32_t(program.size());

  // Retain FXC's valid compute container metadata and replace only its program.
  // This avoids a test implementation accidentally using HLSL division or a
  // different reciprocal sequence from the actual transfer shader helper.
  if (source.size() < sizeof(d::ContainerHeader)) return {};
  const auto* input = reinterpret_cast<const d::ContainerHeader*>(source.data());
  const auto* offsets = reinterpret_cast<const uint32_t*>(source.data() + sizeof(*input));
  std::vector<uint32_t> output(sizeof(*input) / 4 + input->blob_count);
  bool replaced = false;
  for (uint32_t i = 0; i < input->blob_count; ++i) {
    output[sizeof(*input) / 4 + i] = uint32_t(output.size() * 4);
    const auto* blob = reinterpret_cast<const d::BlobHeader*>(source.data() + offsets[i]);
    if (blob->fourcc == d::BlobHeader::FourCC::kShaderEx) {
      const d::BlobHeader replacement{blob->fourcc, uint32_t(program.size() * 4)};
      const auto* header = reinterpret_cast<const uint32_t*>(&replacement);
      output.insert(output.end(), header, header + 2);
      output.insert(output.end(), program.begin(), program.end());
      replaced = true;
    } else {
      const auto* data = reinterpret_cast<const uint32_t*>(blob);
      output.insert(output.end(), data, data + 2 + blob->size_bytes / 4);
    }
  }
  if (!replaced) return {};
  auto* header = reinterpret_cast<d::ContainerHeader*>(output.data());
  header->InitializeIdentification();
  header->size_bytes = uint32_t(output.size() * 4);
  header->blob_count = input->blob_count;
  std::memset(header->hash, 0, sizeof(header->hash));
  CalculateDXBCChecksum(reinterpret_cast<unsigned char*>(output.data()), header->size_bytes, header->hash);
  return output;
}
}

bool TestBoundedTransferDivision(const rex::ui::d3d12::D3D12Provider& provider,
                                const std::filesystem::path& output) {
  using namespace pixel_test;
  constexpr uint32_t pairs = 2048 * 1024;
  constexpr uint64_t bytes = uint64_t(pairs) * 16;
  auto* device = provider.GetDevice();
  auto* queue = provider.GetDirectQueue();
  ComPtr<ID3D12InfoQueue> validation;
  device->QueryInterface(IID_PPV_ARGS(&validation));
  const auto first = validation ? validation->GetNumStoredMessagesAllowedByRetrievalFilter() : 0;
  const auto compiler = LoadLibraryW(L"d3dcompiler_47.dll");
  if (!compiler) return false;
  struct Library { HMODULE handle; ~Library() { FreeLibrary(handle); } } library{compiler};
  const auto compile = reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler, "D3DCompile"));
  if (!compile) return false;
  constexpr char stub[] = "RWByteAddressBuffer values:register(u0);[numthreads(64,1,1)]void main(uint3 id:SV_DispatchThreadID){values.Store4(id.x*16,uint4(id.x,id.x/3,id.x%3,id.x+1));}";
  ComPtr<ID3DBlob> original, errors;
  if (FAILED(compile(stub, sizeof(stub)-1, "bounded-transfer-division-oracle", nullptr, nullptr,
                     "main", "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &original, &errors))) return false;
  const auto code = DivisionOracleShader({static_cast<const uint8_t*>(original->GetBufferPointer()), original->GetBufferSize()});
  if (code.empty()) return false;
  Fixture fixture;
  if (!fixture.event || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&fixture.allocator))) ||
      FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, fixture.allocator.Get(), nullptr, IID_PPV_ARGS(&fixture.list))) ||
      FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fixture.fence)))) return false;
  D3D12_ROOT_PARAMETER parameter{};
  parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  parameter.Descriptor = {0, 0};
  D3D12_ROOT_SIGNATURE_DESC root{1, &parameter};
  ComPtr<ID3DBlob> serialized;
  if (FAILED(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
      FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&fixture.root)))) return false;
  D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
  pipeline.pRootSignature = fixture.root.Get();
  pipeline.CS = {code.data(), code.size()*4};
  if (FAILED(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&fixture.pipelines[0])))) return false;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = bytes;
  desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&fixture.target))) ||
      !Buffer(device, bytes, D3D12_HEAP_TYPE_READBACK, &fixture.readback)) return false;
  fixture.list->SetComputeRootSignature(fixture.root.Get());
  fixture.list->SetComputeRootUnorderedAccessView(0, fixture.target->GetGPUVirtualAddress());
  fixture.list->SetPipelineState(fixture.pipelines[0].Get());
  fixture.list->Dispatch(pairs/64, 1, 1);
  Barrier(fixture.list.Get(), fixture.target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  fixture.list->CopyBufferRegion(fixture.readback.Get(), 0, fixture.target.Get(), 0, bytes);
  if (FAILED(fixture.list->Close())) return false;
  ID3D12CommandList* lists[] = {fixture.list.Get()};
  queue->ExecuteCommandLists(1, lists);
  if (FAILED(queue->Signal(fixture.fence.Get(), 1)) ||
      FAILED(fixture.fence->SetEventOnCompletion(1, fixture.event)) || WaitForSingleObject(fixture.event, 30000) != WAIT_OBJECT_0) return false;
  void* mapped = nullptr;
  D3D12_RANGE range{0, bytes}, none{};
  if (FAILED(fixture.readback->Map(0, &range, &mapped))) return false;
  const auto* values = static_cast<const uint32_t*>(mapped);
  uint64_t invalid = 0, original_invalid = 0;
  for (uint32_t i = 0; i < pairs; ++i) {
    const auto n = i & 2047, divisor = i >> 11;
    const auto q = divisor ? n/divisor : UINT32_MAX;
    const auto r = divisor ? n%divisor : UINT32_MAX;
    original_invalid += values[4*i] != q || values[4*i+1] != r;
    invalid += values[4*i+2] != values[4*i] || values[4*i+3] != values[4*i+1];
  }
  fixture.readback->Unmap(0, &none);
  uint64_t device_errors = 0;
  if (validation) for (uint64_t i = first; i < validation->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
    SIZE_T size = 0;
    validation->GetMessage(i, nullptr, &size);
    std::vector<uint8_t> storage(size);
    auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
    if (SUCCEEDED(validation->GetMessage(i, message, &size)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
      ++device_errors;
      REXLOG_ERROR("Bounded transfer division GPU: {}", message->pDescription);
    }
  }
  std::filesystem::create_directories(output);
  std::ofstream report(output/"bounded-transfer-division.json");
  report << "{\"pairs\":" << pairs << ",\"invalid_vs_original_udiv\":" << invalid
         << ",\"original_invalid_vs_cpu\":" << original_invalid << ",\"device_errors\":" << device_errors
         << ",\"scope\":\"Actual original DXBC UDiv and identical transfer helper run side by side on GPU for all wrapped tile indices 0..2047 and all 10-bit pitches 0..1023, including zero pitch. Both quotient and remainder compared with integer CPU oracle.\"}\n";
  REXLOG_INFO("Bounded transfer division GPU: {} pairs, {} invalid versus original UDiv, {} original invalid versus CPU, {} device errors", pairs, invalid, original_invalid, device_errors);
  return !invalid && !original_invalid && !device_errors && report.good();
}
}
