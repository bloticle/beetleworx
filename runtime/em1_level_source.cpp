#include "em1_level_source.h"
#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace em2::em1 {
namespace {
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
std::string Lower(std::string text) {
  for (char& c : text) {
    if (c == '\\') c = '/';
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  }
  return text;
}
std::string Trim(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r");
  if (first == std::string_view::npos) return {};
  return std::string(text.substr(first, text.find_last_not_of(" \t\r") - first + 1));
}
void Name(std::string_view name) {
  Check(!name.empty() && name.size() <= 255 && name != "." && name != "..",
        "Invalid EM1 level/partition name");
  for (unsigned char c : name)
    Check(c >= 32 && c != 127 && c != ':' && c != '/' && c != '\\',
          "Invalid EM1 level/partition name character");
}
std::string PartitionName(std::string value) {
  Name(value);
  value = Lower(std::move(value));
  if (value.ends_with(".part")) value.resize(value.size() - 5);
  Name(value);
  return value;
}
std::vector<std::string> Lines(PakSource& archive, const std::string& path) {
  std::vector<uint8_t> bytes;
  std::string error;
  // Bound before decompression, including malicious directory lengths.
  const auto& entries = archive.Entries();
  const auto entry = std::find_if(entries.begin(), entries.end(),
      [&](const PakEntry& item) { return item.path == Lower(path); });
  Check(entry != entries.end() && entry->size <= 1024 * 1024,
        "Missing or oversized EM1 level descriptor");
  if (!archive.ReadOriginal(path, bytes, error)) throw std::runtime_error(error);
  Check(std::find(bytes.begin(), bytes.end(), 0) == bytes.end(),
        "EM1 level descriptor contains a null byte");
  const std::string_view text(bytes.empty() ? "" :
      reinterpret_cast<const char*>(bytes.data()), bytes.size());
  std::vector<std::string> result;
  for (size_t start = 0; start < text.size();) {
    const auto newline = text.find('\n', start);
    const auto end = newline == std::string_view::npos ? text.size() : newline;
    Check(end - start <= 4096 && result.size() < 16384,
          "EM1 level descriptor exceeds line bounds");
    result.push_back(Trim(text.substr(start, end - start)));
    start = end + 1;
  }
  return result;
}
LevelScene Scene(std::string value) {
  LevelScene result;
  const auto flag = value.find(" -");
  if (flag != std::string::npos) {
    result.authored_flag = Trim(std::string_view(value).substr(flag + 1));
    value.resize(flag);
  }
  result.source_gsa = Lower(Trim(value));
  Check(result.source_gsa.ends_with(".gsa") && result.source_gsa.size() <= 2048,
        "Expected an original EM1 GSA scene reference");
  for (size_t start = 0; start < result.source_gsa.size();) {
    const auto end = result.source_gsa.find('/', start);
    Name(std::string_view(result.source_gsa).substr(start,
        end == std::string::npos ? result.source_gsa.size() - start : end - start));
    start = end == std::string::npos ? result.source_gsa.size() : end + 1;
  }
  const auto slash = result.source_gsa.find_last_of('/');
  const auto basename = result.source_gsa.substr(slash == std::string::npos ? 0 : slash + 1);
  Check(basename.size() > 4, "Empty EM1 scene basename");
  result.archive = "packfiles/" + basename.substr(0, basename.size() - 4) + ".pak";
  result.binary_scene = result.source_gsa.substr(0, result.source_gsa.size() - 4) + ".bin";
  return result;
}
void PartitionResource(const std::string& line,LevelPartition& partition) {
  const auto flag_at=line.find(" -");
  const auto authored_path=Trim(std::string_view(line).substr(0,flag_at));
  const auto path=Lower(authored_path);
  const auto flag=flag_at==std::string::npos?std::string{}:Trim(std::string_view(line).substr(flag_at+1));
  if(path.ends_with(".gsa")) {
    partition.resources.push_back({LevelPartitionResource::Kind::Scene,partition.scenes.size(),flag});
    partition.scenes.push_back(Scene(line));
  } else if(path.ends_with(".bnk")) {
    Name(path);
    partition.resources.push_back({LevelPartitionResource::Kind::Audio,partition.audio.size(),flag});
    partition.audio.push_back(path);
  } else {
    // Wii REL2948B8..2948C0 skips a resource whose type remains -1.
    // Bare Prefab_Spawner_Pottery occurs in original Mean Street partitions.
    // Preserve its record; never reinterpret it as a scene or soundbank.
    partition.resources.push_back({LevelPartitionResource::Kind::Ignored,partition.ignored.size(),flag});
    partition.ignored.push_back(authored_path);
  }
}
}  // namespace

bool TestLevelPartitionClassification(std::string& error) {
  try {
    LevelPartition part;
    for(const std::string line:{"Environments/Test/World.gsa -inactive","Prefab_Spawner_Pottery",
        "Music.bnk -resident","Unknown_Format.thing -i","Environments/Test/Next.GSA"})
      PartitionResource(line,part);
    Check(part.scenes.size()==2 && part.audio==std::vector<std::string>{"music.bnk"} &&
          part.ignored==std::vector<std::string>{"Prefab_Spawner_Pottery","Unknown_Format.thing"},
          "Partition classification changed source tokens or resource types");
    Check(part.resources.size()==5 && part.resources[0].kind==LevelPartitionResource::Kind::Scene &&
          part.resources[0].authored_flag=="-inactive" && part.resources[1].kind==LevelPartitionResource::Kind::Ignored &&
          part.resources[1].index==0 && part.resources[2].kind==LevelPartitionResource::Kind::Audio &&
          part.resources[2].authored_flag=="-resident" && part.resources[3].kind==LevelPartitionResource::Kind::Ignored &&
          part.resources[3].index==1 && part.resources[3].authored_flag=="-i" && part.resources[4].index==1,
          "Partition classification lost mixed resource ordering/flags/indices");
    bool rejected=false;
    try{PartitionResource("Environments/../Invalid.gsa",part);}
    catch(const std::runtime_error&){rejected=true;}
    Check(rejected,"Unsafe classified scene was accepted");
    error.clear();return true;
  }catch(const std::exception& failure){error=failure.what();return false;}
}

bool ListLevelDescriptors(std::shared_ptr<RvzSource> source,
                          std::vector<std::string>& output,std::string& error) {
  try {
    Check(bool(source),"EM1 source is not mounted");
    auto archive=PakSource::Open(source,"packfiles/levels.pak",error);
    if(!archive)return false;
    std::vector<std::string> names;
    for(const auto& entry:archive->Entries()) {
      if(!entry.path.starts_with("levels/") || !entry.path.ends_with(".level"))continue;
      auto name=entry.path.substr(7);Name(name);
      Check(names.size()<4096,"Too many EM1 level descriptors");
      names.push_back(std::move(name));
    }
    std::sort(names.begin(),names.end());
    Check(std::adjacent_find(names.begin(),names.end())==names.end(),"Duplicate EM1 level descriptor");
    output=std::move(names);error.clear();return true;
  } catch(const std::exception& exception){error=exception.what();return false;}
}

bool ReadLevelContents(std::shared_ptr<RvzSource> source,std::string_view level,
                       LevelContents& output,std::string& error) {
  try {
    Check(bool(source),"EM1 source is not mounted");
    LevelContents prepared;
    if(!ReadLevelPlan(source,level,prepared.plan,error))return false;
    std::vector<LevelScene> requests=prepared.plan.shared_scenes;
    for(const auto& entry:prepared.plan.entry_partitions) {
      const auto part=std::find_if(prepared.plan.partitions.begin(),prepared.plan.partitions.end(),
        [&](const LevelPartition& item){return item.name==entry.name;});
      Check(part!=prepared.plan.partitions.end(),"EM1 entry partition is absent");
      requests.insert(requests.end(),part->scenes.begin(),part->scenes.end());
    }
    Check(!requests.empty() && requests.size()<=1024,"Invalid EM1 entry scene count");
    size_t bytes=0;
    std::unordered_set<std::string> visited;
    for(const auto& request:requests) {
      if(!visited.insert(request.archive+":"+request.binary_scene).second)continue;
      LevelSceneResource scene;scene.request=request;
      if(!ReadAdaptedScene(source,request,scene.resource,error)) {
        error=request.binary_scene+": "+error;return false;
      }
      Check(scene.resource.scene.bytes.size()<=256ull*1024*1024-bytes,"EM1 entry scenes exceed memory budget");
      bytes+=scene.resource.scene.bytes.size();
      prepared.entry_scenes.push_back(std::move(scene));
    }
    output=std::move(prepared);error.clear();return true;
  } catch(const std::exception& exception){error=exception.what();return false;}
}

bool ReadLevelPlan(std::shared_ptr<RvzSource> source, std::string_view level,
                   LevelPlan& output, std::string& error) {
  try {
    Name(level);
    auto archive = PakSource::Open(std::move(source), "packfiles/levels.pak", error);
    if (!archive) return false;
    LevelPlan result;
    result.descriptor = "levels/" + Lower(std::string(level));
    if (!result.descriptor.ends_with(".level")) result.descriptor += ".level";
    result.origin_key = archive->OriginKey() + ":level-v1:" + result.descriptor;
    std::string folder;

    std::unordered_set<std::string> fields;
    for (const auto& line : Lines(*archive, result.descriptor)) {
      if (line.empty() || line.starts_with("//")) continue;
      const auto equals = line.find('=');
      Check(equals != std::string::npos, "Invalid EM1 level field");
      auto key = Lower(Trim(std::string_view(line).substr(0, equals)));
      auto value = Trim(std::string_view(line).substr(equals + 1));
      Check(!key.empty() && fields.insert(key).second, "Empty or duplicate EM1 level field");
      if (key == "folder") { Name(value); folder = Lower(value); }
      else if (key == "initial" || key == "interior") {
        result.entry_partitions.push_back({PartitionName(value),key=="interior"?LevelEntryPartition::Mode::Interior:LevelEntryPartition::Mode::Initial});
      } else if (key == "shared") result.shared_scenes.push_back(Scene(value));
      result.fields.emplace_back(std::move(key), std::move(value));
    }
    std::stable_partition(result.entry_partitions.begin(),result.entry_partitions.end(),
      [](const auto& entry){return entry.mode==LevelEntryPartition::Mode::Initial;});
    Check(!folder.empty() && (!result.entry_partitions.empty() || !result.shared_scenes.empty()),
          "EM1 level has no folder or entry scene");
    const std::string prefix = "levels/" + folder + '/';
    for (const auto& entry : archive->Entries()) {
      if (!entry.path.starts_with(prefix) || !entry.path.ends_with(".part")) continue;
      Check(result.partitions.size() < 1024, "Too many EM1 level partitions");
      LevelPartition partition;
      partition.name = entry.path.substr(prefix.size(), entry.path.size() - prefix.size() - 5);
      Name(partition.name);
      // The original parser classifies each resource by extension. Comment
      // headings describe the file but do not change resource interpretation.
      for (const auto& line : Lines(*archive, entry.path)) {
        if (line.empty()) continue;
        if (line.starts_with("//")) {
          if (Lower(Trim(std::string_view(line).substr(2))) == "soundbanks list") {
            continue;
          }
          const auto colon = line.find(':');
          if (colon == std::string::npos) continue; // Authored disabled entries.
          const auto tag = Lower(Trim(std::string_view(line).substr(2, colon - 2)));
          const auto value = Trim(std::string_view(line).substr(colon + 1));
          if (tag == "gsas" || tag == "audio") {
            if (!value.empty()) partition.section_notes.emplace_back(tag, value);
          }
          else if (tag == "interior" || tag == "destination") {
            (tag == "interior" ? partition.interiors : partition.destinations)
                .push_back(PartitionName(value));
          } else partition.section_notes.emplace_back(tag,value);
          continue;
        }
        PartitionResource(line,partition);
      }
      result.partitions.push_back(std::move(partition));
    }
    for (const auto& initial : result.entry_partitions)
      Check(std::any_of(result.partitions.begin(), result.partitions.end(),
              [&](const LevelPartition& part) { return part.name == initial.name; }),
            "EM1 level entry partition is absent");
    output = std::move(result);
    error.clear();
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}

bool ReadAdaptedScene(std::shared_ptr<RvzSource> source, const LevelScene& request,
                      AdaptedSceneResource& output, std::string& error) {
  try {
    auto archive = PakSource::Open(std::move(source), request.archive, error);
    if (!archive) return false;
    std::vector<uint8_t> original;
    if (!archive->ReadOriginal(request.binary_scene, original, error)) return false;
    AdaptedSceneResource result;
    if (!AdaptScene(original, result.scene, error)) return false;
    result.origin_key = archive->OriginKey() + ":scene-v" +
        std::to_string(kSceneAdapterVersion) + ':' + Lower(request.binary_scene);
    output = std::move(result);
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}
}  // namespace em2::em1
