#include "resolve_coverage.h"
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/logging.h>
#include <algorithm>
#include <bit>
#include <chrono>
namespace {
constexpr uint32_t memory_bytes=0x20000000;
void Append(std::vector<em2::ResolvedStorageRange>& ranges,uint32_t address,uint32_t length,uint32_t bpp) {
    if(!ranges.empty() && ranges.back().bytes_per_texel==bpp && uint64_t(ranges.back().address)+ranges.back().length==address)ranges.back().length+=length;
    else ranges.push_back({address,length,bpp});
}
}
bool em2::GetResolveCoverage(const ResolveWriteObservation& event,std::vector<ResolvedStorageRange>& output) {
    if(!event.info || !event.length || event.address>=memory_bytes || event.length>memory_bytes-event.address ||
       !event.scale_x || event.scale_x>7 || !event.scale_y || event.scale_y>7)return false;
    const auto& info=*event.info;
    if(info.copy_dest_extent_start!=event.address || info.copy_dest_extent_length!=event.length ||
       !info.coordinate_info.width_div_8 || !info.height_div_8)return false;
    const auto* format=rex::graphics::FormatInfo::Get(uint32_t(info.copy_dest_info.copy_dest_format));
    if(!format || format->block_width!=1 || format->block_height!=1 || format->bits_per_pixel%8)return false;
    if(format->type!=rex::graphics::FormatType::kResolvable && !info.IsCopyingDepth())return false;
    const auto bpp=format->bits_per_pixel/8;
    if(bpp!=1 && bpp!=2 && bpp!=4 && bpp!=8 && bpp!=16)return false;
    const uint32_t group_bytes=bpp==1?8:16,lanes=group_bytes/bpp;
    if(info.copy_dest_base&(group_bytes-1))return false;
    const uint32_t left=info.copy_dest_coordinate_info.offset_x_div_8*8,top=info.copy_dest_coordinate_info.offset_y_div_8*8;
    const uint32_t width=info.coordinate_info.width_div_8*8,height=info.height_div_8*8;
    const uint32_t pitch=info.copy_dest_coordinate_info.pitch_aligned_div_32*32;
    const uint32_t texture_height=info.copy_dest_coordinate_info.height_aligned_div_32*32;
    if(!pitch || left+uint64_t(width)>16384 || top+uint64_t(height)>16384 ||
       (info.copy_dest_info.copy_dest_array && !texture_height) || uint64_t(width)*height*bpp>event.length)return false;
    std::vector<uint32_t> groups;groups.reserve(size_t(width/lanes)*height);
    for(uint32_t y=top;y<top+height;++y)for(uint32_t x=left;x<left+width;x+=lanes) {
        const int32_t offset=info.copy_dest_info.copy_dest_array?
            rex::graphics::texture_util::GetTiledOffset3D(int32_t(x),int32_t(y),info.copy_dest_info.copy_dest_slice,pitch,texture_height,std::countr_zero(bpp)):
            rex::graphics::texture_util::GetTiledOffset2D(int32_t(x),int32_t(y),pitch,std::countr_zero(bpp));
        if(offset<0)return false;
        const uint64_t address=uint64_t(info.copy_dest_base)+uint32_t(offset);
        if(address<event.address || address+group_bytes>uint64_t(event.address)+event.length || (address&(group_bytes-1)))return false;
        groups.push_back(uint32_t(address));
    }
    std::sort(groups.begin(),groups.end());groups.erase(std::unique(groups.begin(),groups.end()),groups.end());
    std::vector<ResolvedStorageRange> result;
    for(auto address:groups)Append(result,address,group_bytes,bpp);
    output=std::move(result);return true;
}
void em2::ResolveRangeLedger::Unknown(uint32_t address,uint32_t length) {
    if(!length || address>=memory_bytes)return;
    const uint64_t end=std::min(uint64_t(memory_bytes),uint64_t(address)+length);
    for(uint32_t cell=address/8;uint64_t(cell)*8<end;) {
        auto& page=pages_[cell/512];if(!page)page=std::make_unique<Page>();
        const auto count=uint32_t(std::min(uint64_t(512-cell%512),(end+7)/8-cell));
        std::fill_n(page->cells.begin()+cell%512,count,uint8_t(6));cell+=count;
    }
}
void em2::ResolveRangeLedger::WriteRange(const ResolvedStorageRange& range) {
            const uint8_t code=uint8_t(std::countr_zero(range.bytes_per_texel)+1);
            for(uint32_t cell=range.address/8;cell<(range.address+range.length)/8;) {
                auto& page=pages_[cell/512];if(!page)page=std::make_unique<Page>();
                const auto start=cell%512,count=std::min(512-start,(range.address+range.length)/8-cell);
                if(code==1) {
                    // 8-byte and 16-byte scaled interleavings cannot share a
                    // half-written 16-byte group. Its remaining half is unknown
                    // until a later full write gives it an authoritative type.
                    auto invalidate=[&](uint32_t index){auto& value=page->cells[index];if(value>=2 && value<=5)value=6;};
                    if(start&1)invalidate(start-1);
                    if((start+count)&1)invalidate(start+count);
                }
                std::fill_n(page->cells.begin()+start,count,code);cell+=count;
            }
}
bool em2::ResolveRangeLedger::SeedRanges(const std::vector<ResolvedStorageRange>& ranges,uint32_t sx,uint32_t sy) noexcept {
    try {
        if(!sx || sx>7 || !sy || sy>7)return false;
        ResolveRangeLedger staged;staged.scale_x_=sx;staged.scale_y_=sy;
        uint64_t previous_end=0;
        for(const auto& range:ranges) {
            const auto bpp=range.bytes_per_texel,group=bpp==1?8u:16u;
            if((bpp!=1 && bpp!=2 && bpp!=4 && bpp!=8 && bpp!=16) || !range.length ||
               (range.address&(group-1)) || (range.length&(group-1)) || range.address<previous_end ||
               uint64_t(range.address)+range.length>memory_bytes)return false;
            staged.WriteRange(range);previous_end=uint64_t(range.address)+range.length;
        }
        std::vector<ResolvedStorageRange> checked;if(!staged.Snapshot(checked))return false;
        *this=std::move(staged);return true;
    }catch(...){return false;}
}
bool em2::ResolveRangeLedger::Prepare(const ResolveWriteObservation& event) noexcept {
    try {
        if(!event.info || !event.length || !event.scale_x || event.scale_x>7 || !event.scale_y || event.scale_y>7 ||
           event.address>=memory_bytes || event.length>memory_bytes-event.address ||
           event.info->copy_dest_extent_start!=event.address || event.info->copy_dest_extent_length!=event.length)return false;
        const auto& info=*event.info;
        const std::array<uint32_t,5> key={uint32_t(info.copy_dest_info.copy_dest_format),info.coordinate_info.width_div_8,info.height_div_8,
            info.copy_dest_coordinate_info.packed,uint32_t(info.copy_dest_info.copy_dest_array)|(uint32_t(info.copy_dest_info.copy_dest_slice)<<1)|(uint32_t(info.IsCopyingDepth())<<8)};
        auto shape=shapes_.find(key);
        if(shape==shapes_.end()) {
            std::vector<ResolvedStorageRange> coverage;if(!GetResolveCoverage(event,coverage))return false;

            for(auto& range:coverage)range.address-=info.copy_dest_base;
            if(shapes_.size()>=64)shapes_.erase(shapes_.begin());
            shape=shapes_.emplace(key,std::move(coverage)).first;
        }
        return true;
    }catch(...){return false;}
}
bool em2::ResolveRangeLedger::Record(const ResolveWriteObservation& event) noexcept {
    try {
        if(poisoned_)return false;
        if(!event.scale_x || event.scale_x>7 || !event.scale_y || event.scale_y>7 ||
           (scale_x_ && (event.scale_x!=scale_x_ || event.scale_y!=scale_y_))) {poisoned_=true;return false;}
        scale_x_=event.scale_x;scale_y_=event.scale_y;
        if(!event.info || !event.length || event.address>=memory_bytes || event.length>memory_bytes-event.address ||
           event.info->copy_dest_extent_start!=event.address || event.info->copy_dest_extent_length!=event.length) {
            Unknown(event.address,event.length);return false;
        }
        if(!Prepare(event)){Unknown(event.address,event.length);return false;}
        const auto& info=*event.info;
        const std::array<uint32_t,5> key={uint32_t(info.copy_dest_info.copy_dest_format),info.coordinate_info.width_div_8,info.height_div_8,
            info.copy_dest_coordinate_info.packed,uint32_t(info.copy_dest_info.copy_dest_array)|(uint32_t(info.copy_dest_info.copy_dest_slice)<<1)|(uint32_t(info.IsCopyingDepth())<<8)};
        auto shape=shapes_.find(key);
        for(const auto& relative:shape->second) {
            const uint64_t address=uint64_t(info.copy_dest_base)+relative.address;
            if(address<event.address || address+relative.length>uint64_t(event.address)+event.length ||
               (info.copy_dest_base&((relative.bytes_per_texel==1?8:16)-1))) {Unknown(event.address,event.length);return false;}
        }
        for(const auto& relative:shape->second) {
            const ResolvedStorageRange range{info.copy_dest_base+relative.address,relative.length,relative.bytes_per_texel};
            WriteRange(range);
        }
        return true;
    }catch(...){poisoned_=true;return false;}
}
void em2::ResolveRangeLedger::InvalidatePages(uint32_t first,uint32_t last) noexcept {
    if(first>=memory_bytes || first>last)return;
    last=std::min(last,memory_bytes-1);auto it=pages_.lower_bound(first/4096);
    while(it!=pages_.end() && it->first<=last/4096)it=pages_.erase(it);
}
bool em2::ResolveRangeLedger::Snapshot(std::vector<ResolvedStorageRange>& output) const {
    if(poisoned_)return false;
    std::vector<ResolvedStorageRange> result;
    for(const auto& [index,page]:pages_)for(unsigned cell=0;cell<512;++cell) {
        const auto code=page->cells[cell];if(!code)continue;if(code>5)return false;
        if(code>=2 && page->cells[cell^1]!=code)return false;
        Append(result,index*4096+cell*8,8,1u<<(code-1));
    }
    output=std::move(result);return true;
}
em2::ResolveLedgerWatch::ResolveLedgerWatch(rex::graphics::SharedMemory& memory,ResolveRangeLedger& ledger):memory_(memory),ledger_(ledger) {
    handle_=memory_.RegisterGlobalWatch([](const std::unique_lock<std::recursive_mutex>&,void* context,uint32_t first,uint32_t last,bool gpu) {
        if(!gpu)static_cast<ResolveLedgerWatch*>(context)->ledger_.InvalidatePages(first,last);
    },this);
}
em2::ResolveLedgerWatch::~ResolveLedgerWatch(){if(handle_)memory_.UnregisterGlobalWatch(handle_);}

bool em2::TestResolveCoverage() {
    namespace g=rex::graphics;namespace x=g::xenos;
    const std::array<x::TextureFormat,5> formats={x::TextureFormat::k_8,x::TextureFormat::k_16,x::TextureFormat::k_8_8_8_8,
        x::TextureFormat::k_16_16_16_16_FLOAT,x::TextureFormat::k_32_32_32_32_FLOAT};
    struct Rectangle {uint32_t x,y,w,h;};
    const std::array<Rectangle,5> rectangles={{{0,0,32,32},{8,8,8,8},{24,24,16,16},{40,56,32,24},{0,0,8,8}}};
    auto make=[](x::TextureFormat format,Rectangle rectangle,bool array,unsigned slice,uint32_t base=0x10000) {
        g::draw_util::ResolveInfo info{};const auto bpp=g::FormatInfo::Get(format)->bits_per_pixel/8;const auto log=std::countr_zero(bpp);
        const auto mask_x=(1u<<x::GetTextureTiledXBaseGranularityLog2(array,log))-1;
        const auto mask_y=(1u<<x::GetTextureTiledYBaseGranularityLog2(array,log))-1;
        const auto bx=rectangle.x&~mask_x,by=rectangle.y&~mask_y;
        info.copy_dest_info.copy_dest_format=x::ColorFormat(format);info.copy_dest_info.copy_dest_array=array;info.copy_dest_info.copy_dest_slice=slice;
        info.copy_dest_coordinate_info.pitch_aligned_div_32=4;info.copy_dest_coordinate_info.height_aligned_div_32=4;
        info.copy_dest_coordinate_info.offset_x_div_8=(rectangle.x&mask_x)/8;info.copy_dest_coordinate_info.offset_y_div_8=(rectangle.y&mask_y)/8;
        info.coordinate_info.width_div_8=rectangle.w/8;info.height_div_8=rectangle.h/8;
        if(array) {
            info.copy_dest_base=base+g::texture_util::GetTiledOffset3D(bx,by,0,128,128,log);
            info.copy_dest_extent_start=base+g::texture_util::GetTiledAddressLowerBound3D(rectangle.x,rectangle.y,slice,128,128,log);
            const auto end=base+g::texture_util::GetTiledAddressUpperBound3D(rectangle.x+rectangle.w,rectangle.y+rectangle.h,slice+1,128,128,log);
            info.copy_dest_extent_length=end-info.copy_dest_extent_start;
        } else {
            info.copy_dest_base=base+g::texture_util::GetTiledOffset2D(bx,by,128,log);
            info.copy_dest_extent_start=base+g::texture_util::GetTiledAddressLowerBound2D(rectangle.x,rectangle.y,128,log);
            const auto end=base+g::texture_util::GetTiledAddressUpperBound2D(rectangle.x+rectangle.w,rectangle.y+rectangle.h,128,log);
            info.copy_dest_extent_length=end-info.copy_dest_extent_start;
        }
        return info;
    };
    unsigned tested=0;
    for(const auto format:formats)for(const auto rectangle:rectangles)for(unsigned layer=0;layer<6;++layer) {
        const bool array=layer!=0;const unsigned slice=array?std::array<unsigned,5>{0,1,3,4,7}[layer-1]:0;
        auto info=make(format,rectangle,array,slice);const ResolveWriteObservation event{nullptr,info.copy_dest_extent_start,info.copy_dest_extent_length,2,3,&info};
        std::vector<ResolvedStorageRange> coverage;if(!GetResolveCoverage(event,coverage))return false;
        ResolveRangeLedger prepared;std::vector<ResolvedStorageRange> prepared_ranges;
        if(!prepared.Prepare(event) || prepared.page_count() || !prepared.Snapshot(prepared_ranges) || !prepared_ranges.empty() ||
           !prepared.Record(event) || !prepared.Snapshot(prepared_ranges) || prepared_ranges.size()!=coverage.size() ||
           !std::equal(prepared_ranges.begin(),prepared_ranges.end(),coverage.begin(),[](const auto& a,const auto& b){
               return a.address==b.address && a.length==b.length && a.bytes_per_texel==b.bytes_per_texel;}))return false;
        std::vector<uint8_t> pixels(event.length),groups(event.length);const auto bpp=g::FormatInfo::Get(format)->bits_per_pixel/8;
        // Independent per-texel byte oracle; the implementation enumerates
        // whole storage groups and uses adjusted relative coordinates.
        for(unsigned y=rectangle.y;y<rectangle.y+rectangle.h;++y)for(unsigned px=rectangle.x;px<rectangle.x+rectangle.w;++px) {
            const auto offset=array?g::texture_util::GetTiledOffset3D(px,y,slice,128,128,std::countr_zero(bpp)):
                g::texture_util::GetTiledOffset2D(px,y,128,std::countr_zero(bpp));
            const uint64_t address=0x10000ull+offset;if(address<event.address || address+bpp>uint64_t(event.address)+event.length)return false;
            std::fill_n(pixels.begin()+size_t(address-event.address),bpp,uint8_t(1));
        }
        for(const auto& range:coverage) {
            if(range.bytes_per_texel!=bpp)return false;
            std::fill_n(groups.begin()+range.address-event.address,range.length,uint8_t(1));
        }
        if(pixels!=groups){REXLOG_ERROR("Resolve group coverage mismatch: format={} layer={} rect={},{}",unsigned(format),layer,rectangle.x,rectangle.y);return false;}
        ResolveRangeLedger seeded;std::vector<ResolvedStorageRange> restored;
        if(!seeded.SeedRanges(coverage,3,2) || !seeded.Snapshot(restored) || restored.size()!=coverage.size() ||
           !std::equal(restored.begin(),restored.end(),coverage.begin(),[](const auto& a,const auto& b){
               return a.address==b.address && a.length==b.length && a.bytes_per_texel==b.bytes_per_texel;}))return false;
        const auto pages=seeded.page_count();
        for(const auto bad:std::vector<std::vector<ResolvedStorageRange>>{
            {{0,16,3}},{{0,0,4}},{{1,16,4}},{{0x1FFFFFF0,32,4}},{{16,16,4},{0,16,4}},{{0,16,4},{8,8,1}}}) {
            if(seeded.SeedRanges(bad,2,2) || seeded.page_count()!=pages || seeded.scale_x()!=3 || seeded.scale_y()!=2)return false;
        }
        if(seeded.SeedRanges(coverage,0,2) || !seeded.Snapshot(restored) || restored.size()!=coverage.size())return false;
        ++tested;
    }
    auto full=make(formats[2],rectangles[0],false,0);
    ResolveWriteObservation event{nullptr,full.copy_dest_extent_start,full.copy_dest_extent_length,2,2,&full};
    ResolveRangeLedger ledger;if(!ledger.Record(event))return false;
    std::vector<ResolvedStorageRange> original;if(!ledger.Snapshot(original) || original.empty())return false;
    auto partial=make(formats[1],rectangles[1],false,0);
    event={nullptr,partial.copy_dest_extent_start,partial.copy_dest_extent_length,2,2,&partial};if(!ledger.Record(event))return false;
    std::vector<ResolvedStorageRange> mixed;if(!ledger.Snapshot(mixed))return false;bool narrow=false,wide=false;
    for(const auto& range:mixed){narrow|=range.bytes_per_texel==2;wide|=range.bytes_per_texel==4;}
    if(!narrow || !wide)return false;
    const auto retained=mixed;event.info=nullptr;if(ledger.Record(event) || ledger.Snapshot(mixed))return false;
    if(mixed.size()!=retained.size() || !std::equal(mixed.begin(),mixed.end(),retained.begin(),[](const auto& a,const auto& b){
        return a.address==b.address && a.length==b.length && a.bytes_per_texel==b.bytes_per_texel;}))return false;
    ledger.InvalidatePages(0x10000,0x10000+0xFFFF);if(ledger.page_count() || !ledger.Snapshot(mixed) || !mixed.empty())return false;
    event={nullptr,full.copy_dest_extent_start,full.copy_dest_extent_length,2,2,&full};if(!ledger.Record(event) || !ledger.Snapshot(mixed))return false;
    auto moved=full;moved.copy_dest_base+=0x10000;moved.copy_dest_extent_start+=0x10000;
    const ResolveWriteObservation shifted{nullptr,moved.copy_dest_extent_start,moved.copy_dest_extent_length,2,2,&moved};
    if(!ledger.Record(shifted) || !ledger.Snapshot(mixed))return false;size_t bytes=0;for(const auto& range:mixed)bytes+=range.length;
    if(bytes!=2*32*32*4)return false;
    ledger.InvalidatePages(0x10008,0x10008);if(!ledger.Snapshot(mixed))return false;bytes=0;
    for(const auto& range:mixed){if(range.address<0x20000)return false;bytes+=range.length;}if(bytes!=32*32*4)return false;
    ++event.scale_x;if(ledger.Record(event) || ledger.Snapshot(mixed))return false;
    std::vector<ResolvedStorageRange> unchanged={{1,2,3}};event.info=nullptr;
    if(GetResolveCoverage(event,unchanged) || unchanged.size()!=1 || unchanged[0].address!=1)return false;
    // Recurring full-resolution geometry must not be re-enumerated and sorted
    // for every resolve. Measure first-record and cached repeated recording,
    // including sparse page updates; this is not a gameplay FPS benchmark.
    auto large=make(formats[2],{0,0,1280,720},false,0,0x1000000);
    large.copy_dest_coordinate_info.pitch_aligned_div_32=40;large.copy_dest_coordinate_info.height_aligned_div_32=23;
    large.copy_dest_extent_start=large.copy_dest_base;
    large.copy_dest_extent_length=g::texture_util::GetTiledAddressUpperBound2D(1280,720,1280,2);
    const ResolveWriteObservation large_event{nullptr,large.copy_dest_extent_start,large.copy_dest_extent_length,2,2,&large};
    ResolveRangeLedger recurring;const auto begin=std::chrono::steady_clock::now();if(!recurring.Record(large_event))return false;
    const auto first=std::chrono::steady_clock::now();for(unsigned i=0;i<128;++i)if(!recurring.Record(large_event))return false;
    const auto end=std::chrono::steady_clock::now();std::vector<ResolvedStorageRange> repeated;if(!recurring.Snapshot(repeated))return false;
    size_t repeated_bytes=0;for(const auto& range:repeated)repeated_bytes+=range.length;if(repeated_bytes!=1280*720*4)return false;
    REXLOG_INFO("Resolve ledger 1280x720 CPU metadata timing: {:.3f} ms first record, {:.3f} ms cached mean over128 writes; exact geometry cache and page-bulk updates, not gameplay FPS",
        std::chrono::duration<double,std::milli>(first-begin).count(),std::chrono::duration<double,std::milli>(end-first).count()/128);
    REXLOG_INFO("Resolve exact-group coverage passed: {} per-texel byte comparisons, 8/16/32/64/128-bit formats, partial/cross-tile/adjusted-origin 2D and array slices 0/1/3/4/7; sparse mixed-format ledger, unknown-write rejection, page invalidation and scale-epoch rejection; live tracking not installed",tested);
    return true;
}
