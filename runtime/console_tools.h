#pragma once
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace em2 {
struct ConsoleCommandInfo {const char* name;const char* group;const char* usage;const char* description;const char* example;};
std::span<const ConsoleCommandInfo> ConsoleCommandCatalog();
std::optional<std::string> ExecuteConsoleTool(const std::vector<std::string>&);
std::optional<std::vector<std::string>> ConsoleToolCompletions(const std::vector<std::string>&);
void DrawConsoleBrowser();
void DrawConsoleTools();
bool TestConsoleTools();
}
