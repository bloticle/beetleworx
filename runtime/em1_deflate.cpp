#include "em1_deflate.h"
#define Z_PREFIX
#define Z_SOLO
#include "em1_compat/zlib/zlib.h"
#include <cstdlib>
#include <limits>
#include <stdexcept>
namespace em2::em1 {
std::vector<uint8_t> DeflateNativePakResource(std::span<const uint8_t> bytes){
 if(bytes.empty() || bytes.size()>64ull*1024*1024)throw std::runtime_error("Invalid native PAK compression input");
 z_stream stream{};
 stream.zalloc=[](voidpf,uInt count,uInt size)->voidpf{
   if(size && count>std::numeric_limits<size_t>::max()/size)return nullptr;
   return std::calloc(count,size);
 };
 stream.zfree=[](voidpf,voidpf block){std::free(block);};
 if(deflateInit(&stream,Z_BEST_SPEED)!=Z_OK)throw std::runtime_error("Native PAK deflater initialization failed");
 struct End{z_stream& stream;~End(){deflateEnd(&stream);}} end{stream};
 std::vector<uint8_t> packed(deflateBound(&stream,uLong(bytes.size())));
 stream.next_in=const_cast<Bytef*>(bytes.data());stream.avail_in=uInt(bytes.size());
 stream.next_out=packed.data();stream.avail_out=uInt(packed.size());
 if(deflate(&stream,Z_FINISH)!=Z_STREAM_END || stream.avail_in)throw std::runtime_error("Native PAK compression failed");
 packed.resize(stream.total_out);return packed;
}
}
