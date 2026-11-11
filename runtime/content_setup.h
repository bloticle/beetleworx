#pragma once
#include <filesystem>
#include <string>

namespace em2 {
// Builds only the adjacent content view. Original files and user_data are never
// written. Explicit command-line paths and noninteractive diagnostics stay strict.
bool PrepareGameContent(const std::filesystem::path& executable_directory,
                        std::filesystem::path& game_directory,
                        const std::filesystem::path& content_directory,
                        bool explicit_game_directory, bool interactive,
                        std::string& error);
void ShowContentSetupError(const std::string& error);
bool TestContentSetup(const std::filesystem::path& isolated_directory);
void InitializeContentLibrary(const std::filesystem::path& user_directory);
std::filesystem::path ConfiguredEm1Source();
// Native Scaleform library page: 7200 selects an RVZ, 7201 reports EM2 setup.
bool NativeContentLibraryOption(int id,int choice,int& result);
bool TestContentLibraryPersistence(const std::filesystem::path& isolated_directory);
bool SelectEm1SourceForSetup(const std::filesystem::path& user_directory,std::string& error);
}
