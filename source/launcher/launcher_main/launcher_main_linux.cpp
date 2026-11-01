#include "std_include.hpp"
#include "launcher_actions.hpp"
#include "page_actions.hpp"

// Linux host of the launcher page.
//
// The Windows build shows the page in WebView2. There is no WebView2 here, so
// WebKitGTK hosts the exact same ui/ files and the exact same JSON protocol: a
// shim fills in window.chrome.webview before the page's own scripts run, so
// app.js does not know which host it is talking to.

#include <gtk/gtk.h>
#include <jsc/jsc.h>
#include <webkit2/webkit2.h>

#include <cstdio>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace {
    const char* page_url = "shield://page/index.html";

    GtkWidget* window = nullptr;
    WebKitWebView* view = nullptr;
    page::controller* controller = nullptr;

    bool verbose() {
        return std::getenv("SHIELD_LAUNCHER_DEBUG") != nullptr;
    }

    std::string resource_path_for(const std::string& path) {
        if (path == "/index.html")
            return "/ui/index.html";
        if (path == "/style.css")
            return "/ui/style.css";
        if (path == "/app.js")
            return "/ui/app.js";
        if (path == "/icon.ico")
            return "/ui/icon.ico";
        return {};
    }

    std::string content_type_for(const std::string& path) {
        if (path == "/index.html")
            return "text/html; charset=utf-8";
        if (path == "/style.css")
            return "text/css; charset=utf-8";
        if (path == "/app.js")
            return "text/javascript; charset=utf-8";
        return "image/png";
    }

    // reads a built in file, null when the resource is not there
    GBytes* load_resource(const std::string& path) {
        GError* error = nullptr;
        GBytes* bytes = g_resources_lookup_data(path.c_str(), G_RESOURCE_LOOKUP_FLAGS_NONE, &error);
        if (!bytes) {
            if (error) {
                std::cout << "[resource] " << path << ": " << error->message << std::endl;
                g_error_free(error);
            }
            return nullptr;
        }

        return bytes;
    }

    // reads a file from the launcher folder, the art is shipped next to the exe
    GBytes* load_file(const fs::path& path) {
        gchar* content = nullptr;
        gsize size = 0;
        if (!g_file_get_contents(path.c_str(), &content, &size, nullptr)) {
            if (verbose())
                std::cout << "[file] cannot read " << path.string() << std::endl;
            return nullptr;
        }

        GBytes* bytes = g_bytes_new_take(content, size);
        return bytes;
    }

    void finish_with(WebKitURISchemeRequest* request, GBytes* bytes, const std::string& content_type) {
        gsize size = 0;
        const void* data = g_bytes_get_data(bytes, &size);

        GInputStream* stream = g_memory_input_stream_new_from_data(g_memdup2(data, size), size, g_free);
        WebKitURISchemeResponse* response = webkit_uri_scheme_response_new(stream, static_cast<gint64>(size));
        webkit_uri_scheme_response_set_content_type(response, content_type.c_str());
        webkit_uri_scheme_request_finish_with_response(request, response);

        g_object_unref(response);
        g_object_unref(stream);
        g_bytes_unref(bytes);
    }

    void fail_with(WebKitURISchemeRequest* request, const std::string& what) {
        GError* error = g_error_new(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s", what.c_str());
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
    }

    // shield://page/index.html, the same origin the Windows host serves
    std::string request_path(WebKitURISchemeRequest* request) {
        const char* uri = webkit_uri_scheme_request_get_uri(request);
        std::string text = uri ? uri : "";
        const std::string prefix = "shield://page";

        if (text.rfind(prefix, 0) == 0)
            text = text.substr(prefix.size());
        else if (const char* path = webkit_uri_scheme_request_get_path(request))
            text = path;

        const size_t query = text.find_first_of("?#");
        if (query != std::string::npos)
            text = text.substr(0, query);

        return text.empty() ? "/" : text;
    }

    void on_scheme_request(WebKitURISchemeRequest* request, gpointer) {
        const std::string path = request_path(request);

        if (verbose())
            std::cout << "[scheme] " << path << std::endl;

        if (const std::string resource = resource_path_for(path); !resource.empty()) {
            if (GBytes* bytes = load_resource(resource)) {
                if (verbose())
                    std::cout << "[scheme] served " << g_bytes_get_size(bytes) << " bytes of " << path << std::endl;
                finish_with(request, bytes, content_type_for(path));
                return;
            }
            fail_with(request, path);
            return;
        }

        // the background art next to the launcher, png or jpg
        if (path == "/launcher.png" || path == "/launcher.jpg") {
            const fs::path images = launcher::launcher_dir() / "images";
            for (const char* name : { "launcher.png", "launcher.jpg" }) {
                if (GBytes* bytes = load_file(images / name)) {
                    if (verbose())
                        std::cout << "[scheme] served " << g_bytes_get_size(bytes) << " bytes of " << (images / name).string() << std::endl;
                    finish_with(request, bytes, std::string(name).ends_with("png") ? "image/png" : "image/jpeg");
                    return;
                }
            }
        }

        fail_with(request, path);
    }

    // window.chrome.webview over window.webkit.messageHandlers.shield
    const char* bridge_shim = R"js(
(function () {
  if (window.chrome && window.chrome.webview) return;
  if (!window.webkit || !window.webkit.messageHandlers || !window.webkit.messageHandlers.shield) return;

  const listeners = [];
  const queue = [];

  window.chrome = window.chrome || {};
  window.chrome.webview = {
    postMessage: function (message) {
      window.webkit.messageHandlers.shield.postMessage(JSON.stringify(message));
    },
    addEventListener: function (type, callback) {
      if (type === "message" && listeners.indexOf(callback) < 0) listeners.push(callback);
    },
    removeEventListener: function (type, callback) {
      const at = listeners.indexOf(callback);
      if (type === "message" && at >= 0) listeners.splice(at, 1);
    },
    hostObjects: {}
  };

  window.__shieldDispatch = function (json) {
    const data = JSON.parse(json);
    for (const listener of listeners.slice()) listener({ type: "message", data: data });
  };

  while (queue.length) window.chrome.webview.postMessage(queue.shift());
})();
)js";

    // percent encoded, so no quote or newline of the payload can break the js string
    gchar* dispatch_script(const std::string& json) {
        gchar* encoded = g_uri_escape_string(json.c_str(), nullptr, TRUE);
        gchar* script = g_strdup_printf("(window.__shieldDispatch || function () {})(decodeURIComponent('%s'))", encoded);
        g_free(encoded);
        return script;
    }

    struct dispatch_call {
        WebKitWebView* view;
        char* script;
    };

    gboolean run_dispatch(gpointer data) {
        auto* call = static_cast<dispatch_call*>(data);
        if (call->view) {
            webkit_web_view_evaluate_javascript(call->view, call->script, -1, nullptr, nullptr, nullptr, nullptr, nullptr);
            g_object_unref(call->view);
        }
        g_free(call->script);
        delete call;
        return G_SOURCE_REMOVE;
    }

    // called from any thread, the script runs on the ui one
    void post_json(const std::string& json) {
        if (verbose())
            std::cout << "[-> page] " << json << std::endl;

        auto* call = new dispatch_call{ view, dispatch_script(json) };
        if (call->view)
            g_object_ref(call->view);
        g_main_context_invoke(nullptr, run_dispatch, call);
    }

    void on_script_message(WebKitUserContentManager*, WebKitJavascriptResult* result, gpointer) {
        JSCValue* value = webkit_javascript_result_get_js_value(result);
        if (!jsc_value_is_string(value))
            return;

        char* text = jsc_value_to_string(value);
        if (text) {
            if (verbose())
                std::cout << "[page ->] " << text << std::endl;
            if (controller)
                controller->handle(text);
            g_free(text);
        }
    }

    void open_url(const std::string& url) {
        gtk_show_uri_on_window(GTK_WINDOW(window), url.c_str(), GDK_CURRENT_TIME, nullptr);
    }

    // where the pointer is, gtk wants it for its own move and resize loops
    void pointer_position(gint& x, gint& y) {
        x = 0;
        y = 0;

        GdkSeat* seat = gdk_display_get_default_seat(gdk_display_get_default());
        GdkDevice* pointer = seat ? gdk_seat_get_pointer(seat) : nullptr;
        if (pointer)
            gdk_device_get_position(pointer, nullptr, &x, &y);
    }

    void start_drag() {
        gint x = 0;
        gint y = 0;
        pointer_position(x, y);

        // gtk's move loop eats the page's clicks, so a double click is spotted here
        static gint64 last_time = 0;
        static gint last_x = 0;
        static gint last_y = 0;

        const gint64 now = g_get_monotonic_time();
        const bool double_click = last_time != 0 && now - last_time < 400000
            && std::abs(x - last_x) < 8 && std::abs(y - last_y) < 8;

        last_time = double_click ? 0 : now;
        last_x = x;
        last_y = y;

        if (double_click) {
            if (gtk_window_is_maximized(GTK_WINDOW(window)))
                gtk_window_unmaximize(GTK_WINDOW(window));
            else
                gtk_window_maximize(GTK_WINDOW(window));
            return;
        }

        gtk_window_begin_move_drag(GTK_WINDOW(window), 1, x, y, GDK_CURRENT_TIME);
    }

    void start_resize(const std::string& edge) {
        if (gtk_window_is_maximized(GTK_WINDOW(window)))
            return;

        GdkWindowEdge grip = GDK_WINDOW_EDGE_SOUTH_EAST;
        if (edge == "n")
            grip = GDK_WINDOW_EDGE_NORTH;
        else if (edge == "s")
            grip = GDK_WINDOW_EDGE_SOUTH;
        else if (edge == "e")
            grip = GDK_WINDOW_EDGE_EAST;
        else if (edge == "w")
            grip = GDK_WINDOW_EDGE_WEST;
        else if (edge == "ne")
            grip = GDK_WINDOW_EDGE_NORTH_EAST;
        else if (edge == "nw")
            grip = GDK_WINDOW_EDGE_NORTH_WEST;
        else if (edge == "sw")
            grip = GDK_WINDOW_EDGE_SOUTH_WEST;

        gint x = 0;
        gint y = 0;
        pointer_position(x, y);
        gtk_window_begin_resize_drag(GTK_WINDOW(window), grip, 1, x, y, GDK_CURRENT_TIME);
    }

    void play_startup_sound() {
        const fs::path sounds = launcher::launcher_dir() / "sounds";
        const char* names[] = { "startup_sound.mp3", "startup_sound.ogg" };

        for (const char* name : names) {
            const fs::path file = sounds / name;
            if (!fs::exists(file))
                continue;

            // no audio dependency in the launcher, a player on the machine does it
            const std::string quoted = "'" + file.string() + "'";
            const char* players[] = {
                "mpv --really-quiet --no-video --no-terminal ",
                "ffplay -nodisp -autoexit -loglevel quiet ",
                "paplay ",
            };

            for (const char* player : players) {
                const std::string command = std::string(player) + quoted + " >/dev/null 2>&1 &";
                if (std::system(command.c_str()) == 0)
                    return;
            }
            return;
        }
    }

    std::string pick_path(const std::string& title, bool folder) {
        GtkWidget* dialog = gtk_file_chooser_dialog_new(title.c_str(), GTK_WINDOW(window),
            folder ? GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER : GTK_FILE_CHOOSER_ACTION_OPEN,
            "Cancel", GTK_RESPONSE_CANCEL, "Select", GTK_RESPONSE_ACCEPT, nullptr);

        if (!folder) {
            GtkFileFilter* filter = gtk_file_filter_new();
            gtk_file_filter_set_name(filter, "Mod archives (*.zip)");
            gtk_file_filter_add_pattern(filter, "*.zip");
            gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
        }

        std::string result;
        if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
            char* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
            if (path) {
                result = path;
                g_free(path);
            }
        }

        gtk_widget_destroy(dialog);
        return result;
    }

    void on_window_state(GtkWidget*, GdkEventWindowState* event, gpointer) {
        if (!controller)
            return;
        if (event->changed_mask & GDK_WINDOW_STATE_MAXIMIZED)
            controller->post_window_state((event->new_window_state & GDK_WINDOW_STATE_MAXIMIZED) != 0);
    }

    gboolean on_delete(GtkWidget*, GdkEvent*, gpointer) {
        gtk_main_quit();
        return FALSE;
    }

    page::host build_host() {
        page::host callbacks;

        callbacks.open_url = [](const std::string& url) { open_url(url); };
        callbacks.close_window = [] { gtk_widget_destroy(window); };
        callbacks.close_for_update = [] { gtk_widget_destroy(window); };
        callbacks.minimize_window = [] { gtk_window_iconify(GTK_WINDOW(window)); };
        callbacks.start_drag = [] { start_drag(); };
        callbacks.start_resize = [](const std::string& edge) { start_resize(edge); };
        // the launcher has no audio stack of its own, the volume is saved for the game
        callbacks.set_sound_volume = [](int) {};
        callbacks.play_startup_sound = [] { play_startup_sound(); };
        callbacks.run_async = [](std::function<void()> task) {
            std::thread(std::move(task)).detach();
        };
        callbacks.pick_path = [](const std::string& title, bool folder) { return pick_path(title, folder); };

        return callbacks;
    }

    void install_bridge() {
        WebKitUserContentManager* manager = webkit_web_view_get_user_content_manager(view);

        webkit_user_content_manager_register_script_message_handler(manager, "shield");
        g_signal_connect(manager, "script-message-received::shield", G_CALLBACK(on_script_message), nullptr);

        WebKitUserScript* script = webkit_user_script_new(bridge_shim,
            WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
        webkit_user_content_manager_add_script(manager, script);
        webkit_user_script_unref(script);
    }

    void setup_scheme(WebKitWebContext* context) {
        webkit_web_context_register_uri_scheme(context, "shield", on_scheme_request, nullptr, nullptr);

        // our own origin: secure and cors enabled, the page fetches its own files
        WebKitSecurityManager* security = webkit_web_context_get_security_manager(context);
        webkit_security_manager_register_uri_scheme_as_secure(security, "shield");
        webkit_security_manager_register_uri_scheme_as_cors_enabled(security, "shield");
        webkit_security_manager_register_uri_scheme_as_local(security, "shield");
    }
}

int main(int argc, char** argv) {
    if (!gtk_init_check(&argc, &argv)) {
        std::cout << "Could not open a window, is a display running?" << std::endl;
        return 1;
    }

    std::error_code ec;
    fs::create_directories(launcher::launcher_dir() / "sounds", ec);
    launcher::remove_acts_plugin_once();

    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Project-BO4");
    gtk_window_set_default_size(GTK_WINDOW(window), 1000, 620);
    // the page is laid out for 1000x620, it can grow but not shrink below that
    GdkGeometry geometry = {};
    geometry.min_width = 1000;
    geometry.min_height = 620;
    gtk_window_set_geometry_hints(GTK_WINDOW(window), nullptr, &geometry, GDK_HINT_MIN_SIZE);
    // The page draws its own title bar, with the drag, the resize grips and the close button.
    // decorated(FALSE) alone only removes it on X11: on wayland gtk falls back to whatever the
    // compositor draws. An empty header bar is what makes gtk negotiate client side decorations,
    // and the css below collapses it to nothing.
    GtkWidget* header = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), FALSE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(header), "Project-BO4");
    gtk_window_set_titlebar(GTK_WINDOW(window), header);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);

    GtkCssProvider* chrome_css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(chrome_css,
        "headerbar, headerbar:backdrop { min-height: 0; padding: 0; margin: 0;"
        " border: none; box-shadow: none; background: transparent; }"
        " headerbar > * { opacity: 0; min-height: 0; }", -1, nullptr);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(chrome_css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(chrome_css);

    WebKitWebContext* context = webkit_web_context_new();
    setup_scheme(context);

    view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
        "web-context", context,
        "expand", TRUE,
        nullptr));
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(view));

    install_bridge();

    WebKitSettings* settings = webkit_web_view_get_settings(view);
    webkit_settings_set_enable_developer_extras(settings, verbose());
    webkit_settings_set_enable_write_console_messages_to_stdout(settings, verbose());
    webkit_settings_set_javascript_can_open_windows_automatically(settings, FALSE);

    launcher::settings values = launcher::load_settings();
    controller = new page::controller(values, build_host(), post_json);

    g_signal_connect(window, "delete-event", G_CALLBACK(on_delete), nullptr);
    g_signal_connect(window, "window-state-event", G_CALLBACK(on_window_state), nullptr);

    gtk_widget_show_all(window);
    webkit_web_view_load_uri(view, page_url);

    // the build check can drive the page: SHIELD_LAUNCHER_DEBUG_JS sends one message as the ui does
    if (const char* code = std::getenv("SHIELD_LAUNCHER_DEBUG_JS")) {
        auto* call = new dispatch_call{ view, g_strdup(code) };
        g_object_ref(view);
        g_timeout_add(2000, [](gpointer data) -> gboolean {
            auto* payload = static_cast<dispatch_call*>(data);
            webkit_web_view_evaluate_javascript(payload->view, payload->script, -1, nullptr, nullptr, nullptr, nullptr, nullptr);
            g_free(payload->script);
            g_object_unref(payload->view);
            delete payload;
            return G_SOURCE_REMOVE;
        }, call);
    }

    // the build check can also picture the page, whatever the window manager does with the window
    if (const char* path = std::getenv("SHIELD_LAUNCHER_SNAPSHOT")) {
        struct snapshot_call {
            char* path;
        };

        g_timeout_add(3000, [](gpointer data) -> gboolean {
            auto* payload = static_cast<snapshot_call*>(data);
            webkit_web_view_get_snapshot(view, WEBKIT_SNAPSHOT_REGION_VISIBLE, WEBKIT_SNAPSHOT_OPTIONS_NONE,
                nullptr, [](GObject* object, GAsyncResult* result, gpointer user) {
                    auto* call = static_cast<snapshot_call*>(user);
                    GError* error = nullptr;
                    cairo_surface_t* surface = webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(object), result, &error);
                    if (!surface) {
                        std::cout << "[snapshot] failed: " << (error ? error->message : "unknown") << std::endl;
                        if (error)
                            g_error_free(error);
                    }
                    else {
                        cairo_surface_write_to_png(surface, call->path);
                        std::cout << "[snapshot] " << call->path << std::endl;
                        cairo_surface_destroy(surface);
                    }
                    g_free(call->path);
                    delete call;
                }, payload);

            return G_SOURCE_REMOVE;
        }, new snapshot_call{ g_strdup(path) });
    }

    // a fixed run for the build check: load the page, talk to it, leave
    if (const char* seconds = std::getenv("SHIELD_LAUNCHER_SELFTEST")) {
        g_timeout_add(static_cast<guint>(std::atoi(seconds)) * 1000, [](gpointer) -> gboolean {
            gtk_main_quit();
            return G_SOURCE_REMOVE;
        }, nullptr);
    }

    gtk_main();

    if (controller) {
        delete controller;
        controller = nullptr;
    }

    return 0;
}
