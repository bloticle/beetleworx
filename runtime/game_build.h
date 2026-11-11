#pragma once
#include <filesystem>
#include <string>
namespace rex {class Runtime;}
namespace em2 {
// Decode only. Never executes the candidate or resolves its kernel imports.
bool VerifyCompatibleGameXex(rex::Runtime&,const std::filesystem::path&,std::string& error);
bool TestCompatibleGameXex(rex::Runtime&,const std::filesystem::path&,std::string& error);
}
