#pragma once
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include "em1_navigation_data.h"
#include "em1_parser_probe.h"
#include "em1_scene_adapter.h"
namespace em2::em1 {
struct LevelOriginalScripts;
// Initial registry parse only. Adds a complete original .level entry without
// changing retail registry records. Declared resources retain source identity;
// registration alone does not establish platform-resource readiness.
bool ExtendNativeLevelRegistry(PPCContext&,uint8_t* base,std::span<const uint8_t> original,
    std::string_view descriptor,std::vector<uint8_t>& output,std::string& error,
    std::string_view reserved_prefix={},std::span<const uint32_t> reserved_group_ids={});
// Initial lightweight catalog: reserve every original descriptor's native
// state/resources, preparing its assets only after an explicit load request.
bool ExtendDeferredNativeLevelRegistry(uint8_t* base,std::span<const uint8_t> retail,
    std::span<const uint8_t> registered,std::vector<uint8_t>& output,std::string& error);
bool PrepareDeferredRegisteredLevel(PPCContext&,uint8_t* base,std::string_view descriptor,std::string& error);
struct LevelPreparationStatus {bool catalogued=false,ready=false;std::string error;};
LevelPreparationStatus RegisteredLevelPreparation(std::string_view descriptor);
std::string RegisteredLevelState(std::string_view descriptor);
std::shared_ptr<LevelOriginalScripts> RegisteredLevelScripts(std::string_view descriptor_or_state);
std::shared_ptr<const std::vector<NavigationDatabase>> RegisteredLevelNavigation(std::string_view descriptor);
// Engine-thread retained library of the BOUND scenes, including model aliases.
// Inputs and native definitions live together for the registered level's life.
// Actor admission/resource readiness are separate; this publishes no actors.
struct RegisteredBlueprintLibrary {
  uint8_t* guest_base=nullptr;
  std::vector<uint32_t> inputs,definitions;
  NativeBlueprintIndex index;
  std::vector<SceneAdapterResult> scenes;
  std::vector<std::string> scene_names;
  std::atomic<uint64_t> parent_resolutions{0};
};
class RegisteredLevelActorScope {
 public:
  explicit RegisteredLevelActorScope(std::shared_ptr<RegisteredBlueprintLibrary>);
  ~RegisteredLevelActorScope();
  RegisteredLevelActorScope(const RegisteredLevelActorScope&)=delete;
  RegisteredLevelActorScope& operator=(const RegisteredLevelActorScope&)=delete;
 private:std::shared_ptr<RegisteredBlueprintLibrary> previous_;
};
bool IsScopedRegisteredLevelActor(uint8_t*,uint32_t entity);
bool BeginRegisteredLevelScripts(uint8_t*,std::string_view state,std::string& error);
bool CommitRegisteredLevelScripts(uint8_t*,std::string& error);
bool EndRegisteredLevelScripts(PPCContext&,uint8_t*,std::string& error);
void TickRegisteredLevelScripts(PPCContext&,uint8_t*,double simulation_delta);
bool RetainRegisteredLevelBlueprints(PPCContext&,uint8_t*,std::string_view,std::string&);
std::shared_ptr<RegisteredBlueprintLibrary> RegisteredLevelBlueprints(std::string_view);
std::shared_ptr<RegisteredBlueprintLibrary> MatchRegisteredSceneBlueprints(uint8_t*,uint32_t input,uint32_t size);
void SetRegisteredParserBlueprints(uint32_t parser,std::shared_ptr<RegisteredBlueprintLibrary>);
std::shared_ptr<RegisteredBlueprintLibrary> RegisteredParserBlueprints(uint32_t parser);
// Engine-thread preflight for the console's actual native CleanLoadLevel path.
bool CanEnterRegisteredLevel(uint8_t* base,std::string_view descriptor,std::string& error);
// Opt-in private normal-loader experiment. Never enabled by the settings UI.
void MaybeProbeRegisteredLevelEntry();
bool RegisteredLevelEntryProbeRequested();
void ObserveRegisteredLevelEntryRequest(std::string_view state);
}
