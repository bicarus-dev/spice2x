#pragma once

#include <windows.h>

namespace launcher::splash {

    void start();
    void close(HWND candidate, const char *source);
    void stop();
}