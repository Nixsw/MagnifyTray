#include "tray_application.h"

#include <Windows.h>

int WINAPI wWinMain(
    const HINSTANCE instance,
    HINSTANCE,
    wchar_t*,
    int) {
    magnify::TrayApplication application(instance);
    return application.Run();
}
