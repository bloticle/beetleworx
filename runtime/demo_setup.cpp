#include "demo_setup.h"
#include "content_setup.h"
#include "em1_rvz.h"
#include "pc_settings.h"
#include <rex/cvar.h>
#include <windows.h>
#include <commctrl.h>
#include <fstream>
REXCVAR_DEFINE_BOOL(em2_demo_build,false,"Setup","Use the focused public testing demo menus and safe defaults");
REXCVAR_DEFINE_BOOL(em2_demo_em1,false,"Setup","Start the EM1 courtyard compatibility demo in an isolated profile");
REXCVAR_DECLARE(bool,em1_enter_courtyard);
REXCVAR_DECLARE(bool,em1_masked_material_candidate);
REXCVAR_DECLARE(bool,em1_dual_uv_material_candidate);
REXCVAR_DECLARE(bool,em1_cutout_material_candidate);
REXCVAR_DECLARE(std::string,em1_source_rvz);
namespace em2 {
bool PublicDemoBuild(){return REXCVAR_GET(em2_demo_build);}
bool Em1DemoSession(){return REXCVAR_GET(em2_demo_em1);}
bool PrepareDemoSession(const std::filesystem::path& executable,std::filesystem::path& user,
    bool explicit_user,bool interactive,std::string& error) {
    namespace fs=std::filesystem;error.clear();
    if(interactive && !explicit_user && fs::is_regular_file(executable/"demo-channel.txt"))REXCVAR_SET(em2_demo_build,true);
    if(!PublicDemoBuild() && !Em1DemoSession())return true;
    const auto library=user;
    InitializeContentLibrary(library);
    if(PublicDemoBuild() && interactive && !Em1DemoSession()) {
        for(;;) {
            const TASKDIALOG_BUTTON buttons[]={
                {100,L"Play Epic Mickey 2\nYour Xbox 360 game, recompiled for PC"},
                {101,L"Play Epic Mickey compatibility demo\nExplore the original Dark Beauty Castle courtyard"},
                {102,L"Add or locate Epic Mickey (Wii)\nSelect a Wii disc image or extracted folder; the location is remembered"}};
            TASKDIALOGCONFIG dialog{};dialog.cbSize=sizeof(dialog);dialog.dwFlags=TDF_USE_COMMAND_LINKS|TDF_ALLOW_DIALOG_CANCELLATION;
            dialog.dwCommonButtons=TDCBF_CANCEL_BUTTON;dialog.pszWindowTitle=L"Beetleworx - Public Testing Demo";
            dialog.pszMainInstruction=L"What would you like to play?";
            dialog.pszContent=L"Setup is built into this executable. Both modes use your Epic Mickey 2 Xbox 360 files. The Wii preview also needs an Epic Mickey Wii disc image or extracted folder. Original game files stay unchanged.";
            dialog.cButtons=3;dialog.pButtons=buttons;dialog.nDefaultButton=100;
            int choice=IDCANCEL;
            if(FAILED(TaskDialogIndirect(&dialog,&choice,nullptr,nullptr))){error="Could not open the game selection window.";return false;}
            if(choice==IDCANCEL)return false;
            if(choice==100)break;
            if(choice==102){if(!SelectEm1SourceForSetup(library,error) && !error.empty())ShowContentSetupError(error);error.clear();continue;}
            if(choice==101) {
                if(!fs::is_regular_file(ConfiguredEm1Source()) && !SelectEm1SourceForSetup(library,error)) {
                    if(!error.empty())ShowContentSetupError(error);error.clear();continue;
                }
                REXCVAR_SET(em2_demo_em1,true);break;
            }
        }
    }
    if(Em1DemoSession()) {
        const auto explicit_source=REXCVAR_GET(em1_source_rvz);
        const auto source_path=explicit_source.empty()?ConfiguredEm1Source():fs::path(std::u8string(explicit_source.begin(),explicit_source.end()));
        auto source=em1::RvzSource::Open(source_path,error);
        if(!source || !source->DiscId().starts_with("SEM")){if(error.empty())error="Choose an Epic Mickey Wii disc image or extracted disc folder.";return false;}
        const auto encoded=source_path.u8string();REXCVAR_SET(em1_source_rvz,std::string(encoded.begin(),encoded.end()));
        REXCVAR_SET(em1_enter_courtyard,true);REXCVAR_SET(em1_masked_material_candidate,true);
        REXCVAR_SET(em1_dual_uv_material_candidate,true);REXCVAR_SET(em1_cutout_material_candidate,true);
        // Fresh native profile, separate from EM2's campaign. Copy preferences
        // only, never campaign containers or a developer's save.
        if(!explicit_user) {
            user=library/"em1-preview";fs::create_directories(user);
            for(const char* name:{"controls.ini","pc-settings.ini"})if(fs::is_regular_file(library/name) && !fs::exists(user/name))
                fs::copy_file(library/name,user/name);
        }
    }
    return true;
}
void ApplyPublicDemoSettings() {
    if(!PublicDemoBuild() && !Em1DemoSession())return;
    ChangePCSetting(15,0);ChangePCSetting(21,0);ChangePCSetting(25,0);
    // GameplayTuning supplies enabled movement defaults for fresh profiles.
    // Keep saved ability choices instead of overwriting them at every launch.
}
}
