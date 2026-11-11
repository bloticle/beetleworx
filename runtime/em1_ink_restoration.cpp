#include "em1_ink_restoration.h"
#include "em1_model_closure.h"
#include "em1_nif_container.h"
#include "em1_scene_files.h"
#include <map>
#include <set>

namespace em2::em1 {
namespace {
using nif::Bytes;
using nif::Check;
using nif::View;
constexpr size_t limit=128*1024*1024;
uint32_t word(View b,size_t at){return nif::U32(b,at,true);}
void store(Bytes& b,size_t at,uint32_t n){
  Check(at<=b.size() && b.size()-at>=4,"Invalid ink asset word");
  for(unsigned i=0;i<4;++i)b[at+i]=uint8_t(n>>(24-8*i));
}
std::string lower(std::string s){
  for(auto& c:s){if(c=='\\')c='/';if(c>='A'&&c<='Z')c+=32;}
  return s;
}
std::string string_at(View b,size_t at,size_t end){
  Check(at<end && end<=b.size(),"Invalid ink asset string");
  auto first=b.begin()+at,last=std::find(first,b.begin()+end,0);
  Check(last!=b.begin()+end,"Unterminated ink asset string");
  return std::string(first,last);
}
struct Entry {
  std::string folder,name;
  uint32_t type=0,size=0;
  View packed;
  Bytes replacement;
};
std::vector<Entry> records(View pak){
  Check(pak.size()>=32 && pak.size()<limit && word(pak,0)==0x204B4150 &&
      word(pak,4)==2 && word(pak,8)==0 && word(pak,12)==32,"Unsupported global PAK header");
  const size_t end=32ull+word(pak,16);
  const auto count=word(pak,32);
  const size_t strings=36ull+24ull*count;
  Check(count<=8192 && strings<=end && end<=pak.size(),"Invalid global PAK directory");
  std::vector<Entry> entries;size_t pos=end;std::set<std::string> paths;
  for(uint32_t i=0;i<count;++i){
    size_t at=36+24*i;Entry e;
    e.size=word(pak,at);const auto packed=word(pak,at+4),aligned=word(pak,at+8);
    Check(packed<=aligned && !(aligned%32) && aligned<=pak.size()-pos,"Invalid global PAK payload");
    e.folder=string_at(pak,strings+word(pak,at+12),end);
    e.name=string_at(pak,strings+word(pak,at+20),end);
    e.type=word(pak,at+16);e.packed=pak.subspan(pos,packed);pos+=aligned;
    Check(paths.insert(lower(e.folder+"/"+e.name)).second,"Duplicate original PAK path");
    entries.push_back(std::move(e));
  }
  Check(pos==pak.size(),"Unexpected global PAK tail");
  return entries;
}
Bytes encode(const std::vector<Entry>& entries,View original){
  Bytes out(original.begin(),original.begin()+32);nif::Number(out,uint32_t(entries.size()),true);
  Bytes strings;std::map<std::string,uint32_t> offsets;
  auto put=[&](const std::string& s){
    auto [it,added]=offsets.emplace(s,uint32_t(strings.size()));
    if(added){strings.insert(strings.end(),s.begin(),s.end());strings.push_back(0);}
    return it->second;
  };
  for(const auto& e:entries){
    const auto bytes=e.replacement.empty()?e.packed:View(e.replacement);
    const uint32_t packed=uint32_t(bytes.size());
    for(auto v:{e.replacement.empty()?e.size:packed,packed,(packed+31u)&~31u,put(e.folder),e.type,put(e.name)})
      nif::Number(out,v,true);
  }
  nif::Append(out,strings);out.resize((out.size()+31)&~size_t(31));store(out,16,uint32_t(out.size()-32));
  for(const auto& e:entries){
    nif::Append(out,e.replacement.empty()?e.packed:View(e.replacement));
    out.resize((out.size()+31)&~size_t(31));Check(out.size()<limit,"Ink overlay exceeds PAK budget");
  }
  return out;
}
struct ChildArray {size_t count_at=0,end=0;std::vector<std::string> templates;};
ChildArray player_children(View b){
  Check(word(b,0)==0x01000001,"Unexpected native player palette");
  const size_t boundary=4ull+word(b,4);Check(word(b,boundary)==0x02000002,"Invalid native palette records");
  auto text=[&](uint32_t off){return string_at(b,size_t(off)+6,boundary);};
  size_t pos=boundary+16;ChildArray result;
  const auto entities=word(b,boundary+8);Check(entities<=10000,"Invalid palette entity count");
  for(uint32_t i=0;i<entities;++i){
    const auto entity=text(word(b,pos));const auto components=word(b,pos+20);pos+=24;
    for(uint32_t j=0;j<components;++j){
      auto klass=text(word(b,pos));auto properties=word(b,pos+16);pos+=20;
      for(uint32_t k=0;k<properties;++k){
        auto property=text(word(b,pos)),type=text(word(b,pos+4));
        auto mode=word(b,pos+8),count=mode&1?word(b,pos+12):1;
        const size_t width=type=="Point2"?8:type=="Point3"||type=="Color (RGB)"?12:
          type=="Color (RGBA)"?16:type=="Matrix3"?36:4;
        Check(pos+16<=b.size() && count<=(b.size()-pos-16)/width,"Invalid palette property span");
        if(lower(entity)=="player" && klass=="ChildComponent" && property=="Child Templates"){
          Check(!result.count_at && type=="String" && (mode&1),"Invalid Player.Child Templates");
          result.count_at=pos+12;result.end=pos+16+count*4;
          for(uint32_t m=0;m<count;++m)result.templates.push_back(text(word(b,pos+16+m*4)));
        }
        pos+=16+count*width;
      }
    }
  }
  Check(result.count_at && pos+uint64_t(word(b,boundary+12))*4==b.size(),"Incomplete player palette");
  return result;
}
Bytes extend_children(View retail,const std::vector<std::string>& names){
  const auto row=player_children(retail);const size_t old_end=4ull+word(retail,4);
  Bytes text,links;
  for(const auto& s:names){
    Check(std::find(row.templates.begin(),row.templates.end(),s)==row.templates.end(),"Ink child already present");
    nif::Number(links,uint32_t(old_end-4+text.size()),true);
    auto bytes=(s.size()+1+5)&~size_t(3);Check(bytes<=255,"Ink child name too long");
    auto start=text.size();text.push_back(uint8_t(bytes));text.push_back(uint8_t(s.size()+1));
    text.insert(text.end(),s.begin(),s.end());text.resize(start+bytes,0);
  }
  Bytes b(retail.begin(),retail.end());store(b,row.count_at,uint32_t(row.templates.size()+names.size()));
  b.insert(b.begin()+row.end,links.begin(),links.end());
  b.insert(b.begin()+old_end,text.begin(),text.end());store(b,4,uint32_t(old_end-4+text.size()));
  auto expected=row.templates;expected.insert(expected.end(),names.begin(),names.end());
  Check(player_children(b).templates==expected,"Native player child list did not round-trip");
  return b;
}
Bytes ribbon_material(View input,std::string_view technique){
  nif::Nif model(input);unsigned count=0,source_textures=0,general_steps=0;std::set<uint32_t> pixels;
  const auto original_count=model.blocks.size();
  for(size_t index=0;index<original_count;++index){
    auto& block=model.blocks[index];
    if(model.types[block.type]=="NiPSSimulatorGeneralStep"){
      // The COLOR-free native shader uses the original empty color track's
      // white/default alpha. Refuse assets with authored color/alpha curves.
      const auto bytes=block.Data();
      Check(bytes.size()==28 && std::all_of(bytes.begin(),bytes.end(),
            [](uint8_t value){return value==0;}),
            "Ink shader requires the original empty particle color track");
      ++general_steps;
    }
    if(model.types[block.type]=="NiSourceTexture"){
      const auto bytes=block.Data();const size_t at=12ull+4ull*word(bytes,4);
      Check(at+24==bytes.size() && !bytes[at],"Ink texture was not embedded");
      const auto pixel=word(bytes,at+5);
      Check(pixel<original_count && model.types[model.blocks[pixel].type]=="NiPixelData",
            "Ink texture lacks its original adapted pixel data");
      pixels.insert(pixel);++source_textures;
    }
    if(model.types[block.type]!="RibbonParticleSystem")continue;
    auto bytes=block.Data();nif::Cursor c{bytes,0,model.big};c.Word();
    auto extra=c.Word();Check(extra<=128,"Unexpected ribbon extra-data count");Bytes tint;
    for(unsigned i=0;i<extra;++i){
      const auto link=c.Word();Check(link<original_count,"Invalid ribbon extra-data link");
      const auto& value=model.blocks[link];
      if(model.types[value.type]=="NiColorExtraData"){
        auto color=value.Data();Check(color.size()==20,"Invalid ribbon tint data");
        if(model.String(word(color,0))=="Color_Register_1"){
          Check(tint.empty(),"Ambiguous original ribbon tint");
          tint.assign(color.begin(),color.end());
        }
      }
    }
    Check(!tint.empty(),"Original ribbon tint missing");
    const auto extras_end=c.at;c.Word();c.Take(54);
    auto props=c.Word();c.Take(size_t(props)*4);c.Word();
    Check(c.Word()==1,"Unsupported ink ribbon material table");
    auto name_at=c.at;Check(model.String(c.Word())=="Eds_Ribbon_with_Tinted_Highlight","Unexpected original ink ribbon technique");
    c.Word();Check(c.Word()==0,"Original ribbon material is inactive");
    Bytes out(bytes.begin(),bytes.end());Check(model.big,"Unexpected original ribbon endian");
    store(out,name_at,model.AddString(technique));
    store(tint,0,model.AddString("EnvMapHighlightColour"));
    Bytes extra_link;
    nif::Number(extra_link,model.AddBlock("NiColorExtraData",std::move(tint)),true);
    out.insert(out.begin()+extras_end,extra_link.begin(),extra_link.end());store(out,4,extra+1);
    model.blocks[index].replacement=std::move(out);++count;
  }
  Check(general_steps==1,"Ink shader requires exactly one original particle general step");
  Check(count==1 && source_textures==2 && pixels.size()==2,"Incomplete authored ink ribbon or texture closure");
  return model.Write();
}
void bind_mickey_bone(Bytes& scene){
  // The six authored attachments use the Havok skeleton's names. EM2 renamed
  // SKEL_elbow to LowerArm. Forearm is a separate twist joint in BOTH rigs.
  static constexpr std::pair<std::string_view,std::string_view> bones[]={
    {"SKEL_ear_L","LeftEar"},{"SKEL_ear_R","RightEar"},
    {"SKEL_hand_L","LeftHand"},{"SKEL_hand_R","RightHand"},
    {"SKEL_elbow_L","LeftLowerArm"},{"SKEL_elbow_R","RightLowerArm"}};
  const size_t end=4ull+word(scene,4);
  Check(end<=scene.size(),"Invalid ink attachment string table");
  unsigned converted=0;
  for(size_t at=8;at<end;){
    Check(end-at>=2,"Truncated attachment string");
    const auto stride=scene[at],length=scene[at+1];
    Check(length>0 && stride>=2+length && stride%4==0 && stride<=end-at &&
          !scene[at+1+length],"Invalid ink attachment string");
    const std::string_view current(reinterpret_cast<const char*>(scene.data()+at+2),length-1);
    for(const auto& [source,target]:bones)if(current==source){
      Check(target.size()+3<=stride,"Native bone name exceeds the original string record");
      std::fill(scene.begin()+at+2,scene.begin()+at+stride,0);
      std::copy(target.begin(),target.end(),scene.begin()+at+2);
      scene[at+1]=uint8_t(target.size()+1);++converted;break;
    }
    at+=stride;
  }
  Check(converted==1,"Original ink template requires one known attachment bone");
}
}
bool BuildOriginalMickeyInkPak(std::shared_ptr<RvzSource> source,View retail,std::string_view technique,
    OriginalInkPak& output,std::string& error){
  try {
    Check(source && !technique.empty() && technique.size()<=100,"Original ink requires mounted EM1 and its ribbon shader");
    auto entries=records(retail);auto player=std::find_if(entries.begin(),entries.end(),[](const Entry& e){return lower(e.folder+"/"+e.name)=="palettes/_dynamic.bin";});
    Check(player!=entries.end() && player->type==0 && player->size<4*1024*1024,"Missing retail player palette");
    Bytes target(player->size);
    if(player->size==player->packed.size())std::copy(player->packed.begin(),player->packed.end(),target.begin());
    else if(!InflateZlib(player->packed,target,error))return false;
    constexpr auto archive="packfiles/_dynamic.pak";
    auto pak=PakSource::Open(source,archive,error);if(!pak)return false;
    Bytes source_palette;if(!pak->ReadOriginal("palettes/_dynamic.bin",source_palette,error))return false;
    SceneAdapterResult scene;if(!AdaptScene(source_palette,scene,error))return false;
    std::vector<std::string> names;
    for(const auto& name:player_children(scene.bytes).templates)
      if(name.starts_with("mk_ear_") || name.starts_with("mk_hand_") || name.starts_with("mk_elbow_"))names.push_back(name);
    Check(names.size()==16 && std::set<std::string>(names.begin(),names.end()).size()==16,"Original Mickey drip roster changed");
    player->replacement=extend_children(target,names);
    std::map<std::string,std::string> models;
    std::set<std::string> paths;
    for(const auto& e:entries)paths.insert(lower(e.folder+"/"+e.name));
    auto add=[&](std::string path,uint32_t type,Bytes bytes){
      Check(paths.insert(lower(path)).second,"Ink addition collides with a stock resource");
      size_t slash=path.rfind('/');Entry e;e.folder=path.substr(0,slash);e.name=path.substr(slash+1);
      e.type=type;e.replacement=std::move(bytes);entries.push_back(std::move(e));
    };
    for(const auto& name:names){
      auto path="palettes/fx_characters/mickey/"+name+".bin";Bytes data;
      if(!pak->ReadOriginal(path,data,error) || !AdaptScene(data,scene,error))return false;
      std::vector<SceneFileReference> refs;if(!CollectSceneFileReferences(scene,refs,error))return false;
      for(const auto& ref:refs){
        if(ref.property!="NIF File Path" || ref.path.empty())continue;
        const auto key=lower(ref.path);
        Check(key=="effects/characters/mickey/mk_ear_l1.nif" || key=="effects/characters/mickey/mk_ear_l2.nif" ||
              key=="effects/characters/mickey/mk_hand_l.nif","Unexpected original ink asset path");
        if(models.contains(key))continue;
        const auto native="em1/runtime/ink/"+key.substr(key.rfind('/')+1);
        ModelClosure closure;if(!ReadModelClosure(source,archive,key+"_wii",closure,error))return false;
        Check(closure.missing_texture_origins.empty(),"Original ink ribbon texture unavailable");
        add(native,0x4e494600,ribbon_material(closure.model.bytes,technique));models.emplace(key,native);
      }
      SceneAdapterResult bound;if(!BindSceneFilePaths(scene,models,bound,error))return false;
      bind_mickey_bone(bound.bytes);
      add(path,0,std::move(bound.bytes));
    }
    Check(models.size()==3,"Original ink model closure is incomplete");
    OriginalInkPak result;result.children=uint32_t(names.size());result.ribbon_models=uint32_t(models.size());
    result.bytes=encode(entries,retail);Check(records(result.bytes).size()==entries.size(),"Generated global PAK failed readback");
    output=std::move(result);error.clear();return true;
  }catch(const std::exception& e){error=e.what();return false;}
}
}
