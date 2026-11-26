#ifndef FLUTTER_PLUGIN_WINHTTP_HELPERS_H_
#define FLUTTER_PLUGIN_WINHTTP_HELPERS_H_

#include <windows.h>
#include <winhttp.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "messages.g.h"

namespace flutter_turbo_fetch {

enum class RequestPhase {
  kConnecting,
  kSendingBody,
  kReceiving,
};

struct ParsedUrl {
  bool is_https = false;
  INTERNET_PORT port = 0;
  std::wstring host;
  std::wstring path_and_query;
};

struct ValidatedWinHttpRequest {
  std::string request_id;
  std::string method;
  ParsedUrl parsed_url;
  std::wstring formatted_headers;
  std::optional<std::vector<uint8_t>> body;
  std::optional<int64_t> connect_timeout_ms;
  std::optional<int64_t> send_timeout_ms;
  std::optional<int64_t> receive_timeout_ms;
  std::optional<int64_t> max_response_bytes;
  bool follow_redirects = true;
  bool allow_bad_certificates = false;
};

// Pure UTF-8 <-> UTF-16 conversion helpers (ASCII fast path + Win32 fallback).
std::wstring Utf8ToWide(std::string_view str);
std::string WideToUtf8(std::wstring_view wstr);

// Validates and normalizes an incoming Pigeon request before dispatch.
bool ValidateWinHttpRequest(const TurboHttpRequest& request,
                            ValidatedWinHttpRequest* out_validated,
                            std::string* out_error_message);

// Maps Win32/WinHTTP errors and request phases to unified turbo-fetch codes.
std::string ClassifyWinHttpError(bool is_cancelled,
                                 DWORD win32_error,
                                 RequestPhase phase);

// Parses CRLF-delimited raw HTTP response headers into a lowercase-keyed
// multi-value EncodableMap, optionally extracting Content-Length.
flutter::EncodableMap ParseRawHttpHeaders(
    std::wstring_view raw_headers,
    std::optional<int64_t>* out_content_length);

// Response constructors for error and cancellation outcomes.
TurboHttpResponse MakeErrorResponse(std::string_view code,
                                    std::string_view message);
TurboHttpResponse MakeCancelResponse();

}  // namespace flutter_turbo_fetch

#endif  // FLUTTER_PLUGIN_WINHTTP_HELPERS_H_
