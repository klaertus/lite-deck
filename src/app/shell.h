// src/app/shell.h
#pragma once
#include <string>

namespace ld {

// Opens `url` in the default browser, from a short-lived STA thread (ShellExecute wants one; the
// loop thread is MTA). false if `url` is empty. Never logs the URL: the page URL carries the token.
bool open_url(const std::wstring& url);

}  // namespace ld
