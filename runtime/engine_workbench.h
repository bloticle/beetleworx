#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <optional>
#include <vector>
struct ImGuiIO;
namespace rex::input {class InputSystem;}
namespace em2 {
void SetWorkbenchInputSystem(rex::input::InputSystem*);
void InitializeEngineWorkbench(const std::filesystem::path&);
void ShowEngineSettings();
void ShowEngineConsole();
bool EngineWorkbenchKey(unsigned key,bool down);
void ResetEngineWorkbenchInput();
void SetEngineWorkbenchFocused(bool focused);
void DrawEngineWorkbench(ImGuiIO&);
bool EngineWorkbenchOption(int id,int command,int& result);
void ExecuteEngineConsole(std::string_view);
void DraftEngineConsoleCommand(std::string_view);
std::optional<std::vector<std::string>> TokenizeEngineConsole(std::string_view);
void EngineConsoleMessage(const std::string&);
bool TestEngineWorkbench();
}
