#pragma once
#include <filesystem>
#include <functional>
#include <string>
namespace rex::kernel::xam {class UserProfile;}
namespace em2::net {
void RegisterNativeContentRoot(const void* manager,std::function<bool()> packages_closed);
void UnregisterNativeContentRoot(const void* manager);
std::filesystem::path NativeContentRoot(const void* manager,const std::filesystem::path& original);
// Lifecycle owner only: guest save/profile IO must already be quiescent.
// Rejects live packages; never closes handles or rewrites the original root.
// Supply the live profile for an in-runtime handoff. This invalidates the three
// title save-summary settings atomically with the route change. The native
// SaveManager still needs its authored load/retry path after this succeeds.
bool SwitchNativeContentRoot(const void* manager,const std::filesystem::path& isolated_root,std::string& error,
                             rex::kernel::xam::UserProfile* profile=nullptr);
// Restore the ContentManager's original root, without selecting a guessed
// path or closing live packages. Lifecycle owner only, after guest save IO is
// quiescent. The title must still perform its original SaveManager refresh.
// Repeating an already restored route leaves current summary settings intact.
bool RestoreNativeContentRoot(const void* manager,std::string& error,
                              rex::kernel::xam::UserProfile* profile=nullptr);
bool TestNativeContentRoot(const std::filesystem::path& fixture);
}
