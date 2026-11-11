// Delegate startup to the installed SDK after applying this title's defaults.
// CLI and environment parsing still happens inside the unchanged SDK entry.
#define wWinMain EM2SdkWindowedMain
#include "windowed_app_main_win.cpp"
#undef wWinMain

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous,
                    LPWSTR command_line, int show_command) {
    bool found = false;
    for (auto& flag : rex::cvar::GetRegistry()) {
        if (flag.name != "execute_unclipped_draw_vs_on_cpu") continue;
        if (flag.type != rex::cvar::FlagType::Boolean || !flag.setter("true")) {
            return EXIT_FAILURE;
        }
        flag.default_value = "true";
        found = true;
        break;
    }
    if (!found) return EXIT_FAILURE;
    return EM2SdkWindowedMain(instance, previous, command_line, show_command);
}
