/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/ui/d3d12/d3d12_upload_buffer_pool.h>

#include <rex/logging.h>
#include <rex/math.h>
#include "upload_buffer_pool_test.h"
#include <array>
#include <cstring>
#include <rex/ui/d3d12/d3d12_util.h>

namespace {
// Scoped to the isolated diagnostic thread; normal pool users always call Map.
struct UploadMapProbe {
  HRESULT failure = S_OK;
  Microsoft::WRL::ComPtr<ID3D12Resource> retained;
  ULONG expected_refs = 0;
  unsigned calls = 0;
};
thread_local UploadMapProbe* upload_map_probe = nullptr;
HRESULT MapUpload(ID3D12Resource* resource, const D3D12_RANGE* range, void** mapping) {
  if (auto* probe = upload_map_probe) {
    ++probe->calls;
    probe->expected_refs = resource->AddRef();
    resource->Release();
    probe->retained = resource;
    if (FAILED(probe->failure)) return probe->failure;
  }
  return resource->Map(0, range, mapping);
}
}
namespace rex::ui::d3d12 {

// Align to D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT not to waste any space if
// it's smaller (the size of the heap backing the buffer will be aligned to
// D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT anyway).
D3D12UploadBufferPool::D3D12UploadBufferPool(const D3D12Provider& provider,
                                             size_t page_size)
    : GraphicsUploadBufferPool(rex::align(
          page_size, size_t(D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT))),
      provider_(provider) {}

uint8_t* D3D12UploadBufferPool::Request(
    uint64_t submission_index, size_t size, size_t alignment,
    ID3D12Resource** buffer_out, size_t* offset_out,
    D3D12_GPU_VIRTUAL_ADDRESS* gpu_address_out) {
  size_t offset;
  const D3D12Page* page =
      static_cast<const D3D12Page*>(GraphicsUploadBufferPool::Request(
          submission_index, size, alignment, offset));
  if (!page) {
    return nullptr;
  }
  if (buffer_out) {
    *buffer_out = page->buffer_.Get();
  }
  if (offset_out) {
    *offset_out = offset;
  }
  if (gpu_address_out) {
    *gpu_address_out = page->gpu_address_ + offset;
  }
  return reinterpret_cast<uint8_t*>(page->mapping_) + offset;
}

uint8_t* D3D12UploadBufferPool::RequestPartial(
    uint64_t submission_index, size_t size, size_t alignment,
    ID3D12Resource** buffer_out, size_t* offset_out, size_t* size_out,
    D3D12_GPU_VIRTUAL_ADDRESS* gpu_address_out) {
  size_t offset, size_obtained;
  const D3D12Page* page =
      static_cast<const D3D12Page*>(GraphicsUploadBufferPool::RequestPartial(
          submission_index, size, alignment, offset, size_obtained));
  if (!page) {
    return nullptr;
  }
  if (buffer_out) {
    *buffer_out = page->buffer_.Get();
  }
  if (offset_out) {
    *offset_out = offset;
  }
  if (size_out) {
    *size_out = size_obtained;
  }
  if (gpu_address_out) {
    *gpu_address_out = page->gpu_address_ + offset;
  }
  return reinterpret_cast<uint8_t*>(page->mapping_) + offset;
}

GraphicsUploadBufferPool::Page*
D3D12UploadBufferPool::CreatePageImplementation() {
  D3D12_RESOURCE_DESC buffer_desc;
  util::FillBufferResourceDesc(buffer_desc, page_size_,
                               D3D12_RESOURCE_FLAG_NONE);
  Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
  if (FAILED(provider_.GetDevice()->CreateCommittedResource(
          &util::kHeapPropertiesUpload, provider_.GetHeapFlagCreateNotZeroed(),
          &buffer_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
          IID_PPV_ARGS(&buffer)))) {
    REXLOG_ERROR("Failed to create a D3D upload buffer with {} bytes", page_size_);
    return nullptr;
  }
  D3D12_RANGE read_range;
  read_range.Begin = 0;
  read_range.End = 0;
  void* mapping = nullptr;
  if (FAILED(MapUpload(buffer.Get(), &read_range, &mapping))) {
    REXLOG_ERROR("Failed to map a D3D upload buffer with {} bytes", page_size_);
    // ComPtr owns the only local reference and releases it on return.
    // An explicit Release here leaves ComPtr holding an already-freed object.
    return nullptr;
  }
  // Unmapping will be done implicitly when the resource is destroyed.
  return new D3D12Page(buffer.Get(), mapping);
}

D3D12UploadBufferPool::D3D12Page::D3D12Page(ID3D12Resource* buffer,
                                            void* mapping)
    : buffer_(buffer), mapping_(mapping) {
  gpu_address_ = buffer_->GetGPUVirtualAddress();
}

}  // namespace rex::ui::d3d12


bool em2::TestUploadBufferPool(const rex::ui::d3d12::D3D12Provider& provider) {
  using rex::ui::d3d12::D3D12UploadBufferPool;
  UploadMapProbe probe;
  if (upload_map_probe) return false;
  struct Restore { ~Restore() { upload_map_probe = nullptr; } } restore;
  upload_map_probe = &probe;
  auto check = [](bool ok, const char* why) {
    if (!ok) REXLOG_ERROR("Upload pool regression: {}", why);
    return ok;
  };
  // Fail after real resource creation, without exhausting system or GPU memory.
  // Retain one external reference so an extra Release is observable.
  for (auto failure : {E_OUTOFMEMORY, DXGI_ERROR_DEVICE_REMOVED}) {
    probe = {}; probe.failure = failure;
    D3D12UploadBufferPool pool(provider, 65536);
    ID3D12Resource* output = nullptr;
    size_t offset = 123, obtained = 456;
    if (!check(!pool.RequestPartial(1, 256, 256, &output, &offset, &obtained, nullptr) &&
        !output && offset == 123 && obtained == 456 && probe.calls == 1 && probe.retained,
        "failed map returns no page and leaves caller outputs unchanged")) return false;
    const auto refs = probe.retained->AddRef();
    probe.retained->Release();
    if (!check(refs == probe.expected_refs && probe.retained->GetDesc().Width == 65536,
        "failed map releases exactly one local reference")) return false;
    // Failure must leave the original pool usable, not only a new pool.
    probe.failure = S_OK;
    auto* data = pool.RequestPartial(2, 256, 256, &output, &offset, &obtained, nullptr);
    if (!check(data && output && offset == 0 && obtained == 256,
        "pool accepts a normal allocation after map rejection")) return false;
    std::memset(data, 0x5a, obtained);
  }
  upload_map_probe = nullptr;
  // Exercise real mapping, alignment, full-page rollover, reclaim and cache
  // disposal without submitting any commands against these CPU-only pages.
  D3D12UploadBufferPool pool(provider, 65536);
  ID3D12Resource* first = nullptr; size_t first_offset = 0, first_size = 0;
  D3D12_GPU_VIRTUAL_ADDRESS first_gpu = 0;
  auto* a = pool.RequestPartial(1, 65536, 256, &first, &first_offset, &first_size, &first_gpu);
  if (!check(a && first && first_offset == 0 && first_size == 65536 &&
      first_gpu == first->GetGPUVirtualAddress(), "real full-page allocation")) return false;
  std::memset(a, 0xa7, first_size);
  ID3D12Resource* second = nullptr; size_t second_offset = 0;
  auto* b = pool.Request(2, 256, 256, &second, &second_offset, nullptr);
  if (!check(b && second != first && second_offset == 0 && a[0] == 0xa7 && a[65535] == 0xa7,
      "rollover preserves the previous page")) return false;
  pool.Reclaim(1);
  size_t remainder = 0;
  if (!check(pool.RequestPartial(2, 65536, 256, nullptr, nullptr, &remainder, nullptr) && remainder == 65280,
      "partial request fills the remaining second page")) return false;
  ID3D12Resource* reused = nullptr; size_t reused_offset = 1;
  if (!check(pool.Request(3, 256, 256, &reused, &reused_offset, nullptr) && reused == first && reused_offset == 0,
      "only completed pages are reused")) return false;
  pool.ClearCache();
  REXLOG_INFO("Upload pool regression passed: real D3D12 mapping, allocation/device map failures, exact reference ownership, same-pool recovery, alignment, rollover and completed-page reuse");
  return true;
}
