#include "restored_gremlin_names.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <atomic>
#include <array>
#include <stdexcept>
#include <cstring>
#include <immintrin.h>
#include <windows.h>
PPC_EXTERN_FUNC(__imp__sub_8277E7F0);
namespace em2 {
void RestoreNativeGremlinSpeaker(PPCContext&ctx,uint8_t*base);
namespace {
std::atomic<bool> enabled{false};
std::string Fold(std::string_view value) {
  std::string out(value);
  for(auto& c:out)if(c>='A'&&c<='Z')c=char(c-'A'+'a');
  return out;
}
bool Generic(std::string_view value) {
  const auto key=Fold(value);
  constexpr std::array colors={"blue gremlin","red gremlin","green gremlin",
    "yellow gremlin","purple gremlin","orange gremlin","white gremlin","black gremlin"};
  for(auto color:colors)if(key==color)return true;
  return false;
}
// The original 829253D8 accessor: first active sequence if its array is
// nonempty, otherwise the controller's fallback sequence. No guessed globals.
template<class R> uint32_t Replacement(R& r,uint32_t lr,uint32_t event,
                                      uint32_t controller,uint32_t speaker) {
  if(lr==0x82990588)controller=r.Word(event+104);
  else if(lr!=0x829A14BC)return 0;
  const auto count=int32_t(r.Word(controller+4));
  if(count<0||count>1024)return 0;
  const auto sequence=count?r.Word(r.Word(controller)):r.Word(controller+20);
  if(!sequence)return 0;
  const auto character=r.Text(r.Word(sequence+4));
  const auto key=r.Text(r.Word(speaker));
  return IsRestorableGremlinSpeaker(character,key)?sequence+4:0;
}
struct Reader {
  uint8_t* base;
  void Span(uint32_t at,size_t n){
    if(!base||at<4096||uint64_t(at)+n>0x100000000ull||
       !GuestReadBatch::Readable(base+at,n))throw std::runtime_error("Unreadable BSQ speaker context");
  }
  uint32_t Word(uint32_t at){Span(at,4);return uint32_t(base[at])<<24|uint32_t(base[at+1])<<16|uint32_t(base[at+2])<<8|base[at+3];}
  std::string Text(uint32_t at){
    std::string out;
    for(uint32_t i=0;i<128;++i){Span(at+i,1);const auto c=base[at+i];if(!c)return out;
      if(c<32||c>=127)throw std::runtime_error("Non-ASCII BSQ name key");out.push_back(char(c));}
    throw std::runtime_error("BSQ name key exceeds budget");
  }
};
}
void ConfigureRestoredGremlinNames(bool value){enabled.store(value,std::memory_order_relaxed);}
bool RestoredGremlinNamesEnabled(){return enabled.load(std::memory_order_relaxed);}
bool IsRestorableGremlinSpeaker(std::string_view character,std::string_view speaker){
  if(!Generic(speaker)||character.size()<=8||character.size()>96)return false;
  const auto lower=Fold(character);
  if(!lower.starts_with("gremlin ")||Generic(character))return false;
  // Proper authored keys are names, not paths/markup or unbounded UI data.
  for(char c:character)if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||c==' '||c=='-'||c=='\''))return false;
  return true;
}
bool TestRestoredGremlinNames(std::string& error){
  try {
    auto check=[](bool okay){if(!okay)throw std::runtime_error("Gremlin speaker restoration regression");};
    check(IsRestorableGremlinSpeaker("Gremlin Tiestow","Green Gremlin"));
    check(IsRestorableGremlinSpeaker("Gremlin Kip","BLUE GREMLIN"));
    check(!IsRestorableGremlinSpeaker("Gremlin Kip","Gremlin Kip"));
    check(!IsRestorableGremlinSpeaker("Mickey","Blue Gremlin"));
    check(!IsRestorableGremlinSpeaker("Blue Gremlin","Blue Gremlin"));
    check(!IsRestorableGremlinSpeaker("Gremlin Tiestow","Mickey"));
    check(!IsRestorableGremlinSpeaker("Gremlin <script>","Green Gremlin"));
    struct Fake {
      std::array<uint8_t,4096> data{};
      uint32_t Word(uint32_t a){if(a>4092)throw std::runtime_error("bounds");return uint32_t(data[a])<<24|uint32_t(data[a+1])<<16|uint32_t(data[a+2])<<8|data[a+3];}
      void Put(uint32_t a,uint32_t v){for(int i=0;i<4;++i)data.at(a+i)=uint8_t(v>>(24-i*8));}
      void PutText(uint32_t a,std::string_view s){for(auto c:s)data.at(a++)=uint8_t(c);data.at(a)=0;}
      std::string Text(uint32_t a){std::string s;while(auto c=data.at(a++))s+=char(c);return s;}
    } r;
    r.Put(100,300);r.Put(104,1);r.Put(300,400);r.Put(404,700);
    r.Put(500,800);r.PutText(700,"Gremlin Tiestow");r.PutText(800,"Green Gremlin");
    check(Replacement(r,0x829A14BC,0,100,500)==404);
    r.Put(104,0);r.Put(120,400);
    check(Replacement(r,0x829A14BC,0,100,500)==404);
    r.Put(1004,100);
    check(Replacement(r,0x82990588,900,0,500)==404);
    check(Replacement(r,0x12345678,900,100,500)==0);
    r.Put(104,0xFFFFFFFF);check(Replacement(r,0x829A14BC,0,100,500)==0);
    r.Put(104,0);r.PutText(800,"Gremlin Kip");check(Replacement(r,0x829A14BC,0,100,500)==0);
    // Exercise the production Windows guest reader and context hook, including
    // cold/guarded spans. These are private host pages, never a live guest.
    struct Pages {
      uint8_t* p=static_cast<uint8_t*>(VirtualAlloc(nullptr,32768,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
      ~Pages(){if(p)VirtualFree(p,0,MEM_RELEASE);}
    } pages;
    check(pages.p!=nullptr);
    struct State {
      bool option=RestoredGremlinNamesEnabled();DWORD last=GetLastError();unsigned fp=_mm_getcsr();
      ~State(){ConfigureRestoredGremlinNames(option);_mm_setcsr(fp);SetLastError(last);}
    } state;
    auto put=[&](uint32_t a,uint32_t v){for(int i=0;i<4;++i)pages.p[a+i]=uint8_t(v>>(24-i*8));};
    auto text=[&](uint32_t a,const char* s){std::memcpy(pages.p+a,s,std::strlen(s)+1);};
    constexpr uint32_t controller=4200,array=4300,sequence=4400,speaker=4500,event=4600;
    put(controller,array);put(controller+4,1);put(array,sequence);put(controller+20,sequence);
    put(sequence+4,5000);put(speaker,5200);put(event+104,controller);
    text(5000,"Gremlin Tiestow");text(5200,"Green Gremlin");
    auto invoke=[&](uint32_t lr,bool option,uint32_t expected,uint32_t source_speaker=0){
      if(!source_speaker)source_speaker=speaker;
      PPCContext ctx{};ctx.lr=lr;ctx.r29.u64=controller;ctx.r30.u64=event;
      ctx.r7.u64=source_speaker;ctx.r3.u64=0xABCDEF1234567890ull;ctx.r1.u64=0xB000;
      auto expected_ctx=ctx;expected_ctx.r7.u64=expected;
      ConfigureRestoredGremlinNames(option);
      const auto fp=state.fp;_mm_setcsr(fp);SetLastError(0xA17B29C3);
      RestoreNativeGremlinSpeaker(ctx,pages.p);
      const auto actual_error=GetLastError();const auto actual_fp=_mm_getcsr();
      check(std::memcmp(&ctx,&expected_ctx,sizeof(ctx))==0);
      check(actual_error==0xA17B29C3&&actual_fp==fp);
    };
    invoke(0x829A14BC,false,speaker);
    invoke(0x829A14BC,true,sequence+4);
    invoke(0x82990588,true,sequence+4);
    put(controller+4,0);invoke(0x829A14BC,true,sequence+4);
    invoke(0x11111111,true,speaker);
    text(5000,"Green Gremlin");invoke(0x829A14BC,true,speaker);
    text(5000,"Mickey");invoke(0x829A14BC,true,speaker);
    text(5000,"Gremlin Tiestow");text(5200,"Mickey");invoke(0x829A14BC,true,speaker);
    text(5200,"Green Gremlin");
    DWORD protection=0;check(VirtualProtect(pages.p+24576,4096,PAGE_NOACCESS,&protection)!=0);
    invoke(0x829A14BC,true,24576,24576);
    put(sequence+4,24576);invoke(0x829A14BC,true,speaker);
    error.clear();return true;
  }catch(const std::exception& e){error=e.what();return false;}
}
void RestoreNativeGremlinSpeaker(PPCContext&ctx,uint8_t*base){
  if(!RestoredGremlinNamesEnabled())return;
  const auto saved_error=GetLastError();
  try {GuestReadBatch batch;Reader reader{base};
    if(auto replacement=Replacement(reader,uint32_t(ctx.lr),ctx.r30.u32,ctx.r29.u32,ctx.r7.u32))
      ctx.r7.u64=replacement;
  }catch(...){/* An unowned/unknown dialogue path keeps its original label. */}
  SetLastError(saved_error);
}
}
PPC_FUNC_IMPL(sub_8277E7F0){
  em2::RestoreNativeGremlinSpeaker(ctx,base);
  __imp__sub_8277E7F0(ctx,base);
}
