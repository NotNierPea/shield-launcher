#include "std_include.hpp"
#include "launcher_actions.hpp"
#include "page_actions.hpp"
#include "../launcher_funcs/auto_update.hpp"
#include <algorithm>
#include <dwmapi.h>
#include <mmsystem.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <wrl.h>
#include <WebView2.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comdlg32.lib")

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {
    constexpr int window_width = 1000;
    constexpr int window_height = 620;
    constexpr UINT run_on_ui_message = WM_APP + 1;
    constexpr UINT_PTR close_timer = 1;
    constexpr UINT_PTR sound_timer = 2;
    constexpr const wchar_t* page_origin = L"https://launcher.shield";

    // the splash icon, the page's own splash matches this size so the handoff is seamless
    constexpr int splash_icon_size = 64;

    HWND main_window = nullptr;
    HICON splash_icon = nullptr;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;

    launcher::settings settings;
    std::unique_ptr<page::controller> page_controller;

    std::mutex ui_tasks_mutex;
    std::queue<std::function<void()>> ui_tasks;

    // worker threads hand their results back to the window thread through this
    void run_on_ui(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(ui_tasks_mutex);
            ui_tasks.push(std::move(task));
        }
        PostMessageW(main_window, run_on_ui_message, 0, 0);
    }

    void run_ui_tasks() {
        std::queue<std::function<void()>> tasks;
        {
            std::lock_guard<std::mutex> lock(ui_tasks_mutex);
            std::swap(tasks, ui_tasks);
        }

        while (!tasks.empty()) {
            tasks.front()();
            tasks.pop();
        }
    }

    std::wstring to_wide(const std::string& text) {
        int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring wide(size, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
        return wide;
    }

    std::string to_utf8(const std::wstring& text) {
        int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string narrow(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(), size, nullptr, nullptr);
        return narrow;
    }

    // the page already built the json, this is only the transport
    void post_json(const std::string& json) {
        run_on_ui([json] {
            if (webview)
                webview->PostWebMessageAsJson(to_wide(json).c_str());
        });
    }

    void open_url(const wchar_t* url) {
        ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
    }

    // the startup sound, mci volume runs 0 to 1000
    void set_sound_volume(int volume) {
        mciSendStringW((L"setaudio launcher_sound volume to " + std::to_wstring(volume * 10)).c_str(), nullptr, 0, nullptr);
    }

    void play_startup_sound() {
        fs::path sound = launcher::launcher_dir() / "sounds" / "startup_sound.mp3";
        if (!fs::exists(sound))
            return;

        mciSendStringW(L"close launcher_sound", nullptr, 0, nullptr);
        if (mciSendStringW((L"open \"" + sound.wstring() + L"\" type mpegvideo alias launcher_sound").c_str(), nullptr, 0, nullptr) != 0)
            return;

        set_sound_volume(settings.volume);
        mciSendStringW(L"play launcher_sound", nullptr, 0, nullptr);
    }

    // the page has no real borders, so its title bar and edges hand the mouse to windows' own move and size
    void start_system_drag(int hit) {
        POINT cursor;
        GetCursorPos(&cursor);
        ReleaseCapture();
        SendMessageW(main_window, WM_NCLBUTTONDOWN, hit, MAKELPARAM(cursor.x, cursor.y));
    }

    std::string pick_path(const std::string& title, bool folder) {
        const std::wstring caption = to_wide(title);

        if (folder) {
            BROWSEINFOW browse = {};
            browse.lpszTitle = caption.c_str();
            browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;

            LPITEMIDLIST item = SHBrowseForFolderW(&browse);
            if (!item)
                return {};

            wchar_t path[MAX_PATH] = {};
            const bool ok = SHGetPathFromIDListW(item, path);
            CoTaskMemFree(item);
            return ok ? to_utf8(path) : std::string();
        }

        wchar_t path[MAX_PATH] = {};
        OPENFILENAMEW dialog = { sizeof(dialog) };
        dialog.hwndOwner = main_window;
        dialog.lpstrFilter = L"Mod archives (*.zip)\0*.zip\0All files (*.*)\0*.*\0";
        dialog.lpstrFile = path;
        dialog.nMaxFile = MAX_PATH;
        dialog.lpstrTitle = caption.c_str();
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

        return GetOpenFileNameW(&dialog) ? to_utf8(path) : std::string();
    }

    page::host build_host() {
        page::host callbacks;

        callbacks.open_url = [](const std::string& url) {
            open_url(to_wide(url).c_str());
        };
        callbacks.close_window = [] { DestroyWindow(main_window); };
        callbacks.minimize_window = [] { ShowWindow(main_window, SW_MINIMIZE); };
        callbacks.close_for_update = [] { DestroyWindow(main_window); };
        callbacks.set_sound_volume = [](int volume) { set_sound_volume(volume); };
        callbacks.play_startup_sound = [] { SetTimer(main_window, sound_timer, 1000, nullptr); };

        callbacks.start_drag = [] {
            // the move loop eats the page's clicks, so a double click on the title bar is spotted here
            static DWORD last_time = 0;
            static POINT last_point = {};

            POINT point;
            GetCursorPos(&point);
            DWORD now = GetTickCount();
            bool double_click = now - last_time <= GetDoubleClickTime()
                && std::abs(point.x - last_point.x) <= GetSystemMetrics(SM_CXDOUBLECLK)
                && std::abs(point.y - last_point.y) <= GetSystemMetrics(SM_CYDOUBLECLK);

            last_time = double_click ? 0 : now;
            last_point = point;

            if (double_click)
                ShowWindow(main_window, IsZoomed(main_window) ? SW_RESTORE : SW_MAXIMIZE);
            else
                start_system_drag(HTCAPTION);
        };

        callbacks.start_resize = [](const std::string& edge) {
            static const std::pair<const char*, int> edges[] = {
                { "n", HTTOP }, { "s", HTBOTTOM }, { "e", HTRIGHT }, { "w", HTLEFT },
                { "ne", HTTOPRIGHT }, { "nw", HTTOPLEFT }, { "se", HTBOTTOMRIGHT }, { "sw", HTBOTTOMLEFT },
            };

            for (const auto& [name, hit] : edges) {
                if (edge == name && !IsZoomed(main_window))
                    start_system_drag(hit);
            }
        };

        callbacks.run_async = [](std::function<void()> task) {
            std::thread(std::move(task)).detach();
        };

        callbacks.pick_path = [](const std::string& title, bool folder) {
            return pick_path(title, folder);
        };

        return callbacks;
    }

    // the page and its files are built into the exe, the background art is read from images
    bool open_page_file(const std::wstring& path, ComPtr<IStream>& stream, std::wstring& mime) {
        struct page_file {
            const wchar_t* path;
            const wchar_t* resource;
            const wchar_t* mime;
        };

        static const page_file files[] = {
            { L"/index.html", L"UI_INDEX", L"text/html; charset=utf-8" },
            { L"/style.css", L"UI_STYLE", L"text/css; charset=utf-8" },
            { L"/app.js", L"UI_APP", L"text/javascript; charset=utf-8" },
            { L"/icon.ico", L"UI_ICON", L"image/x-icon" },
        };

        for (const auto& file : files) {
            if (path != file.path)
                continue;

            HRSRC resource = FindResourceW(nullptr, file.resource, MAKEINTRESOURCEW(10));
            HGLOBAL loaded = resource ? LoadResource(nullptr, resource) : nullptr;
            if (!loaded)
                return false;

            auto data = static_cast<const BYTE*>(LockResource(loaded));
            stream.Attach(SHCreateMemStream(data, SizeofResource(nullptr, resource)));
            mime = file.mime;
            return stream != nullptr;
        }

        if (path == L"/launcher.png") {
            fs::path image = launcher::launcher_dir() / "images" / "launcher.png";
            mime = L"image/png";
            return SUCCEEDED(SHCreateStreamOnFileEx(image.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream));
        }

        return false;
    }

    void resize_webview() {
        if (!controller)
            return;

        RECT bounds;
        GetClientRect(main_window, &bounds);
        controller->put_Bounds(bounds);
    }

    void fail(const wchar_t* text) {
        MessageBoxW(main_window, text, L"Project-BO4", MB_ICONERROR | MB_OK);
        DestroyWindow(main_window);
    }

    void setup_webview() {
        // transparent, so the window's splash shows through until the page plays its intro
        ComPtr<ICoreWebView2Controller2> controller2;
        if (SUCCEEDED(controller.As(&controller2)))
            controller2->put_DefaultBackgroundColor({ 0, 0, 0, 0 });

        ComPtr<ICoreWebView2Settings> webview_settings;
        webview->get_Settings(&webview_settings);
        webview_settings->put_IsStatusBarEnabled(FALSE);
        webview_settings->put_IsZoomControlEnabled(FALSE);
#ifndef _DEBUG
        webview_settings->put_AreDefaultContextMenusEnabled(FALSE);
        webview_settings->put_AreDevToolsEnabled(FALSE);

        ComPtr<ICoreWebView2Settings3> settings3;
        if (SUCCEEDED(webview_settings.As(&settings3)))
            settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
#endif

        webview->AddWebResourceRequestedFilter((std::wstring(page_origin) + L"/*").c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        webview->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                ComPtr<ICoreWebView2WebResourceRequest> request;
                args->get_Request(&request);

                LPWSTR uri = nullptr;
                request->get_Uri(&uri);
                std::wstring path = uri ? uri : L"";
                CoTaskMemFree(uri);

                path = path.substr(std::min(path.size(), wcslen(page_origin)));
                path = path.substr(0, path.find_first_of(L"?#"));

                ComPtr<IStream> stream;
                std::wstring mime;
                bool found = open_page_file(path, stream, mime);

                std::wstring headers = found ? L"Content-Type: " + mime + L"\r\nCache-Control: no-store" : L"";
                ComPtr<ICoreWebView2WebResourceResponse> response;
                environment->CreateWebResourceResponse(found ? stream.Get() : nullptr, found ? 200 : 404,
                    found ? L"OK" : L"Not Found", headers.c_str(), &response);
                args->put_Response(response.Get());
                return S_OK;
            }).Get(), nullptr);

        // the page only ever shows our ui, links go to the real browser
        webview->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                LPWSTR uri = nullptr;
                args->get_Uri(&uri);
                std::wstring target = uri ? uri : L"";
                CoTaskMemFree(uri);

                if (target.rfind(std::wstring(page_origin) + L"/", 0) != 0)
                    args->put_Cancel(TRUE);
                return S_OK;
            }).Get(), nullptr);

        webview->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                args->put_Handled(TRUE);
                return S_OK;
            }).Get(), nullptr);

        webview->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                LPWSTR json = nullptr;
                if (SUCCEEDED(args->get_WebMessageAsJson(&json)) && json && page_controller)
                    page_controller->handle(to_utf8(json));
                CoTaskMemFree(json);
                return S_OK;
            }).Get(), nullptr);

        resize_webview();
        webview->Navigate((std::wstring(page_origin) + L"/index.html").c_str());
    }

    void create_webview() {
        // the game can sit in Program Files, so the browser profile goes to local appdata
        wchar_t local_app_data[MAX_PATH] = {};
        GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH);
        fs::path user_data = fs::path(local_app_data) / "ShieldLauncher" / "WebView2";

        HRESULT result = CreateCoreWebView2EnvironmentWithOptions(nullptr, user_data.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [](HRESULT env_result, ICoreWebView2Environment* created_env) -> HRESULT {
                    if (FAILED(env_result) || !created_env) {
                        fail(L"Failed to start WebView2.");
                        return S_OK;
                    }

                    environment = created_env;
                    environment->CreateCoreWebView2Controller(main_window,
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [](HRESULT controller_result, ICoreWebView2Controller* created) -> HRESULT {
                                if (FAILED(controller_result) || !created) {
                                    fail(L"Failed to create the WebView2 control.");
                                    return S_OK;
                                }

                                controller = created;
                                controller->get_CoreWebView2(&webview);
                                setup_webview();
                                return S_OK;
                            }).Get());
                    return S_OK;
                }).Get());

        if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
            fail(L"The Microsoft Edge WebView2 Runtime is not installed.\n\nGet it from https://developer.microsoft.com/microsoft-edge/webview2/");
        else if (FAILED(result))
            fail(L"Failed to start WebView2.");
    }

    LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_SIZE:
            resize_webview();
            if (page_controller)
                page_controller->post_window_state(wparam == SIZE_MAXIMIZED);
            return 0;

        // the thick frame is only kept for sizing and snapping, the whole window is client area
        case WM_NCCALCSIZE:
            if (wparam == TRUE) {
                // maximized windows hang their frame off screen, keep the page inside the monitor
                if (IsZoomed(hwnd)) {
                    auto params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
                    UINT dpi = GetDpiForWindow(hwnd);
                    int padding = GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                    int frame_x = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + padding;
                    int frame_y = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + padding;
                    params->rgrc[0].left += frame_x;
                    params->rgrc[0].right -= frame_x;
                    params->rgrc[0].top += frame_y;
                    params->rgrc[0].bottom -= frame_y;
                }
                return 0;
            }
            break;

        // the page is laid out for 1000x620, it can grow but not shrink below that
        case WM_GETMINMAXINFO: {
            UINT dpi = GetDpiForWindow(hwnd);
            auto info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.x = MulDiv(window_width, dpi, 96);
            info->ptMinTrackSize.y = MulDiv(window_height, dpi, 96);
            return 0;
        }

        case WM_DPICHANGED: {
            auto suggested = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        // the splash, shown from the first frame while WebView2 starts
        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC dc = BeginPaint(hwnd, &paint);
            if (splash_icon) {
                RECT client;
                GetClientRect(hwnd, &client);
                int size = MulDiv(splash_icon_size, GetDpiForWindow(hwnd), 96);
                DrawIconEx(dc, (client.right - size) / 2, (client.bottom - size) / 2, splash_icon, size, size, 0, nullptr, DI_NORMAL);
            }
            EndPaint(hwnd, &paint);
            return 0;
        }

        case run_on_ui_message:
            run_ui_tasks();
            return 0;

        case WM_TIMER:
            KillTimer(hwnd, wparam);
            if (wparam == close_timer)
                DestroyWindow(hwnd);
            else if (wparam == sound_timer)
                play_startup_sound();
            return 0;

        case WM_DESTROY:
            mciSendStringW(L"close launcher_sound", nullptr, 0, nullptr);
            if (controller)
                controller->Close();
            webview = nullptr;
            controller = nullptr;
            environment = nullptr;
            if (splash_icon)
                DestroyIcon(splash_icon);
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
        return 1;

    fs::create_directories(launcher::launcher_dir() / "sounds");
    settings = launcher::load_settings();
    launcher::remove_acts_plugin_once();

    WNDCLASSEXW window_class = { sizeof(window_class) };
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    window_class.hbrBackground = CreateSolidBrush(RGB(13, 15, 18));
    window_class.lpszClassName = L"ShieldLauncher";
    RegisterClassExW(&window_class);

    // the page draws its own title bar
    UINT dpi = GetDpiForSystem();
    int width = MulDiv(window_width, dpi, 96);
    int height = MulDiv(window_height, dpi, 96);
    int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    main_window = CreateWindowExW(0, window_class.lpszClassName, L"Project-BO4",
        WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX, x, y, width, height,
        nullptr, nullptr, instance, nullptr);
    if (!main_window)
        return 1;

    // applies WM_NCCALCSIZE, so the thick frame never shows
    SetWindowPos(main_window, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

    int icon_size = MulDiv(splash_icon_size, GetDpiForWindow(main_window), 96);
    splash_icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON, icon_size, icon_size, LR_DEFAULTCOLOR));

    DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(main_window, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));

    page_controller = std::make_unique<page::controller>(settings, build_host(), post_json);

    ShowWindow(main_window, show);
    UpdateWindow(main_window);

    create_webview();

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessage(&message);
    }

    CoUninitialize();
    return 0;
}
