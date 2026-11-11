#pragma once
#include <filesystem>
#include <string>
#include <cstdint>
namespace em2 {
enum class DemoSupportState {Idle,Checking,Available,Downloading,Ready,NoRelease,Error};
struct DemoSupportSnapshot {
  DemoSupportState state=DemoSupportState::Idle;
  std::string status,version,update_version,report_preview,report_path,report_sha256;
  uint64_t downloaded=0,total=0;
  bool auto_update=false,install_on_exit=false,clean_release=false,report_exported=false;
  // Recovery replaced the running image, or an install/recovery is in progress.
  // Caller must stop before SDK/guest setup and show status to the user.
  bool restart_required=false;
};
// Call before the SDK starts. Does not execute guest code or transmit reports.
void InitializeDemoSupport(const std::filesystem::path& executable_directory,
                           const std::filesystem::path& user_directory);
DemoSupportSnapshot SnapshotDemoSupport();
// 7300 preview,7301 export,7302 issue browser,7303 check,7304 download,
// 7305 install at normal exit,7306 auto-update toggle,7307 report folder,
// 7308 cancel install. 7310 status,7311 preview,7312 update summary queries.
// command is optional user-entered issue description for report actions.
bool SupportOption(int id,const std::string& command,std::string& output);
// Native same-process report preview; call on UI thread, optional game HWND.
bool ShowDemoSupportPreview(void* owner,std::string& output);
// AFTER SDK/audio/window shutdown. Installs a verified staged update using
// Win32 moves/rollback in this same process. No forced quit/helper program.
// restart=true starts only the newly installed canonical beetleworx.exe.
bool ShutdownDemoSupport(bool restart,std::string& output);
bool TestDemoSupport(std::string& error);
// Explicit developer diagnostic: download the published demo and its update,
// then use the normal installer only inside a new, isolated directory.
bool TestLiveDemoUpdate(const std::filesystem::path& isolated_directory,std::string& error);
// Explicit headless developer diagnostic ONLY. Copies the running canonical
// image to a non-executable-named data file, proves the Windows mapped-image
// rename primitive, then restores the original file identity/hash immediately.
// Does not start a process or change versions. Never run automatically.
bool TestRunningDemoSupportSwap(std::string& error);
}
