#include "online_clothing_native.h"
#include "online_room_gameplay.h"
#include "online_clothing_texture.h"
#include "em1_nif_container.h"
#include "em1_portable_pixels.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/kernel/kernel_state.h>
#include <rex/logging.h>
#include <rex/cvar.h>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <windows.h>
#include <immintrin.h>

PPC_EXTERN_FUNC(__imp__sub_82559A08);
PPC_EXTERN_FUNC(__imp__sub_82555510);
PPC_EXTERN_FUNC(__imp__sub_82559C00);
PPC_EXTERN_FUNC(__imp__sub_825849C0);
PPC_EXTERN_FUNC(__imp__sub_82584A70);
PPC_EXTERN_FUNC(__imp__sub_82584AC0);
PPC_EXTERN_FUNC(__imp__sub_829EFF08);
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(bool,em2_online_clothing_bind);
REXCVAR_DEFINE_STRING(em2_probe_online_clothing_directory,"","Debug","Private Room clothing source override; empty generates colors from the mounted original archive");

namespace em2::net {
namespace {
bool Readable(uint32_t at,size_t size,uint8_t* base){return base && at>=4096 && uint64_t(at)+size<=0x100000000ull && GuestReadBatch::Readable(base+at,size);}
struct Calls {
    PPCContext& ctx;PPCContext saved;uint8_t* base;uint32_t low=0,stack=0;unsigned fp=_mm_getcsr();DWORD error=GetLastError();std::array<uint8_t,32768> bytes{};
    Calls(PPCContext& c,uint8_t* b):ctx(c),saved(c),base(b){const auto top=c.r1.u32&~15u;if(top>=65536 && Readable(top-32768,32768,b)){low=top-32768;stack=top-4096;std::memcpy(bytes.data(),base+low,bytes.size());ctx.r1.u64=stack;}}
    ~Calls(){if(stack)std::memcpy(base+low,bytes.data(),bytes.size());ctx=saved;_mm_setcsr(fp);SetLastError(error);}
};
struct Scratch {rex::memory::Memory& memory;uint32_t address;Scratch(rex::memory::Memory& m,uint32_t size):memory(m),address(m.SystemHeapAlloc(size,32)){}~Scratch(){if(address)memory.SystemHeapFree(address);}};
bool Preflight(std::span<const uint8_t> bytes,std::string& error){
    try {using namespace em1::nif;Check(bytes.size()<2*1024*1024,"Clothing texture exceeds diagnostic limit");Nif n(bytes);
        Check(n.version==0x14060500 && n.big && n.blocks.size()==3 && n.footer.size()==8 && U32(n.footer,0,true)==1 && U32(n.footer,4,true)==0,"Unrecognized clothing NIF/root");
        Check(n.types[n.blocks[0].type]=="NiSourceTexture" && n.types[n.blocks[1].type]=="NiStringExtraData","Unrecognized clothing texture graph");
        const auto source=n.blocks[0].Data();const auto fields=12u+4u*U32(source,4,true);Check(fields+24==source.size() && !source[fields] && U32(source,fields+5,true)==2,"Clothing source requests an external image");
        const auto kind=n.types[n.blocks[2].type];
        if(kind=="NiPixelData"){Check(em1::ValidatePortableRgbaPixels(n.blocks[2].Data(),true,error),error.c_str());Check(U32(n.blocks[2].Data(),63,true)==9 && U32(n.blocks[2].Data(),71,true)==256 && U32(n.blocks[2].Data(),75,true)==256,"Unexpected clothing mip dimensions");}
        else {Check(kind=="NiPersistentSrcTextureRendererData","Unexpected clothing pixel object");ClothingTexture checked;std::string a,b;
            Check(RecolorOnlinePants(bytes,LobbyCharacter::Mickey,0,checked,a) || RecolorOnlinePants(bytes,LobbyCharacter::Oswald,1,checked,b),"Unrecognized persistent default clothing pixels");}
        error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
}
bool LoadNativeClothingTexture(PPCContext& ctx,uint8_t* base,std::span<const uint8_t> bytes,NativeClothingTexture& out,std::string& error){
    if(out.root){error="Clothing output already owns a native reference";return false;}
    if(!Preflight(bytes,error))return false;
    Calls calls(ctx,base);auto* kernel=rex::kernel::kernel_state();
    if(!calls.stack || !kernel || !kernel->memory()){error="Native clothing load context unavailable";return false;}
    // Allocating/loading invalidates every outer read-only mapping cache.
    GuestReadBatch::CallbackBoundary boundary;
    const auto size=uint32_t(2048+bytes.size());Scratch scratch(*kernel->memory(),size);
    if(!scratch.address || !Readable(scratch.address,size,base) ||
       (uint64_t(scratch.address)<uint64_t(calls.low)+32768 && uint64_t(scratch.address)+size>calls.low)){error="Native clothing scratch unavailable/overlapping stack";return false;}
    const auto stream=scratch.address,input=stream+2048,keep=calls.stack+512;
    std::memset(base+stream,0,size);std::memcpy(base+input,bytes.data(),bytes.size());PPC_STORE_U32(keep,0);
    // Unexpected native exceptions propagate. Ordinary failed loads use the
    // original complete stream destructor exactly once before returning.
    ctx.r3.u64=stream;__imp__sub_82559A08(ctx,base);
    ctx.r3.u64=stream;ctx.r4.u64=input;ctx.r5.u64=bytes.size();__imp__sub_82555510(ctx,base);
    const bool loaded=(ctx.r3.u32&255)!=0;
    const auto status=PPC_LOAD_U32(stream+796),count=PPC_LOAD_U32(stream+360),slots=PPC_LOAD_U32(stream+352);
    uint32_t root=0;bool retained=false,uploaded=false;NativeClothingTexture next;
    if(loaded && !status && count==1 && Readable(slots,4,base)){
        root=PPC_LOAD_U32(slots);
        if(Readable(root,72,base) && PPC_LOAD_U32(root)==0x82041158 && PPC_LOAD_U32(root+4)){
            ctx.r3.u64=keep;ctx.r4.u64=root;__imp__sub_829EFF08(ctx,base);retained=PPC_LOAD_U32(keep)==root && PPC_LOAD_U32(root+4)>=2;
            if(retained){ctx.r3.u64=root;__imp__sub_825849C0(ctx,base);uploaded=(ctx.r3.u32&255)!=0;
                ctx.r3.u64=root;__imp__sub_82584A70(ctx,base);next.width=ctx.r3.u32;
                ctx.r3.u64=root;__imp__sub_82584AC0(ctx,base);next.height=ctx.r3.u32;
                next.root=root;next.renderer_data=PPC_LOAD_U32(root+40);
                uploaded=uploaded && Readable(next.renderer_data,24,base) && next.width==256 && next.height==256;}
        }
    }
    const bool input_unchanged=std::memcmp(base+input,bytes.data(),bytes.size())==0;
    ctx.r3.u64=stream;__imp__sub_82559C00(ctx,base);
    const bool stream_released=PPC_LOAD_U32(stream+360)==0;
    const auto references=retained && Readable(root,8,base)?PPC_LOAD_U32(root+4):0;
    if(!loaded || status || !retained || !uploaded || !input_unchanged || !stream_released || references!=1){
        if(PPC_LOAD_U32(keep)){ctx.r3.u64=keep;ctx.r4.u64=0;__imp__sub_829EFF08(ctx,base);}
        error="Native clothing load/upload/retention failed: loaded="+std::to_string(loaded)+" status="+std::to_string(status)+" retained="+std::to_string(retained)+" uploaded="+std::to_string(uploaded)+" stream_released="+std::to_string(stream_released)+" refs="+std::to_string(references);return false;
    }
    out=next;error.clear();return true;
}
bool ReleaseNativeClothingTexture(PPCContext& ctx,uint8_t* base,NativeClothingTexture& texture,std::string& error){
    if(!texture.root){error.clear();return true;}
    Calls calls(ctx,base);if(!calls.stack || !Readable(texture.root,8,base) || PPC_LOAD_U32(texture.root)!=0x82041158 || !PPC_LOAD_U32(texture.root+4)){error="Native clothing release has no valid owned reference";return false;}
    GuestReadBatch::CallbackBoundary boundary;const auto slot=calls.stack+512;PPC_STORE_U32(slot,texture.root);
    ctx.r3.u64=slot;ctx.r4.u64=0;__imp__sub_829EFF08(ctx,base);
    if(PPC_LOAD_U32(slot)){error="Native clothing reference did not release";return false;}
    texture={};error.clear();return true;
}
bool AssignNativeClothingPointer(PPCContext& ctx,uint8_t* base,uint32_t slot,uint32_t texture){
    Calls calls(ctx,base);if(!calls.stack || !Readable(slot,4,base))return false;
    const auto old=PPC_LOAD_U32(slot);
    for(const auto root:{old,texture})if(root && (!Readable(root,8,base) || PPC_LOAD_U32(root)!=0x82041158 || !PPC_LOAD_U32(root+4)))return false;
    GuestReadBatch::CallbackBoundary boundary;ctx.r3.u64=slot;ctx.r4.u64=texture;__imp__sub_829EFF08(ctx,base);return PPC_LOAD_U32(slot)==texture;
}
bool RetainNativeClothingReference(PPCContext& ctx,uint8_t* base,uint32_t texture,uint32_t& owned){
    if(owned || !texture)return false;Calls calls(ctx,base);if(!calls.stack)return false;
    const auto slot=calls.stack+512;PPC_STORE_U32(slot,0);if(!AssignNativeClothingPointer(ctx,base,slot,texture))return false;owned=texture;return true;
}
bool DropNativeClothingReference(PPCContext& ctx,uint8_t* base,uint32_t& owned){
    if(!owned)return true;Calls calls(ctx,base);if(!calls.stack)return false;
    const auto slot=calls.stack+512;PPC_STORE_U32(slot,owned);if(!AssignNativeClothingPointer(ctx,base,slot,0))return false;owned=0;return true;
}
void ProbeNativeClothingTextures(PPCContext& ctx,uint8_t* base){
    const auto& directory=REXCVAR_GET(em2_probe_online_clothing_directory);
    if(directory.empty() && !REXCVAR_GET(em2_online_clothing_bind))return;
    if(!REXCVAR_GET(em2_private_profile) || (directory.empty() && OnlineClothingSourceKey().empty()))return;
    auto* game=CurrentNativeRoomGameplay();if(!game || !game->active() || game->halted())return;
    static thread_local bool attempted=false;if(attempted)return;attempted=true;
    Calls preserve(ctx,base);if(!preserve.stack)return;
    std::vector<NativeClothingTexture> retained;retained.reserve(32);std::set<uint32_t> roots,renderers;bool success=true;std::string error;
    const auto folder=std::filesystem::path(std::u8string(directory.begin(),directory.end()));
    for(unsigned character=0;character<2 && success;++character)for(unsigned color=0;color<16 && success;++color){
        std::vector<uint8_t> bytes;
        if(directory.empty()){
            ClothingTexture prepared;
            if(!ReadMountedOnlineClothingVariant(LobbyCharacter(character),uint8_t(color),prepared,error)){success=false;break;}
            bytes=std::move(prepared.bytes);
        }else{
        const auto file=folder/(std::string(character?"oswald-":"mickey-")+std::to_string(color)+".nif");
        std::error_code ec;const auto length=std::filesystem::file_size(file,ec);
        if(ec || !length || length>2*1024*1024){error="Clothing fixture missing or oversized";success=false;break;}
        bytes.resize(size_t(length));std::ifstream in(file,std::ios::binary);in.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
        if(!in || in.gcount()!=std::streamsize(bytes.size())){error="Clothing fixture read failed";success=false;break;}
        }
        NativeClothingTexture texture;success=LoadNativeClothingTexture(ctx,base,bytes,texture,error);
        if(success){retained.push_back(texture);success=roots.insert(texture.root).second && renderers.insert(texture.renderer_data).second;if(!success)error="Native clothing variants aliased a source/renderer";
            REXLOG_INFO("Native Room clothing detached texture: character {} color {} root {:08X} renderer {:08X} size {}/{} refs {}; stream destroyed, sole retained native reference, no actor binding",character,color,texture.root,texture.renderer_data,texture.width,texture.height,PPC_LOAD_U32(texture.root+4));}
    }
    unsigned released=0;for(auto it=retained.rbegin();it!=retained.rend();++it){std::string release_error;if(ReleaseNativeClothingTexture(ctx,base,*it,release_error))++released;else{success=false;error+="; "+release_error;}}
    success=success && retained.size()==32 && released==32;
    REXLOG_INFO("Native Room clothing detached validation: success {} loaded {} unique roots {} unique renderers {} released {} error '{}'; original native texture loading/upload/reference/destruction, no player texture changed",success,retained.size(),roots.size(),renderers.size(),released,error);
}
}
