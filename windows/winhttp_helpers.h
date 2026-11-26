#ifndef FLUTTER_PLUGIN_WINHTTP_HELPERS_H_
#define FLUTTER_PLUGIN_WINHTTP_HELPERS_H_

#include <windows.h>
#include <winhttp.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

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

std::wstring Utf8ToWide(std::string_view str);
std::string WideToUtf8(std::wstring_view wstr);

bool ParseRequestUrl(std::string_view url,
                     ParsedUrl* out_parsed,
                     std::string* out_error_message);

bool FormatAndValidateHeaders(const flutter::EncodableMap& headers,
                              std::wstring* out_formatted);

LPCWSTR ResolveMethodWide(std::string_view method,
                          std::wstring* custom_storage);

int ClampTimeoutMs(std::optional<int64_t> timeout_opt, int default_ms);

std::string ClassifyWinHttpError(bool is_cancelled,
                                 DWORD win32_error,
                                 RequestPhase phase);

flutter::EncodableMap ParseRawHttpHeaders(
    std::wstring_view raw_headers,
    std::optional<int64_t>* out_content_length);

std::wstring_view QueryWinHttpHeaderString(HINTERNET h_request,
                                           DWORD info_level,
                                           wchar_t* stack_buf,
                                           DWORD stack_buf_bytes,
                                           std::wstring* heap_fallback);

TurboHttpResponse MakeErrorResponse(std::string_view code,
                                    std::string_view message);

}  // namespace flutter_turbo_fetch

#endif  // FLUTTER_PLUGIN_WINHTTP_HELPERS_H_
