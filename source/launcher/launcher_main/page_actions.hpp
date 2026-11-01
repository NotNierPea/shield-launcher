#pragma once
#include <functional>
#include <mutex>
#include <string>

#include <rapidjson/document.h>

#include "launcher_actions.hpp"

// The launcher page talks to the C++ side with JSON messages: the page sends
// {"action": ...} and gets {"type": ...} back. Everything that is not window
// plumbing lives here so the Windows host (WebView2) and the Linux one
// (Qt WebEngine) answer the same page the same way.
namespace page {
    // what the window does for the page, a host fills in what it can do
    struct host {
        std::function<void(const std::string& url)> open_url;
        std::function<void()> close_window;
        std::function<void()> minimize_window;
        std::function<void()> start_drag;
        std::function<void(const std::string& edge)> start_resize;
        std::function<void()> close_for_update;
        std::function<void(int volume)> set_sound_volume;
        std::function<void()> play_startup_sound;

        // starts work off the ui thread
        std::function<void(std::function<void()>)> run_async;

        // a native file dialog, returns "" when it is cancelled
        std::function<std::string(const std::string& title, bool folder)> pick_path;
    };

    class controller {
    public:
        // send must be callable from any thread, the hosts marshal to their ui thread
        controller(launcher::settings settings, host callbacks, std::function<void(const std::string&)> send);

        // one message from the page, called on the ui thread
        void handle(const std::string& json);

        void post_init();
        void post_error(const std::string& message);
        void post_window_state(bool maximized);

    private:
        void post_name();
        void post_client_config();
        void post_dedicated_settings();
        void post_installed_languages();
        void post_mods();
        void post_linux();
        void check_for_updates();
        void start_update();
        void start_play(launcher::play_mode mode);
        void handle_mods(const std::string& action, const rapidjson::Value& message);
#ifndef _WIN32
        void handle_linux(const std::string& action, const rapidjson::Value& message);
#endif
        void install_mod(const std::string& url, const std::string& title);
        void post_progress(int value, const std::string& text);

        template <typename fill_type>
        void post(const char* type, const fill_type& fill);

        launcher::settings settings_;
        host host_;
        std::function<void(const std::string&)> send_;

        std::mutex send_mutex_;
        bool playing_ = false;
        bool updating_ = false;
        bool mods_busy_ = false;
    };
}
