#pragma once
#include <cstdint>
#include <string>
#include <string_view>
namespace em2 {
// Applies to subsequent native BSQ dialogue lines; never changes actor lookup,
// audio, the stored sequence, or the original archive.
void ConfigureRestoredGremlinNames(bool enabled);
bool RestoredGremlinNamesEnabled();
bool IsRestorableGremlinSpeaker(std::string_view authored_character,
                               std::string_view speaker_key);
bool TestRestoredGremlinNames(std::string& error);
}
