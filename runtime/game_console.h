#pragma once
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace em2 {
std::string QueueGameLua(std::string_view source,bool needs_world=false);
std::optional<std::string> GameLuaConsoleSource(std::string_view line);
unsigned CancelQueuedGameLua();
unsigned PendingGameLua();
std::optional<std::string> ExecuteGameplayConsoleTool(const std::vector<std::string>&);
std::optional<std::vector<std::string>> GameplayConsoleCompletions(const std::vector<std::string>&);
void DrawGameplayConsole();
bool TestGameplayConsole();
}
