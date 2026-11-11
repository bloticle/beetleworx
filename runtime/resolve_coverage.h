#pragma once
#include "resolve_write_observer.h"
#include <rex/graphics/shared_memory.h>
#include <array>
#include <map>
#include <memory>
#include <vector>
namespace em2 {
struct ResolvedStorageRange {
    uint32_t address,length,bytes_per_texel;
};
// Exact 8-byte (8bpp) or 16-byte (other widths) native tiled groups, coalesced
// only when adjacent. Bounding extents include holes and are not coverage.
// On failure output is unchanged. Enumeration allocates and is not installed
// on the gameplay copy hot path yet.
bool GetResolveCoverage(const ResolveWriteObservation&,std::vector<ResolvedStorageRange>&);
// Sparse current-valid metadata, independent of GPU storage. Serialize updates
// with the native global memory lock when integrating write/invalidation hooks.
// This does not capture/migrate GPU data or register native invalidation hooks.
class ResolveRangeLedger {
public:
    // GPU worker only. Cache geometry before the native write, outside its
    // global memory lock. This does not mark any page as written/valid.
    bool Prepare(const ResolveWriteObservation&) noexcept;
    bool Record(const ResolveWriteObservation&) noexcept;
    // Replace metadata from validated, ordered snapshot ranges at a new scale.
    // Failure preserves the previous ledger, including its invalidation state.
    bool SeedRanges(const std::vector<ResolvedStorageRange>&,uint32_t sx,uint32_t sy) noexcept;
    void InvalidatePages(uint32_t first_byte,uint32_t last_byte) noexcept;
    bool Snapshot(std::vector<ResolvedStorageRange>&) const;
    size_t page_count() const{return pages_.size();}
    uint32_t scale_x() const{return scale_x_;}
    uint32_t scale_y() const{return scale_y_;}
private:
    struct Page {std::array<uint8_t,512> cells{};}; // One cell per native 8 bytes.
    std::map<uint32_t,std::unique_ptr<Page>> pages_;
    // Cache relative coverage for recurring resolve geometry; destination base
    // is validated separately. Bound memory without retaining per-copy events.
    std::map<std::array<uint32_t,5>,std::vector<ResolvedStorageRange>> shapes_;
    uint32_t scale_x_=0,scale_y_=0;
    bool poisoned_=false;
    void Unknown(uint32_t address,uint32_t length);
    void WriteRange(const ResolvedStorageRange&);
};
bool TestResolveCoverage();
// Binds current-valid metadata to native CPU invalidations. GPU resolve
// notifications are ignored because the write observer supplies exact coverage.
// Shared memory and ledger must outlive this registration. No automatic game
// installation; a complete renderer controller must own the lifetime.
class ResolveLedgerWatch {
public:
    ResolveLedgerWatch(rex::graphics::SharedMemory&,ResolveRangeLedger&);
    ~ResolveLedgerWatch();
    ResolveLedgerWatch(const ResolveLedgerWatch&)=delete;
    ResolveLedgerWatch& operator=(const ResolveLedgerWatch&)=delete;
    bool valid() const{return handle_!=nullptr;}
private:
    rex::graphics::SharedMemory& memory_;
    ResolveRangeLedger& ledger_;
    rex::graphics::SharedMemory::GlobalWatchHandle handle_=nullptr;
};
}
