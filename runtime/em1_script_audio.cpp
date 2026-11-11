#include "em1_script_audio.h"
#include "em1_audio_mixer.h"
#include "em1_audio_path.h"
#include "em1_audio_bus.h"
#include "em1_audio_source.h"
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <set>
namespace em2::em1 {
namespace {
void Require(bool condition,const std::string& error){if(!condition)throw std::runtime_error(error);}
std::string BankPath(std::string_view path){
  Require(!path.empty() && path.size()<=240 && path.front()!='/' && path.front()!='\\',"Invalid original soundbank path");
  std::string key(path);
  for(auto& c:key){Require(static_cast<unsigned char>(c)>=32 && c!=127 && c!=':',"Invalid original soundbank path byte");if(c=='\\')c='/';else if(c>='A'&&c<='Z')c+=32;}
  if(key.find('/')==key.npos)key="audio/wii/"+key;
  Require(key.starts_with("audio/wii/") && key.ends_with(".bnk"),"Soundbank belongs outside original Audio/Wii namespace");
  for(size_t at=0;at<key.size();){auto end=key.find('/',at);if(end==key.npos)end=key.size();auto part=std::string_view(key).substr(at,end-at);
    Require(!part.empty() && part!="." && part!="..","Unsafe original soundbank path component");at=end+1;}
  return key;
}
struct Index {std::vector<OriginalAudioDeclaration> declarations;std::map<std::string,RvzFile> files;};
Index MakeIndex(std::string_view identity,const LevelPlan& plan,std::span<const RvzFile> files){
  Require(!identity.empty() && plan.origin_key.starts_with(std::string(identity)+":pak-v1:"),"Audio descriptor belongs to a different original mount");
  std::map<std::string,RvzFile> available;
  for(const auto& file:files){
    std::string lower=file.path;for(auto& c:lower){if(c=='\\')c='/';else if(c>='A'&&c<='Z')c+=32;}
    if(!lower.starts_with("audio/wii/") || !lower.ends_with(".bnk"))continue;
    const auto key=BankPath(file.path);Require(available.emplace(key,file).second,"Ambiguous original soundbank case/slash aliases");
  }
  Index result;
  for(const auto& partition:plan.partitions){
    std::set<size_t> seen;
    for(size_t ordinal=0;ordinal<partition.resources.size();++ordinal){const auto& resource=partition.resources[ordinal];
      if(resource.kind!=LevelPartitionResource::Kind::Audio)continue;
      Require(resource.index<partition.audio.size() && seen.insert(resource.index).second,"Repeated or invalid original audio resource index");
      const auto key=BankPath(partition.audio[resource.index]);const auto found=available.find(key);
      Require(found!=available.end() && found->second.size && found->second.size<=256ull*1024*1024,"Missing/oversized declared original soundbank: "+key);
      result.files.try_emplace(key,found->second);result.declarations.push_back({partition.name,key,resource.authored_flag,ordinal});
      Require(result.declarations.size()<=4096,"Original audio declaration budget exceeded");
    }
    Require(seen.size()==partition.audio.size(),"Original audio declarations lost authored resource order");
  }
  return result;
}
std::string ExpectedOrigin(std::string_view source,std::string_view key){return std::string(source)+":audio-wii46:"+std::string(key);}
bool Inspect(const std::shared_ptr<const WiiAudioBank>& bank,uint32_t event,OriginalAudioEvent& output,std::string& error){
  if(!bank || bank->source_origin.empty()){error="Original event has no source-owned bank";return false;}
  std::vector<uint32_t> actions;if(!WiiEventActions(*bank,event,actions,error))return false;
  for(auto action:actions){WiiAudioAction checked;if(!ReadWiiAudioAction(*bank,action,checked,error))return false;}
  output={bank,event,std::move(actions)};error.clear();return true;
}
uint32_t EventId(std::string_view name){
  Require(!name.empty() && name.size()<=256,"Invalid original audio event name");uint32_t value=0x811c9dc5;
  for(unsigned char c:name){Require(c>0 && c<128,"Original audio event requires supported ASCII conversion");if(c>='A'&&c<='Z')c+=32;value=value*0x01000193u^c;}
  return value;
}
}
bool PrepareLevelOriginalAudio(std::shared_ptr<RvzSource> source,const LevelPlan& plan,std::shared_ptr<LevelOriginalAudio>& output,std::string& error){
  try{
    Require(bool(source),"Original audio has no source mount");const auto index=MakeIndex(source->SourceIdentity(),plan,source->Files());
    auto next=std::make_shared<LevelOriginalAudio>();next->source_=std::move(source);next->source_identity_=next->source_->SourceIdentity();
    next->level_origin_=plan.origin_key;next->entry_partitions_=plan.entry_partitions;next->declarations_=index.declarations;next->files_=index.files;
    output=std::move(next);error.clear();return true;
  }catch(const std::exception& failure){error=failure.what();return false;}
}
bool LevelOriginalAudio::ReadBank(std::string_view name,std::shared_ptr<const WiiAudioBank>& output,std::string& error){
  try{
    const auto key=BankPath(name);
    Require(source_ && source_->SourceIdentity()==source_identity_ && files_.contains(key),"Soundbank is outside this retained original level/source scope");
    if(const auto found=loaded_.find(key);found!=loaded_.end()){output=found->second;error.clear();return true;}
    Require(files_.at(key).size<=512ull*1024*1024-retained_bytes_,"Retained original soundbanks exceed memory budget");
    WiiAudioBank bank;if(!ReadOriginalWiiAudioBank(*source_,key,bank,error))return false;
    Require(bank.source_origin==ExpectedOrigin(source_identity_,key) && bank.bytes && bank.bytes->size()==files_.at(key).size,"Original soundbank byte/source identity changed");
    std::set<std::pair<uint32_t,uint32_t>> objects;std::set<uint32_t> media;
    for(const auto& object:bank.objects)Require(objects.emplace(object.kind,object.id).second,"Duplicate typed original soundbank object identity");
    for(const auto& item:bank.media)Require(media.insert(item.id).second,"Duplicate original soundbank media identity");
    auto retained=std::make_shared<WiiAudioBank>(std::move(bank));loaded_.emplace(key,retained);retained_bytes_+=retained->bytes->size();
    output=std::move(retained);error.clear();return true;
  }catch(const std::exception& failure){error=failure.what();return false;}
}
bool LevelOriginalAudio::InspectEvent(std::string_view bank,std::string_view event,OriginalAudioEvent& output,std::string& error){
  try{
    const auto id=EventId(event);std::shared_ptr<const WiiAudioBank> owner;if(!ReadBank(bank,owner,error))return false;
    return Inspect(owner,id,output,error);
  }catch(const std::exception& failure){error=failure.what();return false;}
}
bool TestLevelOriginalAudioOwnership(){
  try{
    if(!TestAtomicDspEventAdmission() || !TestWiiUserAudioPath() || !TestWiiAudioBusOwnership())return false;
    // Optional read-only original fixture proof, requested explicitly by the
    // headless test runner. Product lookup never depends on this test path.
    if(const auto* directory=std::getenv("EM1_AUDIO_ORIGINAL_FIXTURES")){
      std::ifstream input(std::filesystem::path(directory)/"Init.bnk",std::ios::binary);
      auto bytes=std::make_shared<std::vector<uint8_t>>(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
      WiiAudioBank init;std::string error;if(!input.eof() && input.fail())return false;
      if(!ReadWiiAudioBank(bytes,init,error))return false;
      init.source_origin="original-test:audio-wii46:audio/wii/init.bnk";WiiAudioDefaultControls defaults;
      if(!ReadWiiAudioDefaultControls(init,defaults,error) || defaults.values.size()!=13 || defaults.values.at(0x7e0741ab)!=100)return false;
      std::array<std::shared_ptr<const WiiAudioBank>,1> owners{std::make_shared<WiiAudioBank>(init)};
      std::vector<WiiBusDefinition> route;if(!ReadWiiBusRoute(owners,552651376,route,error) || route.size()!=4 ||
        route[0].id!=0x20f0ca70 || route[1].id!=0x042eb523 || route[2].id!=0x0ef6266a || route[3].id!=0x2101976d ||
        route[2].rtpc.size()!=1 || route[2].rtpc[0].control!=0x7e0741ab || route[2].rtpc[0].points.size()!=2)return false;
      std::array<float,4> parameters;if(!EvaluateWiiInitialBusParameters(route,defaults,parameters,error) || parameters!=std::array<float,4>{0,0,0,0})return false;
      defaults.values[0x7e0741ab]=50;const auto unchanged=parameters;
      if(EvaluateWiiInitialBusParameters(route,defaults,parameters,error) || parameters!=unchanged)return false;
    }
    LevelPlan plan;plan.origin_key="original-a:pak-v1:levels.pak";
    LevelPartition a;a.name="first";a.audio={"A.bnk","B.bnk"};
    a.resources={{LevelPartitionResource::Kind::Audio,1,"-always"},{LevelPartitionResource::Kind::Audio,0,"-resident"}};
    LevelPartition b;b.name="interior";b.audio={"a.bnk"};b.resources={{LevelPartitionResource::Kind::Audio,0,"-stream"}};plan.partitions={a,b};
    const std::vector<RvzFile> files{{"Audio/Wii/A.bnk",80},{"Audio/Wii/B.bnk",160},{"Audio/360/A.bnk",80}};
    auto indexed=MakeIndex("original-a",plan,files);
    if(indexed.files.size()!=2 || indexed.declarations.size()!=3 || indexed.declarations[0].source_path!="audio/wii/b.bnk" ||
      indexed.declarations[1].authored_flag!="-resident" || indexed.declarations[2].partition!="interior")return false;
    try{MakeIndex("original-b",plan,files);return false;}catch(const std::exception&){}
    auto invalid=plan;invalid.partitions[0].audio[0]="audio/360/a.bnk";
    try{MakeIndex("original-a",invalid,files);return false;}catch(const std::exception&){}
    auto absent=files;absent.erase(absent.begin());try{MakeIndex("original-a",plan,absent);return false;}catch(const std::exception&){}
    auto ambiguous=files;ambiguous.push_back({"audio\\WII\\a.BNK",80});try{MakeIndex("original-a",plan,ambiguous);return false;}catch(const std::exception&){}
    auto missing_order=plan;missing_order.partitions[0].resources.pop_back();try{MakeIndex("original-a",missing_order,files);return false;}catch(const std::exception&){}
    auto word=[](std::vector<uint8_t>& out,uint32_t n){for(int shift=24;shift>=0;shift-=8)out.push_back(uint8_t(n>>shift));};
    auto chunk=[&](std::vector<uint8_t>& out,std::string_view tag,const std::vector<uint8_t>& payload){out.insert(out.end(),tag.begin(),tag.end());word(out,uint32_t(payload.size()));out.insert(out.end(),payload.begin(),payload.end());};
    auto bytes=std::make_shared<std::vector<uint8_t>>();std::vector<uint8_t> header,hirc;
    for(auto n:{46u,1u,0u,0u})word(header,n);chunk(*bytes,"BKHD",header);
    const auto event=EventId("Play_Original");word(hirc,3);
    for(auto n:{4u,12u,event,1u,88u,3u,28u,88u,0x04030000u,123u,0u,0u,0u,0u,99u,8u,77u,0xABCDEF12u})word(hirc,n);
    chunk(*bytes,"HIRC",hirc);WiiAudioBank parsed;std::string error;if(!ReadWiiAudioBank(bytes,parsed,error))return false;
    parsed.source_origin=ExpectedOrigin("original-a","audio/wii/a.bnk");auto bank=std::make_shared<WiiAudioBank>(std::move(parsed));OriginalAudioEvent view;
    if(!Inspect(bank,event,view,error) || view.actions!=std::vector<uint32_t>{88} || view.bank->objects.size()!=3 || view.bank->bytes!=bytes)return false;
    bytes.reset();bank.reset();if(!view.bank->bytes || view.bank->objects.back().kind!=99)return false;
    auto untouched=view;if(Inspect(view.bank,event+1,view,error) || view.bank!=untouched.bank || view.actions!=untouched.actions)return false;
    auto newer=std::make_shared<std::vector<uint8_t>>(*view.bank->bytes);(*newer)[11]=62;WiiAudioBank rejected;
    if(ReadWiiAudioBank(newer,rejected,error))return false;
    if(EventId("PLAY_ORIGINAL")!=event)return false;
    return true;
  }catch(...){return false;}
}
}
