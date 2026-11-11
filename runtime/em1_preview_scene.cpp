#include "em1_preview_scene.h"
#include <algorithm>
#include <bit>
#include <map>
#include <set>
#include <stdexcept>
#include <cmath>

namespace em2::em1 {
namespace {
using Bytes=std::vector<uint8_t>;
void Check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
uint32_t Word(std::span<const uint8_t> b,size_t at) {
  Check(at<=b.size() && b.size()-at>=4,"Truncated original scene");
  return uint32_t(b[at])<<24|uint32_t(b[at+1])<<16|uint32_t(b[at+2])<<8|b[at+3];
}
void Set(Bytes& b,size_t at,uint32_t value){for(unsigned i=0;i<4;++i)b.at(at+i)=uint8_t(value>>(24-8*i));}
void Append(Bytes& b,uint32_t value){for(int s=24;s>=0;s-=8)b.push_back(uint8_t(value>>s));}
size_t Stride(std::string_view t) {
  if(t=="Point2")return 8;
  if(t=="Point3" || t=="Color (RGB)")return 12;
  if(t=="Color (RGBA)")return 16;
  if(t=="Matrix3")return 36;
  return 4; // AdaptScene already validated the complete property type domain.
}
struct Property {std::string name,type;Bytes record;};
struct Component {uint32_t klass,version,id,master;std::string name;std::vector<Property> properties;std::set<uint32_t> lineage;};
struct Entity {uint32_t id,master,flags;std::string name;std::vector<Component> components;uint32_t name_offset=0;};
std::string Lower(std::string s){for(auto& c:s){if(c=='\\')c='/';if(c>='A'&&c<='Z')c+='a'-'A';}return s;}
}
bool ReadCourtyardArchitectureScene(std::shared_ptr<RvzSource> source,
    std::string_view published_model_name,ArchitectureScene& output,std::string& error,
    std::string_view gear_model_name,std::string_view gear_physics_name,unsigned gear_number,uint32_t particle_entity) {
  try {
    Check(gear_number==1 || gear_number==2,"Unknown authored courtyard gear");
    Check(published_model_name.starts_with("em1/runtime/") && published_model_name.size()<120,
        "Preview requires an origin-owned published native model");
    const bool bind_gear=!gear_model_name.empty() || !gear_physics_name.empty();
    if(bind_gear)Check(gear_model_name.starts_with("em1/runtime/") && gear_model_name.ends_with(".nif") && gear_model_name.size()<120 &&
      gear_physics_name.starts_with("em1/runtime/") && gear_physics_name.ends_with(".hkx") && gear_physics_name.size()<120,
      "Gear requires both origin-owned native resource names");
    auto pak=PakSource::Open(source,"packfiles/dbc_1st_zonec.pak",error);if(!pak)return false;
    Bytes original;
    constexpr auto scene_path="environments/thepark/darkbeautycastle/gsa/dbc_1st_zonec.bin";
    if(!pak->ReadOriginal(scene_path,original,error))return false;
    SceneAdapterResult validated;if(!AdaptScene(original,validated,error))return false;
    const auto end=Word(original,0),count=Word(original,end+16),refs=Word(original,end+20);
    const auto text=[&](uint32_t p){
      Check(p>=4 && p+2<end,"Invalid original scene string");
      const auto last=std::find(original.begin()+p+2,original.begin()+end,uint8_t(0));
      Check(last!=original.begin()+end,"Unterminated original scene string");
      return std::string(original.begin()+p+2,last);
    };
    size_t at=end+24;std::map<uint32_t,Entity> entities;
    for(uint32_t i=0;i<count;++i) {
      Entity entity{Word(original,at+4),Word(original,at+8),Word(original,at+12),text(Word(original,at)),{}};
      entity.name_offset=Word(original,at);
      const auto components=Word(original,at+16);at+=20;
      for(uint32_t j=0;j<components;++j) {
        Component c{Word(original,at),Word(original,at+4),Word(original,at+8),Word(original,at+12),text(Word(original,at)),{}, {}};
        c.lineage.insert(c.id);const auto props=Word(original,at+16);at+=20;
        for(uint32_t k=0;k<props;++k) {
          const auto begin=at;Property p{text(Word(original,at)),text(Word(original,at+4)),{}};
          const auto amount=(Word(original,at+8)&1)?Word(original,at+12):1;
          at+=16+size_t(amount)*Stride(p.type);
          Check(at<=original.size(),"Truncated original scene property");
          p.record.assign(original.begin()+begin,original.begin()+at);c.properties.push_back(std::move(p));
        }
        entity.components.push_back(std::move(c));
      }
      Check(entities.emplace(entity.id,std::move(entity)).second,"Duplicate original entity identifier");
    }
    std::vector<uint32_t> placed;std::set<uint32_t> placed_ids;
    for(uint32_t i=0;i<refs;++i){const auto id=Word(original,at);Check(placed_ids.insert(id).second,"Duplicate placed entity");placed.push_back(id);at+=4;}
    Check(at==original.size(),"Invalid original placed-entity list");
    // Resolve every selected definition against its complete master chain;
    // entity templates are not assumed to be world placements.
    auto effective=[&](uint32_t id,uint32_t& depth){
      std::vector<const Entity*> chain;std::set<uint32_t> seen;
      for(auto next=id;next;) {
        Check(chain.size()<256 && seen.insert(next).second,"Cyclic original entity inheritance");
        auto found=entities.find(next);Check(found!=entities.end(),"Missing original entity ancestor");
        chain.push_back(&found->second);next=found->second.master;
      }
      depth=uint32_t(chain.size());std::vector<Component> merged;
      for(auto owner=chain.rbegin();owner!=chain.rend();++owner)for(auto c:(*owner)->components) {
        auto previous=std::find_if(merged.begin(),merged.end(),[&](const auto& p){return p.lineage.contains(c.id)||(c.master && p.lineage.contains(c.master));});
        if(previous==merged.end()){merged.push_back(std::move(c));continue;}
        Check(previous->name==c.name,"Component inheritance changes native class");
        auto properties=previous->properties;
        for(const auto& p:c.properties) {
          auto inherited=std::find_if(properties.begin(),properties.end(),[&](const auto& old){return old.name==p.name;});
          if(inherited==properties.end())properties.push_back(p);else *inherited=p;
        }
        c.lineage.insert(previous->lineage.begin(),previous->lineage.end());c.properties=std::move(properties);*previous=std::move(c);
      }
      return merged;
    };
    constexpr auto model_path="environments/thepark/darkbeautycastle/zones/dbc_scaletest2.nif";
    std::vector<Component> selected;uint32_t selected_id=0,depth=0;
    std::vector<Component> particle_components;
    std::vector<Component> trigger_components;uint32_t trigger_id=0;
    std::vector<Component> gear_components;uint32_t gear_id=0;
    std::vector<AuthoredRuntimeLight> runtime_lights;
    std::vector<ArchitectureScene::EventLink> event_links;
    std::vector<ArchitectureScene::GroupMember> group_members;
    for(auto id:placed) {
      uint32_t d=0;auto components=effective(id,d);
      if(id==particle_entity)particle_components=components;
      auto component=[&](std::string_view name)->const Component* {
        const Component* result=nullptr;
        for(const auto& c:components)if(c.name==name){Check(!result,"Ambiguous original component");result=&c;}
        return result;
      };
      if(const auto io=component("InputOutputComponent")) {
        auto array=[&](std::string_view name,std::string_view type,bool optional=false)->const Property* {
          const Property* found=nullptr;
          for(const auto& p:io->properties)if(p.name==name){Check(!found,"Duplicate event connection property");found=&p;}
          if(!found && optional)return nullptr;
          Check(found && found->type==type && Word(found->record,8)==1,"Unsupported event connection array");
          Check(found->record.size()==16+size_t(Word(found->record,12))*4,"Invalid event connection array extent");return found;
        };
        if(const auto groups=array("EntityGroups","String",true)){
          for(uint32_t n=0;n<Word(groups->record,12);++n)
            group_members.push_back({id,entities.at(id).name,text(Word(groups->record,16+size_t(n)*4))});
        }
        const auto inputs=array("Input","String");const auto count=Word(inputs->record,12);
        if(count) {
          const auto outputs=array("Output","String"),targets=array("Target","String"),parameters=array("Parameters","String");
          const auto delays=array("DelayWait","Float"),flags=array("IoFlags","Unsigned Short"),groups=array("TargetGroup","String",true);
          for(const auto* p:{outputs,targets,parameters,delays,flags})Check(Word(p->record,12)==count,"Mismatched parallel event arrays");
          Check(!groups || Word(groups->record,12)==0 || Word(groups->record,12)==count,"Mismatched event target groups");
          for(uint32_t n=0;n<count;++n) {
            const size_t offset=16+size_t(n)*4;ArchitectureScene::EventLink link;
            link.source_entity=id;link.source_name=entities.at(id).name;
            link.input=text(Word(inputs->record,offset));link.output=text(Word(outputs->record,offset));
            link.target=text(Word(targets->record,offset));link.parameters=text(Word(parameters->record,offset));
            if(groups && Word(groups->record,12))link.target_group=text(Word(groups->record,offset));
            link.delay=std::bit_cast<float>(Word(delays->record,offset));Check(std::isfinite(link.delay) && link.delay>=0,"Invalid event delay");
            link.flags=uint16_t(Word(flags->record,offset)>>16);
            event_links.push_back(std::move(link));
          }
        }
      }
      const auto baking=component("JPSLightBakingComponent");
      if(baking) {
        auto property=[&](const Component& c,std::string_view name,std::string_view type,size_t size)->const Property& {
          const Property* result=nullptr;
          for(const auto& p:c.properties)if(p.name==name){Check(!result,"Duplicate runtime light property");result=&p;}
          Check(result && result->type==type && result->record.size()==16+size && !(Word(result->record,8)&1),"Missing/invalid runtime light property");
          return *result;
        };
        auto flag=[&](std::string_view name){return Word(property(*baking,name,"Boolean",4).record,16)!=0;};
        if(flag("GenerateRuntimeLight")) {
          const auto light=component("JPSLightComponent"),transform=component("JPSTransformationComponent");
          Check(light && transform,"Authored runtime light lacks a light/transform component");
          auto scalar=[&](const Component& c,std::string_view name){const auto f=std::bit_cast<float>(Word(property(c,name,"Float",4).record,16));Check(std::isfinite(f),"Nonfinite runtime light scalar");return f;};
          auto vector=[&](const Component& c,std::string_view name,std::string_view type){
            const auto& p=property(c,name,type,12);std::array<float,3> result{};
            for(unsigned j=0;j<3;++j){result[j]=std::bit_cast<float>(Word(p.record,16+j*4));Check(std::isfinite(result[j]),"Nonfinite runtime light vector");}return result;
          };
          Check(Word(property(*transform,"Parent Entity","Entity Pointer",4).record,16)==0 &&
                Word(property(*transform,"Source Entity","Entity Pointer",4).record,16)==0,"Runtime light transform requires parent/source resolution");
          AuthoredRuntimeLight value;value.entity=id;value.name=entities.at(id).name;
          value.position=vector(*transform,"Translation","Point3");
          const auto& rotation=property(*transform,"Rotation","Matrix3",36);
          for(unsigned j=0;j<9;++j)Check(std::isfinite(std::bit_cast<float>(Word(rotation.record,16+j*4))),"Nonfinite light rotation");
          for(unsigned j=0;j<3;++j)value.direction[j]=std::bit_cast<float>(Word(rotation.record,16+(j*3+2)*4));
          const auto type=text(Word(property(*light,"Light Type","String",4).record,16));
          if(type=="Ambient")value.kind=AuthoredRuntimeLight::Kind::Ambient;
          else if(type=="Directional")value.kind=AuthoredRuntimeLight::Kind::Directional;
          else if(type=="Point")value.kind=AuthoredRuntimeLight::Kind::Point;
          else {Check(type=="Spot","Unsupported authored light type");value.kind=AuthoredRuntimeLight::Kind::Spot;}
          value.ambient=vector(*light,"Color (Ambient)","Color (RGB)");value.diffuse=vector(*light,"Color (Diffuse)","Color (RGB)");
          value.attenuation={scalar(*light,"Attenuation (Constant)"),scalar(*light,"Attenuation (Linear)"),scalar(*light,"Attenuation (Quadratic)")};
          value.dimmer=scalar(*light,"Dimmer");value.range_start=scalar(*baking,"Min Falloff Range");value.range_end=scalar(*baking,"Max Falloff Range");
          value.inner_degrees=scalar(*light,"Spot Angle (Inner)");value.outer_degrees=scalar(*light,"Spot Angle (Outer)");value.spot_exponent=scalar(*light,"Spot Exponent");
          value.subtractive=flag("LightIsSubtractive");value.raycast=flag("RuntimeLightRayCast");
          runtime_lights.push_back(std::move(value));
        }
      }
      if(entities.at(id).name=="dbc_scaletest.TriggerRotate 03"){
        Check(!trigger_id,"Duplicate authored gear trigger");trigger_id=id;trigger_components=components;
      }
      if(entities.at(id).name==(gear_number==1?"dbc_scaletest.NOS_HM_ZoneM_Gear01 01":"dbc_scaletest.NOS_HM_ZoneM_Gear01 02")){
        Check(!gear_id,"Duplicate authored gear entity");gear_id=id;gear_components=components;
      }
      bool matches=false;
      for(const auto& c:components)if(c.name=="JPSSceneGraphComponent")for(const auto& p:c.properties)
        if(p.name=="NIF File Path" && p.type=="String" && p.record.size()==20)
          matches=Lower(text(Word(p.record,16)))==model_path;
      if(matches){Check(!selected_id,"Ambiguous courtyard architecture placement");selected_id=id;selected=std::move(components);depth=d;}
    }
    Check(selected_id!=0,"Original courtyard architecture is not placed");
    std::erase_if(selected,[](const auto& c){return c.name!="JPSTransformationComponent" && c.name!="JPSSceneGraphComponent";});
    Check(selected.size()==2,"Courtyard architecture lacks unique transform/scenegraph components");
    // This first architecture entity is authored at identity with no parent.
    // Refuse a changed source instead of accidentally applying hierarchy twice.
    bool translation=false,rotation=false,scale=false;
    for(const auto& c:selected)if(c.name=="JPSTransformationComponent")for(const auto& p:c.properties) {
      if(p.name=="Parent Entity" || p.name=="Source Entity")Check(Word(p.record,16)==0,"Courtyard architecture now has a transform source/parent");
      if(p.name=="Translation" || p.name=="Local Translation") {
        Check(p.record.size()==28,"Invalid courtyard translation");for(size_t n=16;n<28;n+=4)Check(std::bit_cast<float>(Word(p.record,n))==0,"Courtyard architecture is not at its authored identity");translation=true;
      }
      if(p.name=="Rotation" || p.name=="Local Rotation") {
        Check(p.record.size()==52,"Invalid courtyard rotation");for(size_t n=0;n<9;++n)Check(std::bit_cast<float>(Word(p.record,16+n*4))==(n%4==0?1.f:0.f),"Courtyard architecture rotation changed");rotation=true;
      }
      if(p.name=="Scale" || p.name=="Local Scale"){Check(std::bit_cast<float>(Word(p.record,16))==1,"Courtyard architecture scale changed");scale=true;}
    }
    Check(translation && rotation && scale,"Courtyard transform is incomplete");

    Bytes projected(original.begin(),original.begin()+end);
    auto add_string=[&](std::string_view value){
      const auto offset=uint32_t(projected.size());const auto length=value.size()+1,bytes=(length+5)&~size_t(3);
      Check(bytes<=255,"Preview JPS string too long");projected.push_back(uint8_t(bytes));projected.push_back(uint8_t(length));
      projected.insert(projected.end(),value.begin(),value.end());projected.resize(offset+bytes,0);return offset;
    };
    ArchitectureScene prepared;prepared.entity_name="EM1_CourtyardPreview_"+std::string(published_model_name.substr(12));
    const auto name=add_string(prepared.entity_name),path=add_string(published_model_name);
    Set(projected,0,uint32_t(projected.size()));
    projected.insert(projected.end(),original.begin()+end,original.begin()+end+16);
    Append(projected,1);Append(projected,1);
    Append(projected,name);Append(projected,selected_id);Append(projected,0);Append(projected,entities.at(selected_id).flags);Append(projected,2);
    for(auto& c:selected) {
      Append(projected,c.klass);Append(projected,c.version);Append(projected,c.id);Append(projected,0);Append(projected,uint32_t(c.properties.size()));
      for(auto& p:c.properties) {
        if(c.name=="JPSSceneGraphComponent" && p.name=="NIF File Path")Set(p.record,16,path);
        projected.insert(projected.end(),p.record.begin(),p.record.end());
      }
    }
    Append(projected,selected_id);
    if(!AdaptScene(projected,prepared.scene,error))return false;
    Check(trigger_id && trigger_components.size()==4,"Authored gear trigger component set changed");
    Bytes trigger(original.begin(),original.begin()+end);
    trigger.insert(trigger.end(),original.begin()+end,original.begin()+end+16);
    Append(trigger,1);Append(trigger,1);
    // Original name string pointer is retained from the validated source table.
    const auto trigger_name=entities.at(trigger_id).name_offset;
    Append(trigger,trigger_name);Append(trigger,trigger_id);Append(trigger,0);Append(trigger,entities.at(trigger_id).flags);Append(trigger,uint32_t(trigger_components.size()));
    for(const auto& c:trigger_components){
      Check(c.name=="JPSTransformationComponent" || c.name=="PhysicsRigidBodyPhantomShapeComponent" || c.name=="TriggerVolumeComponent" || c.name=="InputOutputComponent","Unexpected trigger component");
      Append(trigger,c.klass);Append(trigger,c.version);Append(trigger,c.id);Append(trigger,0);Append(trigger,uint32_t(c.properties.size()));
      for(const auto& p:c.properties)trigger.insert(trigger.end(),p.record.begin(),p.record.end());
    }
    Append(trigger,trigger_id);
    if(!AdaptScene(trigger,prepared.trigger_prototype,error))return false;
    Check(gear_id && gear_components.size()==8,"Authored gear component set changed");
    Bytes gear(original.begin(),original.begin()+end);
    auto gear_string=[&](std::string_view text){
      const auto at=uint32_t(gear.size()),length=uint32_t(text.size()+1),size=(length+5)&~3u;
      Check(size<=255,"Gear resource name too long");gear.push_back(uint8_t(size));gear.push_back(uint8_t(length));
      gear.insert(gear.end(),text.begin(),text.end());gear.resize(at+size,0);return at;
    };
    const auto gear_model=bind_gear?gear_string(gear_model_name):0,gear_physics=bind_gear?gear_string(gear_physics_name):0;
    Set(gear,0,uint32_t(gear.size()));
    gear.insert(gear.end(),original.begin()+end,original.begin()+end+16);
    Append(gear,1);Append(gear,1);
    Append(gear,entities.at(gear_id).name_offset);Append(gear,gear_id);Append(gear,0);
    Append(gear,entities.at(gear_id).flags);Append(gear,uint32_t(gear_components.size()));
    std::set<std::string> gear_classes;
    unsigned gear_bindings=0;
    for(auto c:gear_components){
      Check(gear_classes.insert(c.name).second,"Duplicate authored gear component class");
      Append(gear,c.klass);Append(gear,c.version);Append(gear,c.id);Append(gear,0);Append(gear,uint32_t(c.properties.size()));
      for(auto p:c.properties){
        const auto target=(c.name=="JPSSceneGraphComponent" && p.name=="NIF File Path")?gear_model:
          (c.name=="PhysicsRigidBodyDynamicComponent" && p.name=="RB Hull File Path")?gear_physics:0;
        if(target){Check(p.type=="String" && p.record.size()==20 && !(Word(p.record,8)&1),"Gear asset path is not a scalar string");Set(p.record,16,target);++gear_bindings;}
        gear.insert(gear.end(),p.record.begin(),p.record.end());
      }
    }
    Check(!bind_gear || gear_bindings==2,"Gear native resource binding incomplete");
    Check(gear_classes==std::set<std::string>{"JPSTransformationComponent","JPSSceneGraphComponent","InputOutputComponent",
      "PhysicsRigidBodyDynamicComponent","RotatorComponent","VulnerabilityComponent","HealthComponent","GameEventsComponent"},"Unexpected authored gear classes");
    Append(gear,gear_id);
    if(!AdaptScene(gear,prepared.gear_prototype,error))return false;
    if(particle_entity){
      Check(particle_components.size()==4,"Particle projection requires a placed four-component actor");
      std::set<std::string> classes;
      Bytes particle(original.begin(),original.begin()+end);
      const auto model_offset=uint32_t(particle.size());
      const auto length=published_model_name.size()+1,size=(length+5)&~size_t(3);
      Check(size<=255,"Particle resource name too long");
      particle.push_back(uint8_t(size));particle.push_back(uint8_t(length));
      particle.insert(particle.end(),published_model_name.begin(),published_model_name.end());particle.resize(model_offset+size,0);
      Set(particle,0,uint32_t(particle.size()));
      particle.insert(particle.end(),original.begin()+end,original.begin()+end+16);
      Append(particle,1);Append(particle,1);
      Append(particle,entities.at(particle_entity).name_offset);Append(particle,particle_entity);Append(particle,0);
      Append(particle,entities.at(particle_entity).flags);Append(particle,4);
      unsigned bindings=0;
      for(auto c:particle_components){
        Check(classes.insert(c.name).second,"Duplicate particle component class");
        Append(particle,c.klass);Append(particle,c.version);Append(particle,c.id);Append(particle,0);Append(particle,uint32_t(c.properties.size()));
        for(auto p:c.properties){
          if(c.name=="JPSSceneGraphComponent" && p.name=="NIF File Path"){
            Check(p.type=="String" && p.record.size()==20 && !(Word(p.record,8)&1),"Invalid particle NIF path");
            const auto original_path=Lower(text(Word(p.record,16)));
            Check(original_path=="effects/_shared/common_steam_02.nif" || original_path=="effects/_shared/steam_jet_01.nif","Unsupported particle source asset");
            Set(p.record,16,model_offset);++bindings;
          }
          particle.insert(particle.end(),p.record.begin(),p.record.end());
        }
      }
      Check(bindings==1 && classes==std::set<std::string>{"JPSTransformationComponent","JPSSceneGraphComponent","InputOutputComponent","ParticleSystemControllerComponent"},"Unexpected particle component set");
      Append(particle,particle_entity);
      if(!AdaptScene(particle,prepared.particle_prototype,error))return false;
      prepared.particle_source_entity_id=particle_entity;
    }
    // EM2 TriggerVolume startup (8291C6C8) resolves PhysicsPhantomComponent,
    // not the legacy rigid-body phantom class that still accepts EM1 records.
    // Keep the untouched blueprint above; adapt this known empty-hull box only.
    Bytes native_trigger(original.begin(),original.begin()+end);
    const auto native_class=uint32_t(native_trigger.size());
    constexpr std::string_view phantom_class="PhysicsPhantomComponent";
    const auto class_bytes=(phantom_class.size()+6)&~size_t(3);
    native_trigger.push_back(uint8_t(class_bytes));native_trigger.push_back(uint8_t(phantom_class.size()+1));
    native_trigger.insert(native_trigger.end(),phantom_class.begin(),phantom_class.end());native_trigger.resize(native_class+class_bytes,0);
    Set(native_trigger,0,uint32_t(native_trigger.size()));
    native_trigger.insert(native_trigger.end(),original.begin()+end,original.begin()+end+16);
    Append(native_trigger,1);Append(native_trigger,1);
    Append(native_trigger,trigger_name);Append(native_trigger,trigger_id);Append(native_trigger,0);Append(native_trigger,entities.at(trigger_id).flags);Append(native_trigger,4);
    for(auto c:trigger_components){
      if(c.name=="PhysicsRigidBodyPhantomShapeComponent"){
        bool empty_hull=false;
        for(const auto& p:c.properties)if(p.name=="RB Hull File Path" && p.type=="String" && p.record.size()==20)empty_hull=text(Word(p.record,16)).empty();
        Check(empty_hull,"Native trigger adaptation requires the original empty hull");
        c.klass=native_class;
        std::erase_if(c.properties,[](const auto& p){return p.name!="Phantom Size" && p.name!="Collision Layer" && p.name!="Allow Look Through";});
        Check(c.properties.size()==3,"Native trigger box lacks its authored properties");
      }
      Append(native_trigger,c.klass);Append(native_trigger,c.version);Append(native_trigger,c.id);Append(native_trigger,0);Append(native_trigger,uint32_t(c.properties.size()));
      for(const auto& p:c.properties)native_trigger.insert(native_trigger.end(),p.record.begin(),p.record.end());
    }
    Append(native_trigger,trigger_id);
    if(!AdaptScene(native_trigger,prepared.trigger_native_prototype,error))return false;
    prepared.origin_key=pak->OriginKey()+":"+scene_path+":architecture-preview1";
    prepared.model_path=model_path;prepared.original_definitions=count;prepared.original_placements=refs;prepared.resolved_ancestors=depth;
    prepared.source_entity_id=selected_id;
    prepared.gear_source_entity_id=gear_id;prepared.gear_source_entity_name=entities.at(gear_id).name;
    prepared.runtime_lights=std::move(runtime_lights);
    prepared.event_links=std::move(event_links);
    uint64_t fingerprint=14695981039346656037ull;
    auto byte=[&](uint8_t value){fingerprint=(fingerprint^value)*1099511628211ull;};
    auto word=[&](uint32_t value){for(int shift=24;shift>=0;shift-=8)byte(uint8_t(value>>shift));};
    for(const auto& link:prepared.event_links){
      word(link.source_entity);
      for(const auto* value:{&link.source_name,&link.output,&link.input,&link.target,&link.parameters,&link.target_group}){
        for(unsigned char c:*value)byte(c);byte(0);
      }
      word(std::bit_cast<uint32_t>(link.delay));byte(uint8_t(link.flags>>8));byte(uint8_t(link.flags));
    }
    prepared.event_link_fingerprint=fingerprint;
    prepared.group_members=std::move(group_members);fingerprint=14695981039346656037ull;
    for(const auto& member:prepared.group_members){
      word(member.source_entity);
      for(const auto* value:{&member.source_name,&member.group}){for(unsigned char c:*value)byte(c);byte(0);}
    }
    prepared.group_fingerprint=fingerprint;
    output=std::move(prepared);error.clear();return true;
  }catch(const std::exception& e){error=e.what();return false;}
}
}
