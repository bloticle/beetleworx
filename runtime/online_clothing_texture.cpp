#include "online_clothing_texture.h"
#include "em1_nif_container.h"
#include <rex/logging.h>
#include <array>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>

namespace em2::net {
namespace {
using namespace em2::em1::nif;
constexpr unsigned side=256;
constexpr std::array<uint64_t,2> default_pixels{0x06E0902017CFCF90ull,0x3F40C72DB4F7A159ull};
uint64_t Fingerprint(View data){uint64_t h=14695981039346656037ull;for(auto v:data)h=(h^v)*1099511628211ull;return h;}
// Fixed 256x256 BGRA8 source layout, independently checked against Xenos tiled
// addressing documented at https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/texture_address.h
size_t Tiled(unsigned x,unsigned y){
    const unsigned macro=((x>>5)+(y>>5)*8)<<9,micro=((x&7)+((y&6)<<2))<<2;
    const unsigned offset=macro+((micro&~15u)<<1)+(micro&15)+((y&8)<<5)+((y&1)<<4);
    return ((offset&~511u)<<3)+((offset&448)<<2)+(offset&63)+((y&16)<<7)+(((((y&8)>>2)+(x>>3))&3)<<6);
}
Bytes Untile(View raw){
    Check(raw.size()>=side*side*4,"Truncated tiled default diffuse texture");Bytes result(side*side*4);
    for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x){const auto from=Tiled(x,y);const auto to=(y*side+x)*4;
        Check(from+4<=raw.size(),"Default diffuse tile exceeds source");
        result[to]=raw[from+2];result[to+1]=raw[from+1];result[to+2]=raw[from];result[to+3]=raw[from+3];}
    return result;
}
bool Pants(unsigned character,unsigned x,unsigned y,View pixel){
    // Audited default UV islands. Mickey's mouth/tongue uses separate red
    // islands, so color classification alone must never choose clothing.
    if(x<144 || y<(character?185u:181u))return false;
    if(!character)return unsigned(pixel[0])*5>unsigned(pixel[1])*6 && unsigned(pixel[0])*5>unsigned(pixel[2])*6;
    return unsigned(pixel[2])*5>unsigned(pixel[0])*6 && pixel[2]>pixel[1];
}
unsigned Tint(Bytes& pixels,unsigned character,uint32_t rgba){
    const std::array<unsigned,3> target{rgba>>24,(rgba>>16)&255,(rgba>>8)&255};const auto peak=*std::max_element(target.begin(),target.end());
    Check(peak>0,"Black clothing palette is not supported");unsigned changed=0;
    for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x){const auto at=(y*side+x)*4;
        if(!Pants(character,x,y,View(pixels).subspan(at,4)))continue;
        const unsigned light=std::max({pixels[at],pixels[at+1],pixels[at+2]});bool differs=false;
        for(unsigned c=0;c<3;++c){const auto value=uint8_t((target[c]*light+peak/2)/peak);differs|=pixels[at+c]!=value;pixels[at+c]=value;}
        changed+=differs;
    }
    return changed;
}
Bytes Mips(const Bytes& base){
    Bytes pixels=base,previous=base;unsigned width=side;
    while(width>1){const auto next_width=width/2;Bytes next(next_width*next_width*4);
        for(unsigned y=0;y<next_width;++y)for(unsigned x=0;x<next_width;++x)for(unsigned c=0;c<4;++c){unsigned sum=0;
            for(unsigned dy=0;dy<2;++dy)for(unsigned dx=0;dx<2;++dx)sum+=previous[((y*2+dy)*width+x*2+dx)*4+c];
            next[(y*next_width+x)*4+c]=uint8_t((sum+2)/4);}
        Append(pixels,next);previous=std::move(next);width=next_width;
    }
    return pixels;
}
Bytes Portable(View original,const Bytes& pixels,bool big){
    Bytes block;Number(block,1,big);block.push_back(32);Number(block,UINT32_MAX,big);Number(block,0,big);block.push_back(1);Number(block,0,big);block.push_back(original[18]);
    for(unsigned channel=0;channel<4;++channel){Number(block,channel,big);Number(block,0,big);block.push_back(8);block.push_back(0);}
    Number(block,UINT32_MAX,big);Number(block,9,big);Number(block,4,big);unsigned offset=0;
    for(unsigned width=side;width;width/=2){Number(block,width,big);Number(block,width,big);Number(block,offset,big);offset+=width*width*4;}
    Check(offset==pixels.size(),"Incomplete recolored texture mip chain");Number(block,offset,big);Number(block,1,big);Append(block,pixels);return block;
}
Bytes Read(const std::filesystem::path& path){std::ifstream in(path,std::ios::binary);Check(bool(in),"Could not open default clothing source");Bytes b{std::istreambuf_iterator<char>(in),{}};Check(!in.bad() && b.size()<2*1024*1024,"Invalid clothing source read/size");return b;}
}
bool RecolorOnlinePants(std::span<const uint8_t> original,LobbyCharacter character,uint8_t color,ClothingTexture& output,std::string& error){
    try{
        const auto kind=unsigned(character);Check(kind<2 && color<kLobbyColors.size(),"Invalid character or clothing color");
        Nif n(original);Check(n.version==0x14060500 && n.big && n.blocks.size()==3,"Unrecognized default diffuse NIF layout");
        Check(n.footer.size()==8 && U32(n.footer,0,n.big)==1 && U32(n.footer,4,n.big)==0,"Unrecognized default diffuse root");
        Check(n.types[n.blocks[0].type]=="NiSourceTexture" && n.types[n.blocks[2].type]=="NiPersistentSrcTextureRendererData","Unrecognized default diffuse blocks");
        const auto block=n.blocks[2].Data();const auto word=[&](size_t at){return U32(block,at,n.big);};
        Check(block.size()>=195 && word(0)==1 && block[4]==32 && word(14)==1 && word(63)==9 && word(67)==4 && word(71)==side && word(75)==side && word(79)==0,"Unrecognized Xbox default diffuse format");
        constexpr unsigned end=71+9*12;
        Check(word(end+8)==1 && word(end+12)==1 && word(end)==block.size()-end-16,"Unrecognized Xbox diffuse payload");
        auto base=Untile(block.subspan(end+16));Check(Fingerprint(base)==default_pixels[kind],"Default diffuse pixels differ from the audited clothing UV layout");
        ClothingTexture next;next.mip_levels=9;
        if(color==kind){next.bytes.assign(original.begin(),original.end());output=std::move(next);error.clear();return true;}
        next.changed_pixels=Tint(base,kind,kLobbyColors[color].rgba);Check(next.changed_pixels>4000,"Default pants region was not found");
        n.blocks[2].type=n.Type("NiPixelData");n.blocks[2].replacement=Portable(block,Mips(base),n.big);
        auto& source=n.blocks[0];source.replacement.assign(source.original.begin(),source.original.end());
        const auto source_bytes=View(source.replacement);const auto fields=12u+4u*U32(source_bytes,4,n.big);
        Check(fields+24==source_bytes.size() && U32(source_bytes,fields+5,n.big)==2,"Unrecognized default source texture link");
        source.replacement[fields]=0;source.replacement[source.replacement.size()-2]=0;source.replacement.back()=0;
        next.bytes=n.Write();output=std::move(next);error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
bool TestOnlineClothingTextures(){
    bool ok=true;unsigned checks=0;auto check=[&](bool value,const char* why){++checks;if(!value){ok=false;REXLOG_ERROR("Online clothing texture: {}",why);}};
    std::vector<bool> visited(side*side);Bytes tiled(side*side*4);
    for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x){const auto at=Tiled(x,y);check(at%4==0 && at/4<visited.size() && !visited[at/4],"tiled texels must be bounded and unique");visited[at/4]=true;
        tiled[at]=uint8_t(x);tiled[at+1]=uint8_t(y);tiled[at+2]=uint8_t(x^y);tiled[at+3]=255;}
    const auto decoded=Untile(tiled);bool correct=true;for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x){const auto at=(y*side+x)*4;correct&=decoded[at]==uint8_t(x^y) && decoded[at+1]==y && decoded[at+2]==x && decoded[at+3]==255;}check(correct,"BGRA tile decode");
    for(unsigned character=0;character<2;++character){Bytes before(side*side*4);for(size_t at=0;at<before.size();at+=4){before[at]=character?30:120;before[at+1]=40;before[at+2]=character?120:30;before[at+3]=255;}
        const auto button=(220*side+190)*4;before[button]=before[button+1]=before[button+2]=190;auto after=before;
        check(Tint(after,character,kLobbyColors[2].rgba)>4000,"pants recolored");bool isolated=true;
        for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x){const auto at=(y*side+x)*4;isolated&=after[at+3]==before[at+3];if(!Pants(character,x,y,View(before).subspan(at,4)))isolated&=std::equal(before.begin()+at,before.begin()+at+4,after.begin()+at);}
        check(isolated,"non-pants texels, buttons and alpha retained");const auto chain=Mips(after);check(chain.size()==349524,"complete RGBA mip chain");}
    ClothingTexture untouched;untouched.bytes={1,2,3};std::string error;check(!RecolorOnlinePants({},LobbyCharacter::Mickey,16,untouched,error) && untouched.bytes==Bytes({1,2,3}),"failure preserves caller output");
    if(ok)REXLOG_INFO("Online clothing texture passed {} tile/bounds/channel/isolation/button/alpha/mip/failure checks; real source and native rendering separate",checks);return ok;
}
bool WriteOnlineClothingVariants(const std::filesystem::path& source,const std::filesystem::path& destination,std::string& error){
    try{
        Check(!std::filesystem::exists(destination),"Clothing diagnostic refuses an existing destination");
        struct File {std::string name;ClothingTexture texture;};std::vector<File> files;
        const char* sources[]={"MickeyMouseNG_D_tex.nif","OswaldNG_D_tex.nif"};
        for(unsigned kind=0;kind<2;++kind){const auto original=Read(source/sources[kind]);
            for(unsigned color=0;color<kLobbyColors.size();++color){ClothingTexture result;std::string why;
                Check(RecolorOnlinePants(original,LobbyCharacter(kind),uint8_t(color),result,why),why.c_str());
                Check(color!=kind || result.bytes==original,"Default clothing must remain byte-exact");
                Nif prepared(result.bytes);Check(prepared.blocks.size()==3,"Clothing output block count changed");
                if(color!=kind){const auto b=prepared.blocks[2].Data();Check(prepared.types[prepared.blocks[2].type]=="NiPixelData" && U32(b,63,true)==9 && U32(b,71+8*12,true)==1,"Portable clothing mip metadata failed");}
                files.push_back({std::string(kind?"oswald-":"mickey-")+std::to_string(color)+".nif",std::move(result)});
            }
            ClothingTexture rejected;auto changed=original;changed.back()^=1;
            Check(!RecolorOnlinePants(changed,LobbyCharacter(kind),2,rejected,error),"Damaged source footer was accepted");
        }
        Check(std::filesystem::create_directory(destination),"Could not create clothing diagnostic destination");std::ofstream report(destination/"clothing-variants.json");report<<"{\"native_binding\":false,\"variants\":[";
        for(size_t i=0;i<files.size();++i){const auto& file=files[i];std::ofstream out(destination/file.name,std::ios::binary);out.write(reinterpret_cast<const char*>(file.texture.bytes.data()),file.texture.bytes.size());Check(out.good(),"Could not write clothing variant");
            report<<(i?",":"")<<"{\"file\":\""<<file.name<<"\",\"mips\":"<<file.texture.mip_levels<<",\"changed_base_pixels\":"<<file.texture.changed_pixels<<"}";}
        report<<"]}\n";Check(report.good(),"Could not write clothing report");error.clear();REXLOG_INFO("Prepared {} default clothing variants; original assets unchanged, native per-instance binding NOT installed",files.size());return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
}
