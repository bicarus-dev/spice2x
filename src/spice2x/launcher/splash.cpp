#include "splash.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include <windows.h>

#include "build/defs.h"
#include "build/icon.h"
#include "launcher/shutdown.h"
#include "util/logging.h"

namespace launcher::splash {

    namespace {

        // the splash is a small, non-activating Win32 window rendered entirely with GDI.
        // keeping it independent from the game's graphics APIs lets it appear before the
        // game creates a D3D device and avoids interfering with that device during startup.
        constexpr wchar_t CLASS_NAME[] = L"Spice2xSplashWindow";
        constexpr COLORREF BACKGROUND_COLOR = RGB(28, 29, 32);
        constexpr COLORREF ACCENT_COLOR = RGB(239, 91, 69);
        constexpr COLORREF PRIMARY_TEXT_COLOR = RGB(245, 245, 242);
        constexpr COLORREF SECONDARY_TEXT_COLOR = RGB(174, 176, 181);
        constexpr COLORREF PROGRESS_TRACK_COLOR = RGB(52, 54, 59);
        constexpr COLORREF CLOSE_HOVER_COLOR = RGB(58, 60, 65);
        constexpr COLORREF CLOSE_PRESSED_COLOR = RGB(196, 57, 47);
        constexpr COLORREF BORDER_COLOR = RGB(66, 68, 73);

        // fixed geometry is collected here so the layout can be understood and adjusted
        // without hunting through the drawing code. coordinates are client-area pixels.
        constexpr int WINDOW_WIDTH = 430;
        constexpr int WINDOW_HEIGHT = 154;
        constexpr int ACCENT_WIDTH = 6;
        constexpr int CONTENT_LEFT = 124;
        constexpr int CONTENT_RIGHT_MARGIN = 24;
        constexpr int ICON_SIZE = 64;
        constexpr int ICON_LEFT = 34;
        constexpr int ICON_VERTICAL_OFFSET = -9;
        constexpr int TITLE_TOP = 22;
        constexpr int TITLE_BOTTOM = 55;
        constexpr int VERSION_TOP = 56;
        constexpr int VERSION_BOTTOM = 78;
        constexpr int STATUS_TOP = 79;
        constexpr int STATUS_BOTTOM = 104;
        constexpr int PROGRESS_TOP = 118;
        constexpr int PROGRESS_BOTTOM = 122;
        constexpr int PROGRESS_SEGMENT_WIDTH = 58;
        constexpr int PROGRESS_PIXELS_PER_SECOND = 88;

        constexpr WORD ARROW_CURSOR_ID = 32512;
        constexpr UINT_PTR ANIMATION_TIMER_ID = 1;
        constexpr UINT ANIMATION_INTERVAL_MS = 16;
        constexpr auto RECENTER_INTERVAL = std::chrono::milliseconds(250);

        // state_mutex protects creation and transfer of ui_thread. start() waits on
        // state_cv until the UI thread has either created the window or failed to do so.
        std::mutex state_mutex;
        std::condition_variable state_cv;
        std::jthread ui_thread;
        bool startup_complete = false;

        // win-event callbacks and stop() run outside the splash thread. they use these
        // atomics only to post messages; the splash thread remains the HWND owner.
        std::atomic<HWND> splash_window = nullptr;
        std::atomic<DWORD> ui_thread_id = 0;
        std::atomic<bool> close_requested = false;

        template <typename T>
        void delete_gdi_object(T &object) {
            if (object) {
                DeleteObject(object);
                object = nullptr;
            }
        }

        struct GdiResources {
            HBRUSH background_brush = nullptr;
            HBRUSH accent_brush = nullptr;
            HBRUSH progress_track_brush = nullptr;
            HBRUSH close_hover_brush = nullptr;
            HBRUSH close_pressed_brush = nullptr;
            HPEN border_pen = nullptr;
            HPEN close_pen = nullptr;
            HFONT title_font = nullptr;
            HFONT status_font = nullptr;
            HICON icon = nullptr;

            bool initialize(HINSTANCE instance) {
                background_brush = CreateSolidBrush(BACKGROUND_COLOR);
                accent_brush = CreateSolidBrush(ACCENT_COLOR);
                progress_track_brush = CreateSolidBrush(PROGRESS_TRACK_COLOR);
                close_hover_brush = CreateSolidBrush(CLOSE_HOVER_COLOR);
                close_pressed_brush = CreateSolidBrush(CLOSE_PRESSED_COLOR);
                border_pen = CreatePen(PS_SOLID, 1, BORDER_COLOR);
                close_pen = CreatePen(PS_SOLID, 2, PRIMARY_TEXT_COLOR);
                title_font = CreateFontW(
                    30, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
                status_font = CreateFontW(
                    15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
                icon = static_cast<HICON>(LoadImageW(
                    instance,
                    MAKEINTRESOURCEW(MAINICON),
                    IMAGE_ICON,
                    ICON_SIZE,
                    ICON_SIZE,
                    LR_DEFAULTCOLOR));
                const bool initialized =
                    background_brush && accent_brush && progress_track_brush &&
                    close_hover_brush && close_pressed_brush && border_pen && close_pen &&
                    title_font && status_font && icon;
                if (!initialized) {
                    destroy();
                }
                return initialized;
            }

            void destroy() {
                if (icon) {
                    DestroyIcon(icon);
                    icon = nullptr;
                }
                delete_gdi_object(status_font);
                delete_gdi_object(title_font);
                delete_gdi_object(close_pen);
                delete_gdi_object(border_pen);
                delete_gdi_object(close_pressed_brush);
                delete_gdi_object(close_hover_brush);
                delete_gdi_object(progress_track_brush);
                delete_gdi_object(accent_brush);
                delete_gdi_object(background_brush);
            }
        };

        // window state and cached drawing resources never leave the splash UI thread.
        struct UiState {
            GdiResources gdi;
            std::chrono::steady_clock::time_point animation_start;
            std::chrono::steady_clock::time_point last_recenter;
            bool close_hovered = false;
            bool close_pressed = false;
            bool tracking_mouse_leave = false;
        } ui;

        RECT get_close_button_rect(const RECT &client) {
            return { client.right - 40, 1, client.right - 1, 34 };
        }

        RECT get_text_rect(const RECT &client, int top, int bottom) {
            return { CONTENT_LEFT, top, client.right - CONTENT_RIGHT_MARGIN, bottom };
        }

        RECT get_progress_track_rect(const RECT &client) {
            return get_text_rect(client, PROGRESS_TOP, PROGRESS_BOTTOM);
        }

        bool rect_contains(const RECT &outer, const RECT &inner) {
            return inner.left >= outer.left && inner.top >= outer.top &&
                inner.right <= outer.right && inner.bottom <= outer.bottom;
        }

        bool is_close_button_at(HWND window, LPARAM l_param) {
            RECT client {};
            GetClientRect(window, &client);
            const RECT close_rect = get_close_button_rect(client);
            const POINT point {
                static_cast<short>(LOWORD(l_param)),
                static_cast<short>(HIWORD(l_param)),
            };
            return PtInRect(&close_rect, point);
        }

        void center_on_primary_monitor(HWND window) {
            // the MONITOR_DEFAULTTOPRIMARY flag follows whichever display is primary now. this is
            // intentionally recalculated while the splash is visible because the launcher
            // may change monitor topology or resolution during startup.
            MONITORINFO monitor_info {};
            monitor_info.cbSize = sizeof(monitor_info);
            const POINT primary_origin { 0, 0 };
            HMONITOR primary_monitor = MonitorFromPoint(primary_origin, MONITOR_DEFAULTTOPRIMARY);
            if (!GetMonitorInfoW(primary_monitor, &monitor_info)) {
                return;
            }

            RECT window_rect {};
            if (!GetWindowRect(window, &window_rect)) {
                return;
            }

            const int width = window_rect.right - window_rect.left;
            const int height = window_rect.bottom - window_rect.top;
            const RECT &monitor_rect = monitor_info.rcMonitor;
            const int x = monitor_rect.left + (monitor_rect.right - monitor_rect.left - width) / 2;
            const int y = monitor_rect.top + (monitor_rect.bottom - monitor_rect.top - height) / 2;

            // avoid redundant SetWindowPos calls from the periodic monitor check.
            if (window_rect.left != x || window_rect.top != y) {
                SetWindowPos(
                    window,
                    nullptr,
                    x,
                    y,
                    0,
                    0,
                    SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
            }
        }

        void draw_frame(HDC dc, const RECT &client) {
            // draw the background, left accent strip, and one-pixel outer border.
            FillRect(dc, &client, ui.gdi.background_brush);

            RECT accent = client;
            accent.right = ACCENT_WIDTH;
            FillRect(dc, &accent, ui.gdi.accent_brush);

            HGDIOBJ previous_pen = SelectObject(dc, ui.gdi.border_pen);
            HGDIOBJ previous_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, 0, 0, client.right, client.bottom);
            SelectObject(dc, previous_brush);
            SelectObject(dc, previous_pen);
        }

        void draw_close_button(HDC dc, const RECT &client) {
            // draw the close control directly because the splash has no native title bar.
            // its click path shuts down the process; ordinary WM_CLOSE still only hides splash.
            const RECT close_rect = get_close_button_rect(client);
            if (ui.close_hovered || ui.close_pressed) {
                HBRUSH close_brush = ui.close_pressed ?
                    ui.gdi.close_pressed_brush : ui.gdi.close_hover_brush;
                FillRect(dc, &close_rect, close_brush);
            }
            const int center_x = (close_rect.left + close_rect.right) / 2;
            const int center_y = (close_rect.top + close_rect.bottom) / 2;
            const int glyph_half_size = 5;
            HGDIOBJ previous_pen = SelectObject(dc, ui.gdi.close_pen);
            MoveToEx(
                dc,
                center_x - glyph_half_size,
                center_y - glyph_half_size,
                nullptr);
            LineTo(
                dc,
                center_x + glyph_half_size,
                center_y + glyph_half_size);
            MoveToEx(
                dc,
                center_x + glyph_half_size,
                center_y - glyph_half_size,
                nullptr);
            LineTo(
                dc,
                center_x - glyph_half_size,
                center_y + glyph_half_size);
            SelectObject(dc, previous_pen);
        }

        void draw_icon(HDC dc, const RECT &client) {
            const int icon_top =
                (client.bottom - ICON_SIZE) / 2 + ICON_VERTICAL_OFFSET;
            DrawIconEx(
                dc,
                ICON_LEFT,
                icon_top,
                ui.gdi.icon,
                ICON_SIZE,
                ICON_SIZE,
                0,
                nullptr,
                DI_NORMAL);
        }

        void draw_text(
                HDC dc, HFONT font, COLORREF color, const wchar_t *text, RECT bounds) {
            SetBkMode(dc, TRANSPARENT);
            HGDIOBJ previous_font = SelectObject(dc, font);
            SetTextColor(dc, color);
            DrawTextW(dc, text, -1, &bounds, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            SelectObject(dc, previous_font);
        }

        void draw_text_content(HDC dc, const RECT &client) {
            const std::string version_string(VERSION_STRING_CFG);
            const std::wstring version(version_string.begin(), version_string.end());
            draw_text(
                dc,
                ui.gdi.title_font,
                PRIMARY_TEXT_COLOR,
                L"spice2x",
                get_text_rect(client, TITLE_TOP, TITLE_BOTTOM));
            draw_text(
                dc,
                ui.gdi.status_font,
                SECONDARY_TEXT_COLOR,
                version.c_str(),
                get_text_rect(client, VERSION_TOP, VERSION_BOTTOM));
            draw_text(
                dc,
                ui.gdi.status_font,
                SECONDARY_TEXT_COLOR,
                L"Starting...",
                get_text_rect(client, STATUS_TOP, STATUS_BOTTOM));
        }

        void draw_progress(HDC dc, const RECT &client) {
            // the progress segment repeatedly travels from just left of the track to just
            // beyond its right edge. IntersectRect clips it to the visible track bounds.
            const RECT progress_track = get_progress_track_rect(client);
            FillRect(dc, &progress_track, ui.gdi.progress_track_brush);

            const int travel =
                progress_track.right - progress_track.left + PROGRESS_SEGMENT_WIDTH;
            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - ui.animation_start).count();
            const int segment_left = progress_track.left - PROGRESS_SEGMENT_WIDTH +
                static_cast<int>((elapsed_ms * PROGRESS_PIXELS_PER_SECOND / 1000) % travel);
            RECT progress_segment {
                segment_left,
                progress_track.top,
                segment_left + PROGRESS_SEGMENT_WIDTH,
                progress_track.bottom,
            };
            RECT visible_segment {};
            if (IntersectRect(&visible_segment, &progress_segment, &progress_track)) {
                FillRect(dc, &visible_segment, ui.gdi.accent_brush);
            }
        }

        void draw_full_frame(HDC dc, const RECT &client) {
            draw_frame(dc, client);
            draw_close_button(dc, client);
            draw_icon(dc, client);
            draw_text_content(dc, client);
            draw_progress(dc, client);
        }

        void draw_buffered_frame(HDC window_dc, const RECT &client) {
            HDC buffer_dc = CreateCompatibleDC(window_dc);
            HBITMAP buffer_bitmap = buffer_dc ? CreateCompatibleBitmap(
                window_dc, client.right, client.bottom) : nullptr;

            // a direct full-frame draw is still correct if the temporary back buffer fails.
            if (!buffer_dc || !buffer_bitmap) {
                if (buffer_dc) {
                    DeleteDC(buffer_dc);
                }
                draw_full_frame(window_dc, client);
                return;
            }

            HGDIOBJ previous_bitmap = SelectObject(buffer_dc, buffer_bitmap);
            draw_full_frame(buffer_dc, client);
            BitBlt(
                window_dc,
                0,
                0,
                client.right,
                client.bottom,
                buffer_dc,
                0,
                0,
                SRCCOPY);
            SelectObject(buffer_dc, previous_bitmap);
            DeleteObject(buffer_bitmap);
            DeleteDC(buffer_dc);
        }

        void paint(HWND window) {
            PAINTSTRUCT paint {};
            HDC window_dc = BeginPaint(window, &paint);
            if (!window_dc) {
                return;
            }

            RECT client {};
            GetClientRect(window, &client);
            const RECT progress_track = get_progress_track_rect(client);

            // timer ticks invalidate only the progress track. draw that small region directly;
            // every other paint uses a back buffer so the complete frame appears atomically.
            if (rect_contains(progress_track, paint.rcPaint)) {
                draw_progress(window_dc, client);
            } else {
                draw_buffered_frame(window_dc, client);
            }
            EndPaint(window, &paint);
        }

        LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
            // all window messages are dispatched on the dedicated splash thread.
            switch (message) {
                case WM_PAINT:
                    paint(window);
                    return 0;
                case WM_ERASEBKGND:
                    return 1;
                case WM_MOUSEMOVE: {
                    const bool hovered = is_close_button_at(window, l_param);
                    if (ui.close_hovered != hovered) {
                        ui.close_hovered = hovered;
                        InvalidateRect(window, nullptr, FALSE);
                    }
                    if (!ui.tracking_mouse_leave) {
                        TRACKMOUSEEVENT tracking {
                            .cbSize = sizeof(tracking),
                            .dwFlags = TME_LEAVE,
                            .hwndTrack = window,
                            .dwHoverTime = 0,
                        };
                        ui.tracking_mouse_leave = TrackMouseEvent(&tracking);
                    }
                    return 0;
                }
                case WM_MOUSELEAVE:
                    ui.close_hovered = false;
                    ui.tracking_mouse_leave = false;
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                case WM_LBUTTONDOWN:
                    if (is_close_button_at(window, l_param)) {
                        ui.close_pressed = true;
                        SetCapture(window);
                        InvalidateRect(window, nullptr, FALSE);
                    }
                    return 0;
                case WM_LBUTTONUP:
                    if (ui.close_pressed) {
                        const bool close_clicked = is_close_button_at(window, l_param);
                        ui.close_pressed = false;
                        ReleaseCapture();
                        InvalidateRect(window, nullptr, FALSE);
                        if (close_clicked) {
                            log_info("splash", "close button clicked, shutting down");
                            std::thread([] { launcher::shutdown(); }).detach();
                        }
                    }
                    return 0;
                case WM_CAPTURECHANGED:
                    ui.close_pressed = false;
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                case WM_DISPLAYCHANGE:
                case WM_SETTINGCHANGE:
                    // react immediately when Windows reports a display topology change.
                    center_on_primary_monitor(window);
                    return 0;
                case WM_TIMER:
                    if (w_param == ANIMATION_TIMER_ID) {
                        const auto now = std::chrono::steady_clock::now();
                        if (now - ui.last_recenter >= RECENTER_INTERVAL) {
                            center_on_primary_monitor(window);
                            ui.last_recenter = now;
                        }
                        RECT client {};
                        GetClientRect(window, &client);
                        const RECT progress_track = get_progress_track_rect(client);
                        InvalidateRect(window, &progress_track, FALSE);
                    }
                    return 0;
                case WM_CLOSE:
                    DestroyWindow(window);
                    return 0;
                case WM_DESTROY:
                    log_info("splash", "closing splash window");
                    KillTimer(window, ANIMATION_TIMER_ID);
                    PostQuitMessage(0);
                    return 0;
                default:
                    return DefWindowProcW(window, message, w_param, l_param);
            }
        }

        void CALLBACK window_event_proc(
                HWINEVENTHOOK, DWORD event, HWND window, LONG object_id,
                LONG child_id, DWORD, DWORD) {
            // request closure asynchronously instead of destroying the HWND from a hook callback.
            if (event == EVENT_OBJECT_SHOW && object_id == OBJID_WINDOW &&
                child_id == CHILDID_SELF && window && window != splash_window.load()) {
                close(window, "WinEvent EVENT_OBJECT_SHOW");
            }
        }

        void signal_startup_complete() {
            // unblock start() after window creation has either succeeded or failed. waiting
            // here prevents callers from racing stop() against an uninitialized UI thread.
            std::lock_guard<std::mutex> lock(state_mutex);
            startup_complete = true;
            state_cv.notify_all();
        }

        void run_ui_thread() {
            // this thread owns the splash class, HWND, timer, event hook, and message pump.
            ui_thread_id.store(GetCurrentThreadId());
            HINSTANCE instance = GetModuleHandleW(nullptr);

            WNDCLASSW window_class {};
            window_class.lpfnWndProc = window_proc;
            window_class.hInstance = instance;
            window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(ARROW_CURSOR_ID));
            window_class.lpszClassName = CLASS_NAME;
            if (!RegisterClassW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                log_warning("splash", "failed to register splash window class: {}", GetLastError());
                signal_startup_complete();
                ui_thread_id.store(0);
                return;
            }

            if (!ui.gdi.initialize(instance)) {
                log_warning("splash", "failed to create GDI resources");
                UnregisterClassW(CLASS_NAME, instance);
                signal_startup_complete();
                ui_thread_id.store(0);
                return;
            }

            // observe this process showing a visible top-level window.
            HWINEVENTHOOK event_hook = SetWinEventHook(
                EVENT_OBJECT_SHOW,
                EVENT_OBJECT_SHOW,
                nullptr,
                window_event_proc,
                GetCurrentProcessId(),
                0,
                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNTHREAD);
            if (!event_hook) {
                log_warning(
                    "splash", "failed to register window event hook: {}",
                    GetLastError());
                ui.gdi.destroy();
                UnregisterClassW(CLASS_NAME, instance);
                signal_startup_complete();
                ui_thread_id.store(0);
                return;
            }

            // the TOOLWINDOW style keeps the splash out of the taskbar, and NOACTIVATE prevents
            // it from stealing keyboard focus.
            HWND window = CreateWindowExW(
                WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                CLASS_NAME,
                L"spice2x",
                WS_POPUP,
                0,
                0,
                WINDOW_WIDTH,
                WINDOW_HEIGHT,
                nullptr,
                nullptr,
                instance,
                nullptr);
            const DWORD window_error = window ? ERROR_SUCCESS : GetLastError();
            splash_window.store(window);
            if (window) {
                log_info("splash", "created splash window");
                ui.animation_start = std::chrono::steady_clock::now();
                ui.last_recenter = ui.animation_start;
                ui.close_hovered = false;
                ui.close_pressed = false;
                ui.tracking_mouse_leave = false;
                center_on_primary_monitor(window);
                if (!SetTimer(window, ANIMATION_TIMER_ID, ANIMATION_INTERVAL_MS, nullptr)) {
                    log_warning("splash", "failed to start animation timer: {}", GetLastError());
                }
                ShowWindow(window, SW_SHOWNOACTIVATE);
                UpdateWindow(window);
            } else {
                log_warning("splash", "failed to create splash window: {}", window_error);
            }
            signal_startup_complete();

            // the GetMessage function blocks efficiently between paint, timer, display, and close events.
            // receiving WM_DESTROY posts WM_QUIT, which terminates this loop.
            if (window) {
                MSG message {};
                while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                if (IsWindow(window)) {
                    DestroyWindow(window);
                }
            }

            // release resources in the reverse order of setup before publishing that the UI
            // thread is no longer available to receive posted messages.
            if (event_hook) {
                UnhookWinEvent(event_hook);
            }
            ui.gdi.destroy();
            splash_window.store(nullptr);
            UnregisterClassW(CLASS_NAME, instance);
            ui_thread_id.store(0);
        }
    }

    void start() {
        // start is idempotent and synchronous only through initial window creation. The
        // splash continues running independently after this function returns.
        std::unique_lock<std::mutex> lock(state_mutex);
        if (ui_thread.joinable()) {
            return;
        }

        startup_complete = false;
        close_requested.store(false);
        ui_thread = std::jthread(run_ui_thread);
        state_cv.wait(lock, [] { return startup_complete; });
    }

    void close(HWND candidate, const char *source) {
        HWND window = splash_window.load();
        if (!window || !candidate || GetAncestor(candidate, GA_ROOT) != candidate ||
            !IsWindowVisible(candidate) || close_requested.exchange(true)) {
            return;
        }

        log_info(
            "splash", "close requested: source={}, hwnd={}",
            source, fmt::ptr(candidate));
        if (!PostMessageW(window, WM_CLOSE, 0, 0)) {
            const DWORD error = GetLastError();
            close_requested.store(false);
            log_warning(
                "splash", "failed to schedule close for hwnd={}: {}",
                fmt::ptr(candidate), error);
        }
    }

    void stop() {
        // ask the UI thread to exit through its message pump, then join it. move the thread
        // object out while holding the mutex so no concurrent start() can reuse it early.
        std::jthread thread;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (!ui_thread.joinable()) {
                return;
            }

            HWND window = splash_window.load();
            if (window) {
                PostMessageW(window, WM_CLOSE, 0, 0);
            } else {
                const DWORD thread_id = ui_thread_id.load();
                if (thread_id) {
                    PostThreadMessageW(thread_id, WM_QUIT, 0, 0);
                }
            }
            thread = std::move(ui_thread);
        }

        // never join while holding state_mutex: the UI thread may still need that mutex to
        // complete its startup notification.
        thread.join();
    }

}