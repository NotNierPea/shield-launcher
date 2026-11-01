#pragma once
#include <string>

// HTTPS download that does not depend on a TLS backend compiled into the launcher.
//
// The launcher ships no network stack of its own: curl does the transfer (present
// on Windows 10+ and on every desktop Linux), with wget and PowerShell as the
// fallbacks. Errors come back as text meant for the page.
namespace webdl {
    // Downloads url into destPath, overwriting it. Returns an empty string on
    // success, a readable error otherwise.
    std::string download(const std::string& url, const std::string& destPath);

    // Reads a small URL into memory, used for the GitHub release check.
    std::string fetch(const std::string& url, std::string& error);
}
