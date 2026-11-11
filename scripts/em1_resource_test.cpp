#include "em1_level_source.h"
#include "em1_preview_scene.h"
#include "em1_spherical_lighting.h"
#include "em1_character_lighting.h"
#include <bit>
#include <iomanip>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void Check(bool value, const std::string& message) {
  if (!value) throw std::runtime_error(message);
}
std::string Hash(std::span<const uint8_t> bytes) {
  BCRYPT_ALG_HANDLE provider{};
  Check(BCryptOpenAlgorithmProvider(&provider, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0,
        "SHA256 initialization");
  std::array<uint8_t, 32> digest{};
  const auto status = BCryptHash(provider, nullptr, 0, const_cast<PUCHAR>(bytes.data()),
                                ULONG(bytes.size()), digest.data(), ULONG(digest.size()));
  BCryptCloseAlgorithmProvider(provider, 0);
  Check(status >= 0, "SHA256 failed");
  const char* hex = "0123456789abcdef";
  std::string output;
  for (const auto byte : digest) { output += hex[byte >> 4]; output += hex[byte & 15]; }
  return output;
}
}
int wmain(int argc, wchar_t** argv) {
  try {
    using namespace em2::em1;
    if(argc==2 && std::wstring_view(argv[1])==L"--character-lighting-check") {
      Check(!CharacterLightingRegister({}),"Empty shader acquired lighting binding");
      unsigned mutations=0;
      for(const auto& binding:character_lighting_data::bindings) {
        Check(CharacterLightingRegister(binding.code)==binding.first,"Native SH program mapped to wrong registers");
        Check(!CharacterLightingRegister(binding.code.first(binding.code.size()-1)),"Truncated shader acquired lighting binding");
        std::vector<uint32_t> changed(binding.code.begin(),binding.code.end());
        for(size_t i=0;i<changed.size();++i) {
          changed[i]^=1;Check(!CharacterLightingRegister(changed),"Modified shader acquired native lighting binding");changed[i]^=1;++mutations;
        }
      }
      std::cout<<"PASS: all four native Mickey layouts; "<<mutations<<" shader mutations and truncated/empty inputs rejected\n";
      return 0;
    }
    if(argc==3 && std::wstring_view(argv[1])==L"--sh-check") {
      std::ifstream input{std::filesystem::path(argv[2])};Check(bool(input),"Missing independent SH fixtures");
      std::string line;unsigned vectors=0,projections=0;
      while(std::getline(input,line)) {
        std::istringstream row(line);char kind;row>>kind;LightVector vector{};
        for(auto& f:vector){uint32_t bits;Check(bool(row>>bits),"Truncated SH vector fixture");f=std::bit_cast<float>(bits);}
        if(kind=='V') {
          auto [length,direction]=lighting_detail::Distance(vector);uint32_t expected;
          Check(bool(row>>expected),"Missing vector length");
          const bool tiny=std::bit_cast<float>(expected)<=1e-6f;
          Check(std::bit_cast<uint32_t>(length)==(tiny?0:expected),"Original SDK length differs");
          for(float f:direction){Check(bool(row>>expected),"Missing normalized component");Check(std::bit_cast<uint32_t>(f)==(tiny?0:expected),"Original SDK normalization differs");}
          ++vectors;
        } else {
          Check(kind=='P',"Unknown SH fixture kind");
          for(float f:lighting_detail::Project(vector)){uint32_t bits;Check(bool(row>>bits),"Missing projection coefficient");const float expected=std::bit_cast<float>(bits);Check(std::abs(f-expected)<=4e-7f*std::max(1.f,std::abs(expected)),"Original PC SH projection differs beyond FMA rounding tolerance");}
          ++projections;
        }
      }
      Check(vectors>0 && projections>0,"Empty SH fixtures");
      const LightRotation camera{{{1,0,0},{0,1,0},{0,0,1}}};
      AuthoredRuntimeLight light;light.kind=AuthoredRuntimeLight::Kind::Ambient;light.position={0,0,10};light.dimmer=1;light.ambient={1,.5f,.25f};
      std::array lights{light};const auto ambient=EvaluateAuthoredLighting(lights,{},camera);
      for(unsigned c=0;c<3;++c)Check(std::abs(ambient[0][c]-light.ambient[c]*255*3.544907808303833f)<.0002f,"Ambient energy mismatch");
      for(unsigned b=1;b<9;++b)Check(ambient[b]==LightVector{},"Ambient light has directional terms");
      lights[0].subtractive=true;const auto negative=EvaluateAuthoredLighting(lights,{},camera);
      for(unsigned b=0;b<9;++b)for(unsigned c=0;c<3;++c)Check(std::abs(ambient[b][c]+negative[b][c])<.0002f,"Subtractive lighting lost its sign");
      lights[0]=light;lights[0].range_start=0;lights[0].range_end=5;
      Check(EvaluateAuthoredLighting(lights,{},camera)==LightCoefficients{},"Out-of-range light contributes");
      lights[0]=light;lights[0].raycast=true;bool rejected=false;
      try{EvaluateAuthoredLighting(lights,{},camera);}catch(const std::exception&){rejected=true;}
      Check(rejected,"Missing raycast visibility silently accepted");
      const std::array<float,1> hidden{0},half{.5f};
      Check(EvaluateAuthoredLighting(lights,{},camera,hidden)==LightCoefficients{},"Occluded light contributes");
      const auto faded=EvaluateAuthoredLighting(lights,{},camera,half);
      for(unsigned c=0;c<3;++c)Check(std::abs(faded[0][c]*2-ambient[0][c])<.0002f,"Visibility fade mismatch");
      light.kind=AuthoredRuntimeLight::Kind::Point;light.ambient={};light.diffuse={1,.5f,.25f};light.attenuation={1,0,0};lights[0]=light;
      const auto point=EvaluateAuthoredLighting(lights,{},camera);
      lights[0].range_start=5;lights[0].range_end=15;const auto falloff=EvaluateAuthoredLighting(lights,{},camera);
      for(unsigned b=0;b<9;++b)for(unsigned c=0;c<3;++c)Check(std::abs(falloff[b][c]*2-point[b][c])<.0002f,"Distance falloff mismatch");
      lights[0]=light;lights[0].kind=AuthoredRuntimeLight::Kind::Spot;lights[0].direction={0,0,-1};lights[0].inner_degrees=20;lights[0].outer_degrees=60;lights[0].spot_exponent=1;
      Check(EvaluateAuthoredLighting(lights,{},camera)==point,"Spotlight axis lost full contribution");
      lights[0].direction={1,0,0};Check(EvaluateAuthoredLighting(lights,{},camera)==LightCoefficients{},"Outside spotlight cone contributes");
      lights[0]=light;const LightRotation turned{{{0,0,1},{0,1,0},{-1,0,0}}};const auto rotated=EvaluateAuthoredLighting(lights,{},turned);
      for(unsigned c=0;c<3;++c)Check(std::abs(rotated[3][c]-point[2][c])<.0002f && rotated[0][c]==point[0][c],"Camera rotation changed irradiance energy or first-band orientation");
      const LightRotation world{{{-1,0,0},{0,1,0},{0,0,-1}}};
      const auto world_point=EvaluateAuthoredLighting(lights,{},world);
      for(unsigned c=0;c<3;++c)Check(std::abs(world_point[2][c]+point[2][c])<.0002f && world_point[0][c]==point[0][c],"World-normal binding did not cancel the view-space Z flip");
      Check(EvaluateAuthoredLighting(lights,{0,0,20},world)!=world_point,"SH sample failed to follow player position across a light");
      std::cout<<"PASS: "<<vectors<<" original SDK vector fixtures bit-exact; "<<projections<<" PC projection fixtures within FMA tolerance; ambient/subtractive/range/visibility checks\n";
      return 0;
    }
    if(argc==4 && std::wstring_view(argv[2])==L"--runtime-lights") {
      std::string error;std::shared_ptr<RvzSource> source=RvzSource::Open(argv[1],error);Check(bool(source),error);
      ArchitectureScene scene;
      Check(ReadCourtyardArchitectureScene(source,"em1/runtime/lighting-test.nif",scene,error),error);
      Check(scene.scene.entity_count==1 && scene.scene.component_count==2,"Lighting decode changed architecture projection");
      std::ofstream out{std::filesystem::path(argv[3])};Check(bool(out),"Cannot write light audit");
      for(const auto& light:scene.runtime_lights) {
        out<<std::dec<<light.entity<<'\t'<<int(light.kind)<<'\t'<<light.subtractive<<'\t'<<light.raycast;
        auto number=[&](float value){out<<'\t'<<std::hex<<std::setw(8)<<std::setfill('0')<<std::bit_cast<uint32_t>(value);};
        for(const auto& vector:{light.position,light.direction,light.ambient,light.diffuse,light.attenuation})for(float v:vector)number(v);
        for(float v:{light.dimmer,light.range_start,light.range_end,light.inner_degrees,light.outer_degrees,light.spot_exponent})number(v);
        out<<'\n';
      }
      Check(bool(out),"Failed writing light audit");
      std::cout<<"PASS: "<<scene.runtime_lights.size()<<" runtime light records decoded from original RVZ; geometry projection unchanged\n";
      return 0;
    }
    Check(argc == 3 || argc == 4,
          "Usage: em1_resource_test original.rvz independent-golden-directory [audit-levels]");
    std::string error;
    const std::vector<uint8_t> hello{0x78,0x9c,0xcb,0x48,0xcd,0xc9,0xc9,0x07,0x00,0x06,0x2c,0x02,0x15};
    std::vector<uint8_t> output(5);
    Check(InflateZlib(hello, output, error) && std::string(output.begin(), output.end()) == "hello", error);
    auto corrupt = hello; corrupt.back() ^= 1;
    Check(!InflateZlib(corrupt, output, error), "Accepted invalid Zlib checksum");
    corrupt = hello; corrupt.push_back(0);
    Check(!InflateZlib(corrupt, output, error), "Accepted trailing Zlib data");
    corrupt = hello; corrupt.pop_back();
    Check(!InflateZlib(corrupt, output, error), "Accepted truncated Zlib stream");
    output.resize(4); Check(!InflateZlib(hello, output, error), "Accepted short Zlib output");
    output.resize(6); Check(!InflateZlib(hello, output, error), "Accepted long Zlib output");
    const std::vector<uint8_t> empty{0x78,0x9c,0x03,0x00,0x00,0x00,0x00,0x01};
    Check(InflateZlib(empty, {}, error), "Rejected empty Zlib stream: " + error);
    std::shared_ptr<RvzSource> source = RvzSource::Open(argv[1], error);
    Check(bool(source), error);
    unsigned resources = 0, scenes = 0;
    for (const auto* name : {"dbc_1st_zonec", "levels", "dbc_playerstart_courtyard"}) {
      auto archive = PakSource::Open(source, std::string("packfiles/") + name + ".pak", error);
      Check(bool(archive), error);
      std::ifstream golden(std::filesystem::path(argv[2]) / (std::string(name) + ".tsv"));
      Check(golden.good(), "Cannot read independent golden manifest");
      std::string line;
      unsigned count = 0;
      while (std::getline(golden, line)) {
        std::istringstream row(line);
        std::string path, size, expected, adapted;
        std::getline(row, path, '\t'); std::getline(row, size, '\t');
        std::getline(row, expected, '\t'); std::getline(row, adapted, '\t');
        std::vector<uint8_t> bytes;
        Check(archive->ReadOriginal(path, bytes, error), path + ": " + error);
        Check(bytes.size() == std::stoull(size) && Hash(bytes) == expected,
              "Native original resource differs from independent oracle: " + path);
        if (!adapted.empty()) {
          SceneAdapterResult scene;
          Check(AdaptScene(bytes, scene, error), path + ": " + error);
          Check(Hash(scene.bytes) == adapted, "Native scene differs from oracle: " + path);
          ++scenes;
        }
        ++resources; ++count;
      }
      Check(count == archive->Entries().size(), "Archive count differs from oracle");
      PackedResource marker; marker.entry.path = "unchanged"; marker.bytes = {0x55};
      for (const auto* invalid : {"../bad.bin", "/absolute.bin", "missing.bin", "x:/file.bin"}) {
        Check(!archive->ReadPacked(invalid, marker, error), "Accepted invalid/missing resource");
        Check(marker.entry.path == "unchanged" && marker.bytes == std::vector<uint8_t>{0x55},
              "Failure changed previous resource output");
      }
    }
    LevelPlan level;
    Check(ReadLevelPlan(source, "DBC_Courtyard_1st", level, error), error);
    Check(level.partitions.size() == 6 && level.shared_scenes.size() == 1,
          "Unexpected first-castle descriptor closure");
    unsigned requested = 0;
    for (const auto& partition : level.partitions) {
      if (partition.name != "stream_3") continue;
      Check(partition.scenes.size() == 1, "Unexpected courtyard scene count");
      AdaptedSceneResource scene;
      Check(ReadAdaptedScene(source, partition.scenes.front(), scene, error), error);
      Check(scene.scene.entity_count > 0 && scene.origin_key.starts_with("em1-rvz-v1:"),
            "Missing native scene or original-source namespace");
      ++requested;
    }
    AdaptedSceneResource spawn;
    Check(ReadAdaptedScene(source, level.shared_scenes.front(), spawn, error), error);
    Check(spawn.scene.entity_count > 0 && requested == 1, "No courtyard/spawn scene resolved");
    const auto previous = level.origin_key;
    Check(!ReadLevelPlan(source, "../escape", level, error) && level.origin_key == previous,
          "Invalid level request changed previous plan");
    if (argc == 4) {
      auto catalog = PakSource::Open(source, "packfiles/levels.pak", error);
      Check(bool(catalog), error);
      unsigned accepted = 0, rejected = 0;
      for (const auto& entry : catalog->Entries()) {
        if (!entry.path.ends_with(".level")) continue;
        LevelPlan candidate;
        if (ReadLevelPlan(source, entry.path.substr(7), candidate, error)) ++accepted;
        else { ++rejected; std::cout << "LEVEL_REJECT " << entry.path << ": " << error << '\n'; }
      }
      std::cout << "LEVEL_AUDIT accepted=" << accepted << " rejected=" << rejected << '\n';
    }
    std::cout << "PASS: " << resources << " original resources, " << scenes
              << " scene adapters; native RVZ-to-courtyard/spawn resolution; strict Zlib failures\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
