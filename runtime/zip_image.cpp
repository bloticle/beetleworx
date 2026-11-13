// Stored/DEFLATE ZIP and ZIP64 reader. Format reference: PKWARE APPNOTE 6.3.10.
#include "zip_image.h"
#include "archive_io.h"
#define Z_SOLO
#define Z_PREFIX
#include "em1_compat/zlib/zlib.h"
#include <algorithm>
#include <cstdlib>
#include <cstddef>
#include <map>
#include <set>
namespace em2 {
namespace {
using archive_detail::Require;using archive_detail::File;using archive_detail::Hash;
constexpr uint64_t limit=16ull*1024*1024*1024,stride=8*1024*1024;
struct Canceled{};
uint16_t U16(const uint8_t* p){return uint16_t(p[0])|uint16_t(p[1])<<8;}
uint32_t U32(const uint8_t* p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
uint64_t U64(const uint8_t* p){return U32(p)|(uint64_t(U32(p+4))<<32);}
constexpr auto crc_table=[](){std::array<uint32_t,256> table{};for(uint32_t i=0;i<256;++i){auto n=i;for(unsigned j=0;j<8;++j)n=(n>>1)^((n&1)?0xedb88320u:0);table[i]=n;}return table;}();
uint32_t Crc(uint32_t crc,std::span<const uint8_t> bytes){for(auto b:bytes)crc=crc_table[(crc^b)&255]^(crc>>8);return crc;}
std::string Lower(std::string s){for(auto& c:s)if(c>='A' && c<='Z')c+=32;return s;}
struct Allocator {
 struct alignas(std::max_align_t) Allocation {size_t size;};size_t used=0;
 static voidpf Allocate(voidpf opaque,uInt count,uInt width){auto& a=*static_cast<Allocator*>(opaque);constexpr size_t budget=128*1024*1024;
  if(!width || !count || size_t(count)>budget/width)return nullptr;const auto bytes=size_t(count)*width;if(bytes>budget-a.used)return nullptr;
  auto* p=static_cast<Allocation*>(std::calloc(1,sizeof(Allocation)+bytes));if(!p)return nullptr;p->size=bytes;a.used+=bytes;return p+1;}
 static void Free(voidpf opaque,voidpf pointer){if(!pointer)return;auto& a=*static_cast<Allocator*>(opaque);auto* p=static_cast<Allocation*>(pointer)-1;a.used-=p->size;std::free(p);}
};
struct Stream {z_stream stream{};bool valid=false;~Stream(){if(valid)inflateEnd(&stream);}};
struct Checkpoint : Stream {uint64_t position=0,consumed=0;uint32_t crc=0;};
struct Entry {std::string path;uint64_t data=0,packed=0,size=0,base=0;uint32_t crc=0;uint16_t method=0;bool directory=false;std::vector<std::unique_ptr<Checkpoint>> points;};
std::string Path(std::string name,bool& directory){
 for(auto& c:name)if(c=='\\')c='/';Require(!name.empty() && name.front()!='/' && name.find(':')==std::string::npos && name.find('\0')==std::string::npos,"Unsafe ZIP path");
 directory=name.back()=='/';if(directory)name.pop_back();Require(!name.empty() && name.size()<=2048,"Invalid ZIP path length");
 size_t at=0,depth=0;while(at<=name.size()){auto end=name.find('/',at);if(end==std::string::npos)end=name.size();const auto part=name.substr(at,end-at);Require(!part.empty() && part!="." && part!=".." && part.size()<=255 && ++depth<=64,"Unsafe ZIP path component");if(end==name.size())break;at=end+1;}return name;
}
}
struct ZipImage::Impl {
 File file;Allocator allocator;std::vector<Entry> entries;std::vector<ArchiveFile> files;std::vector<std::pair<uint64_t,size_t>> starts;
 std::array<uint8_t,32> identity{};uint64_t size=0,unpacked=0;Stream decoder;z_stream& active=decoder.stream;bool& initialized=decoder.valid;bool ended=false,indexing=false;
 size_t active_entry=SIZE_MAX;uint64_t position=0,consumed=0,fed=0;uint32_t crc=0;std::array<uint8_t,65536> input{};
 ~Impl()=default;
 void Reset(size_t index,const Checkpoint* point=nullptr){if(initialized)inflateEnd(&active);active={};initialized=false;active_entry=SIZE_MAX;ended=false;
  active.opaque=&allocator;active.zalloc=Allocator::Allocate;active.zfree=Allocator::Free;
  const auto result=point?inflateCopy(&active,const_cast<z_stream*>(&point->stream)):inflateInit2(&active,-15);Require(result==Z_OK,"ZIP decoder/checkpoint memory budget exceeded");initialized=true;
  active.next_in=nullptr;active.avail_in=0;active.next_out=nullptr;active.avail_out=0;position=point?point->position:0;consumed=fed=point?point->consumed:0;crc=point?point->crc:0xffffffffu;active_entry=index;
 }
 void Pump(std::span<uint8_t> out){auto& e=entries.at(active_entry);size_t done=0;
  while(done<out.size() && !ended){
   if(!active.avail_in && fed<e.packed){const auto n=size_t(std::min<uint64_t>(input.size(),e.packed-fed));file.Read(e.data+fed,std::span(input).first(n));fed+=n;active.next_in=input.data();active.avail_in=uInt(n);}
   const auto until=stride-position%stride;const auto n=size_t(std::min<uint64_t>(out.size()-done,until));active.next_out=out.data()+done;active.avail_out=uInt(n);const auto before=active.avail_in;
   const auto result=inflate(&active,Z_NO_FLUSH);const auto made=n-active.avail_out;consumed+=before-active.avail_in;
   Require(position<=e.size && made<=e.size-position,"ZIP DEFLATE stream exceeds its declared size");crc=Crc(crc,out.subspan(done,made));done+=made;position+=made;
   Require(result==Z_OK || result==Z_STREAM_END || result==Z_BUF_ERROR,"Invalid ZIP DEFLATE stream");
   if(result==Z_STREAM_END){ended=true;Require(position==e.size && consumed==e.packed && ~crc==e.crc,"ZIP DEFLATE length or CRC mismatch");}
   else Require(made || before!=active.avail_in,"Truncated or stalled ZIP DEFLATE stream");
   if(indexing && !ended && position && position<e.size && position%stride==0 && (e.points.empty() || e.points.back()->position!=position)){auto point=std::make_unique<Checkpoint>();Require(inflateCopy(&point->stream,&active)==Z_OK,"ZIP seek checkpoint budget exceeded");point->valid=true;point->stream.next_in=nullptr;point->stream.avail_in=0;point->stream.next_out=nullptr;point->stream.avail_out=0;point->position=position;point->consumed=consumed;point->crc=crc;e.points.push_back(std::move(point));}
  }
  Require(done==out.size(),"Short ZIP DEFLATE output");
 }
 void Finish(){if(ended)return;auto& e=entries.at(active_entry);Require(position==e.size,"ZIP entry ended before its declared size");std::array<uint8_t,1> extra{};
  while(!ended){if(!active.avail_in && fed<e.packed){const auto n=size_t(std::min<uint64_t>(input.size(),e.packed-fed));file.Read(e.data+fed,std::span(input).first(n));fed+=n;active.next_in=input.data();active.avail_in=uInt(n);}
   const auto before=active.avail_in;active.next_out=extra.data();active.avail_out=1;const auto result=inflate(&active,Z_NO_FLUSH);consumed+=before-active.avail_in;
   Require(active.avail_out==1,"ZIP DEFLATE output exceeds its declared length");Require(result==Z_OK || result==Z_STREAM_END || result==Z_BUF_ERROR,"Invalid ZIP DEFLATE ending");
   if(result==Z_STREAM_END){ended=true;Require(consumed==e.packed && ~crc==e.crc,"ZIP entry CRC or compressed length mismatch");}else Require(before!=active.avail_in,"Truncated ZIP DEFLATE ending");
  }
 }
 void ReadEntry(size_t index,uint64_t offset,std::span<uint8_t> out){auto& e=entries.at(index);Require(offset<=e.size && out.size()<=e.size-offset,"Read outside ZIP entry");
  if(e.method==0){file.Read(e.data+offset,out);return;}
  const Checkpoint* point=nullptr;for(const auto& p:e.points)if(p->position<=offset)point=p.get();else break;
  if(active_entry!=index || position>offset || (point && position<point->position))Reset(index,point);
  std::array<uint8_t,65536> discard{};while(position<offset)Pump(std::span(discard).first(size_t(std::min<uint64_t>(discard.size(),offset-position))));
  Pump(out);if(position==e.size)Finish();
 }
 explicit Impl(const std::filesystem::path& path,const std::function<bool(uint64_t,uint64_t)>& progress):file(path){
  Require(file.size>=22 && file.size<=limit,"Invalid ZIP container size");const auto tail_size=size_t(std::min<uint64_t>(file.size,65557));std::vector<uint8_t> tail(tail_size);file.Read(file.size-tail_size,tail);size_t end=SIZE_MAX;
  for(size_t i=tail.size()-22;;--i){if(U32(tail.data()+i)==0x06054b50 && U16(tail.data()+i+10)>0 && i+22+U16(tail.data()+i+20)==tail.size()){end=i;break;}if(i==0)break;}Require(end!=SIZE_MAX,"ZIP end directory is missing or truncated");
  const auto* footer=tail.data()+end;Require(U16(footer+4)==0 && U16(footer+6)==0 && U16(footer+8)==U16(footer+10),"Split ZIP archives are not supported yet");
  uint64_t count=U16(footer+10),cd_size=U32(footer+12),cd_at=U32(footer+16),end_at=file.size-tail_size+end,cd_limit=end_at;
  if(count==65535 || cd_size==0xffffffff || cd_at==0xffffffff){Require(end_at>=20,"ZIP64 locator is missing");std::array<uint8_t,20> locator{};file.Read(end_at-20,locator);
   Require(U32(locator.data())==0x07064b50 && U32(locator.data()+4)==0 && U32(locator.data()+16)==1,"Invalid or split ZIP64 locator");const auto at=U64(locator.data()+8);Require(at<=end_at-20 && 56<=end_at-20-at,"Invalid ZIP64 directory extent");std::array<uint8_t,56> record{};file.Read(at,record);
   const auto length=U64(record.data()+4);Require(U32(record.data())==0x06064b50 && length>=44 && length<=4*1024*1024 && length+12<=end_at-20-at && U32(record.data()+16)==0 && U32(record.data()+20)==0 && U64(record.data()+24)==U64(record.data()+32),"Invalid ZIP64 end directory");
   count=U64(record.data()+32);cd_size=U64(record.data()+40);cd_at=U64(record.data()+48);cd_limit=at;
  }
  Require(count>0 && count<=100000 && cd_size<=64*1024*1024 && cd_at<=cd_limit && cd_size<=cd_limit-cd_at,"ZIP central directory exceeds its budget");std::vector<uint8_t> directory(size_t(cd_size),uint8_t(0));file.Read(cd_at,directory);
  std::map<uint64_t,uint64_t> occupied;std::map<std::string,std::pair<ArchiveFile,bool>> paths;uint64_t path_bytes=0;size_t at=0;
  for(uint64_t n=0;n<count;++n){Require(at<=directory.size() && directory.size()-at>=46,"Truncated ZIP central directory entry");const auto* p=directory.data()+at;Require(U32(p)==0x02014b50,"Invalid ZIP central directory signature");const auto flags=U16(p+8),method=U16(p+10),name_size=U16(p+28),extra_size=U16(p+30),comment_size=U16(p+32);const size_t length=46+size_t(name_size)+extra_size+comment_size;
   Require(length<=directory.size()-at && name_size>0,"Truncated ZIP name/extra/comment");Require(!(flags&~0x080e) && (method==0 || method==8),"Encrypted ZIP or unsupported ZIP compression; use Stored or DEFLATE");
   const auto unix_type=(U32(p+38)>>16)&0xf000;Require(unix_type==0 || unix_type==0x4000 || unix_type==0x8000,"ZIP links/special files are unsupported");bool is_dir=false;auto name=Path(std::string(reinterpret_cast<const char*>(p+46),name_size),is_dir);path_bytes+=name.size();Require(path_bytes<=64*1024*1024,"ZIP path metadata exceeds its budget");
   uint64_t bytes=U32(p+24),packed=U32(p+20),local=U32(p+42),disk=U16(p+34);bool found64=false;
   for(size_t x=46+name_size;x<46+size_t(name_size)+extra_size;){Require(46+size_t(name_size)+extra_size-x>=4,"Truncated ZIP extra header");const auto tag=U16(p+x),z=U16(p+x+2);x+=4;Require(z<=46+size_t(name_size)+extra_size-x,"Truncated ZIP extra data");
    if(tag==1){Require(!found64,"Duplicate ZIP64 extra field");found64=true;size_t used=0;const auto take=[&](unsigned width){Require(width<=z-used,"Missing ZIP64 size/offset");const auto v=width==8?U64(p+x+used):U32(p+x+used);used+=width;return v;};if(bytes==0xffffffff)bytes=take(8);if(packed==0xffffffff)packed=take(8);if(local==0xffffffff)local=take(8);if(disk==65535)disk=take(4);}x+=z;
   }
   Require(found64 || (U32(p+24)!=0xffffffff && U32(p+20)!=0xffffffff && U32(p+42)!=0xffffffff && U16(p+34)!=65535),"Missing ZIP64 entry fields");
   Require(disk==0 && bytes<=limit && packed<=limit && local<cd_at && 30<=cd_at-local,"Invalid ZIP entry extent or missing ZIP64 fields");if(is_dir)Require(bytes==0,"ZIP directory has file data");if(method==0)Require(bytes==packed,"Stored ZIP entry lengths differ");
   std::array<uint8_t,30> header{};file.Read(local,header);const auto local_name=U16(header.data()+26),local_extra=U16(header.data()+28);Require(U32(header.data())==0x04034b50 && U16(header.data()+6)==flags && U16(header.data()+8)==method,"ZIP local/central header mismatch");
   const uint64_t data=local+30+local_name+local_extra;Require(data<=cd_at && packed<=cd_at-data,"ZIP payload overlaps its central directory");std::vector<uint8_t> local_text(local_name);file.Read(local+30,local_text);bool local_dir=false;Require(Path(std::string(local_text.begin(),local_text.end()),local_dir)==name && local_dir==is_dir,"ZIP local/central filename mismatch");
   const auto local_crc=U32(header.data()+14),local_packed=U32(header.data()+18),local_bytes=U32(header.data()+22);Require(local_crc==U32(p+16) || ((flags&8) && !local_crc),"ZIP local/central CRC mismatch");
   Require((local_packed==0xffffffff || local_packed==packed || ((flags&8) && !local_packed)) && (local_bytes==0xffffffff || local_bytes==bytes || ((flags&8) && !local_bytes)),"ZIP local/central length mismatch");
   std::vector<uint8_t> extra(local_extra);file.Read(local+30+local_name,extra);bool local64=false;
   for(size_t x=0;x<extra.size();){Require(extra.size()-x>=4,"Truncated local ZIP extra header");const auto tag=U16(extra.data()+x),length=U16(extra.data()+x+2);x+=4;Require(length<=extra.size()-x,"Truncated local ZIP extra data");
    if(tag==1){Require(!local64,"Duplicate local ZIP64 extra field");local64=true;size_t used=0;const auto take=[&](){Require(8<=length-used,"Truncated local ZIP64 size");const auto n=U64(extra.data()+x+used);used+=8;return n;};
     if(local_bytes==0xffffffff){const auto n=take();Require(n==bytes || ((flags&8) && !n),"Local ZIP64 size mismatch");}if(local_packed==0xffffffff){const auto n=take();Require(n==packed || ((flags&8) && !n),"Local ZIP64 compressed size mismatch");}}
    x+=length;
   }
   Require(local64 || (local_bytes!=0xffffffff && local_packed!=0xffffffff),"Missing local ZIP64 sizes");uint64_t payload_end=data+packed;
   if(flags&8){const bool wide=local_bytes==0xffffffff || local_packed==0xffffffff || U32(p+24)==0xffffffff || U32(p+20)==0xffffffff;const size_t fields=wide?20:12;Require(payload_end<=cd_at && fields<=cd_at-payload_end,"ZIP data descriptor is truncated");std::array<uint8_t,24> descriptor{};const auto n=size_t(std::min<uint64_t>(descriptor.size(),cd_at-payload_end));file.Read(payload_end,std::span(descriptor).first(n));
    const auto matches=[&](size_t at){return fields<=n-at && U32(descriptor.data()+at)==U32(p+16) && (wide?U64(descriptor.data()+at+4):U32(descriptor.data()+at+4))==packed && (wide?U64(descriptor.data()+at+12):U32(descriptor.data()+at+8))==bytes;};
    const bool signature=n>=fields+4 && U32(descriptor.data())==0x08074b50 && matches(4);Require(signature || matches(0),"ZIP data descriptor disagrees with its directory");payload_end+=fields+(signature?4:0);
   }
   const auto next=occupied.lower_bound(local);Require((next==occupied.end() || payload_end<=next->first) && (next==occupied.begin() || std::prev(next)->second<=local),"Overlapping ZIP local entries");occupied.emplace(local,payload_end);
   Entry entry;entry.path=name;entry.data=data;entry.packed=packed;entry.size=bytes;entry.crc=U32(p+16);entry.method=method;entry.directory=is_dir;entry.base=size;
   if(!is_dir){Require(bytes<limit-size,"ZIP expanded data exceeds its limit");starts.emplace_back(size,entries.size());size+=bytes+1;unpacked+=bytes;}
   const auto key=Lower(name);auto existing=paths.find(key);Require(existing==paths.end() || (is_dir && existing->second.first.directory && !existing->second.second),"Duplicate ZIP path");paths[key]={{name,entry.base,bytes,is_dir},true};
   for(size_t slash=name.find('/');slash!=std::string::npos;slash=name.find('/',slash+1)){const auto parent=name.substr(0,slash),parent_key=Lower(parent);const auto it=paths.find(parent_key);Require(it==paths.end() || it->second.first.directory,"ZIP file conflicts with a parent directory");if(it==paths.end())paths.emplace(parent_key,std::pair{ArchiveFile{parent,0,0,true},false});}
   entries.push_back(std::move(entry));at+=length;
  }
  Require(at==directory.size() && size>0,"Unsupported trailing ZIP directory record or empty game archive");for(const auto& [key,value]:paths)files.push_back(value.first);
  Hash hash;std::vector<uint8_t> buffer(1024*1024);const auto total=file.size+unpacked;
  for(uint64_t pos=0;pos<file.size;){if(progress && !progress(pos,total))throw Canceled{};const auto n=size_t(std::min<uint64_t>(buffer.size(),file.size-pos));file.Read(pos,std::span(buffer).first(n));hash.Add(std::span(buffer).first(n));pos+=n;}identity=hash.Finish();
  indexing=true;uint64_t done=file.size;
  for(size_t i=0;i<entries.size();++i){auto& entry=entries[i];uint32_t sum=0xffffffff;for(uint64_t pos=0;pos<entry.size;){if(progress && !progress(done,total))throw Canceled{};const auto n=size_t(std::min<uint64_t>(buffer.size(),entry.size-pos));auto out=std::span(buffer).first(n);ReadEntry(i,pos,out);if(entry.method==0)sum=Crc(sum,out);pos+=n;done+=n;}
   if(entry.method==0)Require(~sum==entry.crc,"Stored ZIP entry CRC mismatch");else if(entry.size==0){Reset(i);Finish();}
   if(progress && !progress(done,total))throw Canceled{};
  }indexing=false;
 }
 void Read(uint64_t offset,std::span<uint8_t> out){Require(offset<=size && out.size()<=size-offset,"Read outside ZIP logical stream");auto it=std::upper_bound(starts.begin(),starts.end(),offset,[](uint64_t n,const auto& p){return n<p.first;});Require(it!=starts.begin(),"ZIP logical read has no file");--it;ReadEntry(it->second,offset-it->first,out);}
};
ZipImage::ZipImage(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
ZipImage::~ZipImage()=default;
std::unique_ptr<ZipImage> ZipImage::Open(const std::filesystem::path& path,std::string& error,const std::function<bool(uint64_t,uint64_t)>& progress){try{auto out=std::unique_ptr<ZipImage>(new ZipImage(std::make_unique<Impl>(path,progress)));error.clear();return out;}catch(const Canceled&){error.clear();return {};}catch(const std::exception& e){error=e.what();return {};}}
uint64_t ZipImage::Size()const{return impl_->size;}
std::span<const uint8_t> ZipImage::Identity()const{return impl_->identity;}
const std::vector<ArchiveFile>& ZipImage::Files()const{return impl_->files;}
bool ZipImage::Read(uint64_t at,std::span<uint8_t> out,std::string& error){try{impl_->Read(at,out);error.clear();return true;}catch(const std::exception& e){if(impl_->initialized){inflateEnd(&impl_->active);impl_->initialized=false;}impl_->active_entry=SIZE_MAX;error=e.what();return false;}}
}
