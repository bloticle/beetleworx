#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
struct PPCContext;
namespace em2 {
// Fixed-action diagnostics only in a validated isolated native online QA root.
// Called at the original coroutine-scheduler boundary; never retains a VM.
void TickPrivateNativeRestart(PPCContext&,uint8_t* base);
std::string QueueGameLua(std::string_view source,bool needs_world=false);
std::string QueueRetailLevelLoad(std::string_view state,bool needs_world=true);
std::optional<std::string> GameLuaConsoleSource(std::string_view line);
unsigned CancelQueuedGameLua();
unsigned PendingGameLua();
std::optional<std::string> ExecuteGameplayConsoleTool(const std::vector<std::string>&);
std::optional<std::vector<std::string>> GameplayConsoleCompletions(const std::vector<std::string>&);
void DrawGameplayConsole();
bool TestGameplayConsole();
}
