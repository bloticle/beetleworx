#pragma once
#include "em1_audio_bank.h"
#include "em1_level_source.h"
namespace em2::em1 {
struct OriginalAudioDeclaration {
  std::string partition,source_path,authored_flag;
  size_t resource_ordinal=0;
};
struct OriginalAudioEvent {
  std::shared_ptr<const WiiAudioBank> bank;
  uint32_t id=0;
  std::vector<uint32_t> actions;
};
// Asset ownership only. Reuses the original46 reader and pins its source mount;
// it does NOT publish Wii banks into native62, activate partitions, interpret
// event graphs or authorize a Lua/native audio capability. Banks are read lazily
// and exclusively from this level's declared Audio/Wii namespace. Serialize use.
class LevelOriginalAudio {
 public:
  const std::string& SourceIdentity()const{return source_identity_;}
  const std::string& LevelOrigin()const{return level_origin_;}
  const std::vector<OriginalAudioDeclaration>& Declarations()const{return declarations_;}
  size_t UniqueBanks()const{return files_.size();}
  uint64_t RetainedBytes()const{return retained_bytes_;}
  bool ReadBank(std::string_view declared_name,std::shared_ptr<const WiiAudioBank>&,std::string& error);
  bool InspectEvent(std::string_view declared_bank,std::string_view event_name,OriginalAudioEvent&,std::string& error);
 private:
  friend bool PrepareLevelOriginalAudio(std::shared_ptr<RvzSource>,const LevelPlan&,std::shared_ptr<LevelOriginalAudio>&,std::string&);
  friend bool TestLevelOriginalAudioOwnership();
  std::shared_ptr<RvzSource> source_;
  std::string source_identity_,level_origin_;
  std::vector<OriginalAudioDeclaration> declarations_;
  std::vector<LevelEntryPartition> entry_partitions_;
  std::map<std::string,RvzFile> files_;
  std::map<std::string,std::shared_ptr<const WiiAudioBank>> loaded_;
  uint64_t retained_bytes_=0;
};
bool PrepareLevelOriginalAudio(std::shared_ptr<RvzSource>,const LevelPlan&,
    std::shared_ptr<LevelOriginalAudio>&,std::string& error);
bool TestLevelOriginalAudioOwnership();
}
