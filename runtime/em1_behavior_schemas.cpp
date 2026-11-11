#include "em1_behavior_schemas.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <windows.h>
#include <immintrin.h>
#include <cstring>
#include <array>
#include <set>
#include <stdexcept>
PPC_EXTERN_FUNC(__imp__sub_8238E9A0);
namespace em2::em1 { namespace {
void Check(bool okay,const char*why){if(!okay)throw std::runtime_error(why);}
struct Restore{PPCContext&ctx;PPCContext saved;DWORD error=GetLastError();unsigned csr=_mm_getcsr();Restore(PPCContext&c):ctx(c),saved(c){}~Restore(){ctx=saved;SetLastError(error);_mm_setcsr(csr);}};
struct Reader{
 uint8_t*base;
 void Span(uint32_t a,size_t n){Check(base&&a>=4096&&n&&uint64_t(a)+n<=0x100000000ull&&GuestReadBatch::Readable(base+a,n),"Native behavior reflection pointer is unreadable");}
 uint32_t Word(uint32_t a){Span(a,4);return(uint32_t(base[a])<<24)|(uint32_t(base[a+1])<<16)|(uint32_t(base[a+2])<<8)|base[a+3];}
 uint16_t Short(uint32_t a){Span(a,2);return uint16_t(base[a]<<8)|base[a+1];}
 uint8_t Byte(uint32_t a){Span(a,1);return base[a];}
 std::string Text(uint32_t a){if(!a)return{};std::string s;for(unsigned i=0;i<256;++i){auto c=Byte(a+i);if(!c)return s;Check(c>=32&&c<127,"Native behavior class/member name is invalid");s.push_back(char(c));}throw std::runtime_error("Native behavior reflection name exceeds budget");}
 std::string Klass(uint32_t a){return a?Text(Word(a)):std::string{};}
 uint32_t Size(uint8_t type,uint8_t sub,uint32_t klass){if(type==24||type==31)type=sub;switch(type){case 1:case 2:case 3:case 4:return 1;case 5:case 6:case 32:return 2;case 7:case 8:case 11:case 20:case 21:case 29:case 30:case 33:return 4;case 9:case 10:return 8;case 12:case 13:return 16;case 14:case 15:case 16:return 48;case 17:case 18:return 64;case 22:return 12;case 26:case 28:return 8;case 25:Check(klass!=0,"Native reflected struct has no class");return Word(klass+8);default:throw std::runtime_error("Unsupported native reflected default type");}}
};
}
bool ReadNativeBehaviorSchemas(PPCContext&ctx,uint8_t*base,std::span<const std::string> requested,NativeBehaviorSchemas&out,std::string&error){
 Restore restore(ctx);
 try{
  Check(!requested.empty()&&requested.size()<=512,"Native behavior schema request budget exceeded");Reader r{base};
  Check(ctx.r1.u32>=0x5000&&(ctx.r1.u32&15)==0,"Native behavior schema stack unavailable");r.Span(ctx.r1.u32-16384,16384);
  const auto singleton=r.Word(0x833CCF18);const auto table=r.Word(singleton);
  Check(r.Word(table+12)==0x824F2490,"Native built-in class registry getter differs");
  //824F2490 is exactly `return global[833E6534]`, no side effects.
  const auto registry=r.Word(0x833E6534);Check(r.Word(r.Word(registry)+12)==0x824F2520,"Native class-name lookup differs");
  const auto slots=r.Word(registry+12),count=r.Word(registry+16),mask=r.Word(registry+20);
  Check(mask<65536&&((mask+1)&mask)==0&&count<=mask,"Native class registry shape invalid");r.Span(slots,(mask+1)*12ull);
  std::map<std::string,uint32_t> classes;
  for(uint32_t i=0;i<=mask;++i){auto at=slots+i*12;if(r.Word(at)==UINT32_MAX)continue;auto name=r.Text(r.Word(at+4));auto klass=r.Word(at+8);Check(!name.empty()&&name==r.Klass(klass)&&classes.emplace(name,klass).second,"Native class registry entry conflicts");}
  Check(classes.size()==count,"Native class registry changed during snapshot");
  NativeBehaviorSchemas ready;std::vector<std::string> pending(requested.begin(),requested.end());
  for(size_t cursor=0;cursor<pending.size();++cursor){Check(pending.size()<=2048,"Native behavior schema closure exceeds budget");auto name=pending[cursor];if(ready.classes.contains(name))continue;auto found=classes.find(name);Check(found!=classes.end(),"Required native behavior class is not registered");auto at=found->second;r.Span(at,48);NativeBehaviorClass c;c.name=name;c.parent=r.Klass(r.Word(at+4));c.size=r.Word(at+8);c.version=r.Word(at+44);c.flags=r.Word(at+40);Check(c.size&&c.size<=65536,"Native behavior class size exceeds budget");if(!c.parent.empty())pending.push_back(c.parent);
   const auto members=r.Word(at+24),number=r.Word(at+28),defaults=r.Word(at+32);Check(number<=1024,"Native behavior member budget exceeded");if(number)r.Span(members,number*24ull);
   for(uint32_t i=0;i<number;++i){auto m=members+i*24;BehaviorMember v;v.name=r.Text(r.Word(m));auto klass=r.Word(m+4);v.klass=r.Klass(klass);v.type=r.Byte(m+12);v.subtype=r.Byte(m+13);v.array_size=r.Short(m+14);v.flags=r.Short(m+16);v.offset=r.Short(m+18);Check(!v.name.empty()&&v.offset<c.size,"Native behavior member extent invalid");if(!v.klass.empty())pending.push_back(v.klass);
    const auto enumeration=r.Word(m+8);if(enumeration){const auto items=r.Word(enumeration+4),n=r.Word(enumeration+8);Check(n<=2048,"Native behavior enum budget exceeded");if(n)r.Span(items,n*8ull);for(uint32_t j=0;j<n;++j)v.enumeration.push_back({int32_t(r.Word(items+j*8)),r.Text(r.Word(items+j*8+4))});}
    if(defaults){const auto off=int32_t(r.Word(defaults+i*4));Check(off>=-2&&off<1024*1024,"Native behavior default offset invalid");if(off==-2)v.default_bytes=std::vector<uint8_t>{};else if(off>=0){auto size=r.Size(v.type,v.subtype,klass);if(v.array_size){Check(size<=65536/v.array_size,"Native behavior default inline-array overflow");size*=v.array_size;}Check(size&&size<=65536&&v.offset+uint64_t(size)<=c.size,"Native behavior default extent invalid");r.Span(defaults+uint32_t(off),size);v.default_bytes=std::vector<uint8_t>(base+defaults+off,base+defaults+off+size);if(v.type==20||v.type==29||v.type==33)Check(r.Word(defaults+uint32_t(off))==0,"Non-null native pointer default needs relocation, not a byte copy");}}
    c.members.push_back(std::move(v));
   }
   // Exact82EC6D60 class-signature verification ABI. This may use transient
   // native hash buffers, so no read-cache state crosses the callback.
   {GuestReadBatch::CallbackBoundary boundary;ctx=restore.saved;const auto stack=ctx.r1.u32-8192;PPC_STORE_U32(stack,ctx.r1.u32);ctx.r1.u64=stack;ctx.r3.u64=at;ctx.r4.u64=0;__imp__sub_8238E9A0(ctx,base);c.signature=ctx.r3.u32;c.signature_verified=true;}
   ready.classes.emplace(name,std::move(c));
  }
  Check(r.Word(0x833CCF18)==singleton&&r.Word(0x833E6534)==registry&&r.Word(registry+12)==slots&&r.Word(registry+16)==count&&r.Word(registry+20)==mask,"Native behavior reflection changed during ownership copy");out=std::move(ready);error.clear();return true;
 }catch(const std::exception&e){error=e.what();return false;}
}
bool TestNativeBehaviorSchemaRejection(std::string&error){PPCContext ctx{};ctx.r24.u64=0x12345678;const auto before=ctx;const auto csr=_mm_getcsr();SetLastError(0x5a1234);NativeBehaviorSchemas out;NativeBehaviorClass marker;marker.name="preserved";out.classes.emplace(marker.name,marker);const std::array<std::string,1>names{"hkbProjectData"};if(ReadNativeBehaviorSchemas(ctx,nullptr,names,out,error)||error.empty()||out.classes.size()!=1||!out.classes.contains("preserved")||std::memcmp(&ctx,&before,sizeof ctx)||GetLastError()!=0x5a1234||_mm_getcsr()!=csr){error="Native behavior schema rejection changed host/PPC state";return false;}error.clear();return true;}
}
