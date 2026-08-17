#include <windows.h>

#include "app/ApplicationController.h"

// Native Windows entry point (docs/01 §6).
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    return vw::app::ApplicationController().run();
}
