#include <windows.h>

#include "app/ApplicationController.h"

// Native Windows entry point (docs/01 §6).
// PWSTR pCmdLine contains command line args — when launched via file
// association (e.g. double-clicking .mp4), Windows passes the file path.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR pCmdLine, int) {
    vw::app::ApplicationController app;
    app.setCommandLine(pCmdLine ? pCmdLine : L"");
    return app.run();
}
