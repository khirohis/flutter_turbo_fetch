#include "winhttp_helpers.h"

#include <cctype>
#include <charconv>
#include <climits>

namespace flutter_turbo_fetch {

namespace {

std::string_view TrimAsciiView(std::string_view s) {
  size_t first = 0;
  while (first < s.size() &&
         std::isspace(static_cast<unsigned char>(s[first]))) {
    ++first;
  }
  size_t last = s.size();
  while (last > first &&
         std::isspace(static_cast<unsigned char>(s[last - 1]))) {
    --last;
  }
  return s.substr(first, last - first);
}

void AssignLowerAscii(std::string_view src, std::string* dst) {
  dst->resize(src.size());
  for (size_t i = 0; i < src.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(src[i]);
    (*dst)[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A'))
                                       : static_cast<char>(c);
  }
}

bool ContainsForbiddenHeaderChars(std::string_view s, bool is_name) {
  if (is_name && s.empty()) {
    return true;
  }
  for (unsigned char c : s) {
    if (c == '\r' || c == '\n' || c == '\0') {
      return true;
    }
    if (is_name && (c <= 0x20 || c == ':' || c >= 0x7f)) {
      return true;
    }
  }
  return false;
}

std::wstring_view TrimTrailingNulls(const wchar_t* data,
                                    size_t max_chars,
                                    DWORD bytes) {
  size_t chars = bytes / sizeof(wchar_t);
  if (chars > max_chars) {
    chars = max_chars;
  }
  while (chars > 0 && data[chars - 1] == L'\0') {
    --chars;
  }
  return std::wstring_view(data, chars);
}

}  // namespace

std::wstring Utf8ToWide(std::string_view str) {
  if (str.empty() || str.size() > static_cast<size_t>(INT_MAX)) {
    return std::wstring();
  }

  std::wstring result(str.size(), L'\0');
  bool is_ascii = true;
  for (size_t i = 0; i < str.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(str[i]);
    if (c >= 0x80) {
      is_ascii = false;
      break;
    }
    result[i] = static_cast<wchar_t>(c);
  }
  if (is_ascii) {
    return result;
  }

  int len = MultiByteToWideChar(CP_UTF8, 0, str.data(),
                                static_cast<int>(str.size()), nullptr, 0);
  if (len <= 0) {
    return std::wstring();
  }
  result.resize(static_cast<size_t>(len));
  MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()),
                      result.data(), len);
  return result;
}

std::string WideToUtf8(std::wstring_view wstr) {
  if (wstr.empty() || wstr.size() > static_cast<size_t>(INT_MAX)) {
    return std::string();
  }

  std::string result(wstr.size(), '\0');
  bool is_ascii = true;
  for (size_t i = 0; i < wstr.size(); ++i) {
    uint16_t wc = static_cast<uint16_t>(wstr[i]);
    if (wc >= 0x80) {
      is_ascii = false;
      break;
    }
    result[i] = static_cast<char>(wc);
  }
  if (is_ascii) {
    return result;
  }

  int len = WideCharToMultiByte(CP_UTF8, 0, wstr.data(),
                                static_cast<int>(wstr.size()), nullptr, 0,
                                nullptr, nullptr);
  if (len <= 0) {
    return std::string();
  }
  result.resize(static_cast<size_t>(len));
  WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
                      result.data(), len, nullptr, nullptr);
  return result;
}

bool ParseRequestUrl(std::string_view url,
                     ParsedUrl* out_parsed,
                     std::string* out_error_message) {
  const std::wstring url_w = Utf8ToWide(url);
  if (url_w.empty() || url_w.size() > static_cast<size_t>(MAXDWORD)) {
    *out_error_message = "Invalid or empty URL.";
    return false;
  }

  URL_COMPONENTS url_comp{};
  url_comp.dwStructSize = sizeof(url_comp);
  url_comp.dwSchemeLength = static_cast<DWORD>(-1);
  url_comp.dwHostNameLength = static_cast<DWORD>(-1);
  url_comp.dwUrlPathLength = static_cast<DWORD>(-1);
  url_comp.dwExtraInfoLength = static_cast<DWORD>(-1);

  if (!WinHttpCrackUrl(url_w.c_str(), static_cast<DWORD>(url_w.size()), 0,
                       &url_comp) ||
      url_comp.dwHostNameLength == 0 ||
      (url_comp.nScheme != INTERNET_SCHEME_HTTP &&
       url_comp.nScheme != INTERNET_SCHEME_HTTPS)) {
    *out_error_message =
        "Failed to parse URL or unsupported scheme: " + std::string(url);
    return false;
  }

  out_parsed->is_https = (url_comp.nScheme == INTERNET_SCHEME_HTTPS);
  out_parsed->port = url_comp.nPort;
  out_parsed->host.assign(url_comp.lpszHostName, url_comp.dwHostNameLength);
  if (url_comp.dwUrlPathLength > 0) {
    out_parsed->path_and_query.assign(url_comp.lpszUrlPath,
                                      url_comp.dwUrlPathLength);
  } else {
    out_parsed->path_and_query = L"/";
  }
  if (url_comp.dwExtraInfoLength > 0) {
    out_parsed->path_and_query.append(url_comp.lpszExtraInfo,
                                      url_comp.dwExtraInfoLength);
  }
  return true;
}

bool FormatAndValidateHeaders(const flutter::EncodableMap& headers,
                              std::wstring* out_formatted) {
  if (headers.empty()) {
    out_formatted->clear();
    return true;
  }

  std::string utf8_headers;
  utf8_headers.reserve(headers.size() * 48);
  for (const auto& kv : headers) {
    if (!std::holds_alternative<std::string>(kv.first) ||
        !std::holds_alternative<std::string>(kv.second)) {
      continue;
    }
    const auto& key = std::get<std::string>(kv.first);
    const auto& val = std::get<std::string>(kv.second);
    if (ContainsForbiddenHeaderChars(key, /*is_name=*/true) ||
        ContainsForbiddenHeaderChars(val, /*is_name=*/false)) {
      return false;
    }
    utf8_headers.append(key);
    utf8_headers.append(": ");
    utf8_headers.append(val);
    utf8_headers.append("\r\n");
  }
  *out_formatted = Utf8ToWide(utf8_headers);
  return true;
}

LPCWSTR ResolveMethodWide(std::string_view method,
                          std::wstring* custom_storage) {
  if (method == "GET") return L"GET";
  if (method == "POST") return L"POST";
  if (method == "PUT") return L"PUT";
  if (method == "DELETE") return L"DELETE";
  if (method == "PATCH") return L"PATCH";
  if (method == "HEAD") return L"HEAD";
  if (method == "OPTIONS") return L"OPTIONS";
  *custom_storage = Utf8ToWide(method);
  return custom_storage->c_str();
}

int ClampTimeoutMs(std::optional<int64_t> timeout_opt, int default_ms) {
  if (!timeout_opt.has_value()) {
    return default_ms;
  }
  int64_t val = *timeout_opt;
  if (val <= 0) {
    return 0;
  }
  if (val > static_cast<int64_t>(INT_MAX)) {
    return INT_MAX;
  }
  return static_cast<int>(val);
}

std::string ClassifyWinHttpError(bool is_cancelled,
                                 DWORD win32_error,
                                 RequestPhase phase) {
  if (is_cancelled || win32_error == ERROR_WINHTTP_OPERATION_CANCELLED ||
      win32_error == ERROR_INVALID_HANDLE) {
    return "cancel";
  }

  if (win32_error == ERROR_WINHTTP_TIMEOUT) {
    switch (phase) {
      case RequestPhase::kConnecting:
        return "connectionTimeout";
      case RequestPhase::kSendingBody:
        return "sendTimeout";
      case RequestPhase::kReceiving:
        return "receiveTimeout";
    }
  }

  switch (win32_error) {
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
    case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
    case ERROR_WINHTTP_SECURE_INVALID_CERT:
    case ERROR_WINHTTP_SECURE_CERT_REVOKED:
    case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
    case static_cast<DWORD>(SEC_E_UNTRUSTED_ROOT):
    case static_cast<DWORD>(SEC_E_CERT_EXPIRED):
    case static_cast<DWORD>(SEC_E_WRONG_PRINCIPAL):
    case static_cast<DWORD>(SEC_E_ILLEGAL_MESSAGE):
    case static_cast<DWORD>(SEC_E_INVALID_TOKEN):
      return "badCertificate";

    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR:
    case ERROR_WINHTTP_INVALID_URL:
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
    case ERROR_WINHTTP_REDIRECT_FAILED:
    case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:
    case ERROR_WINHTTP_HEADER_NOT_FOUND:
    case ERROR_INVALID_PARAMETER:
      return "connectionError";

    default:
      return "unknown";
  }
}

flutter::EncodableMap ParseRawHttpHeaders(
    std::wstring_view raw_headers,
    std::optional<int64_t>* out_content_length) {
  if (out_content_length != nullptr) {
    *out_content_length = std::nullopt;
  }

  flutter::EncodableMap result;
  const std::string utf8_raw = WideToUtf8(raw_headers);
  const std::string_view raw_view(utf8_raw);

  std::string key_buf;
  key_buf.reserve(32);

  size_t pos = 0;
  bool is_first_line = true;
  while (pos < raw_view.size()) {
    size_t end = raw_view.find("\r\n", pos);
    if (end == std::string_view::npos) {
      end = raw_view.size();
    }
    std::string_view line = raw_view.substr(pos, end - pos);
    pos = (end < raw_view.size()) ? end + 2 : raw_view.size();

    if (line.empty()) {
      continue;
    }
    if (is_first_line && line.size() >= 5 && line.substr(0, 5) == "HTTP/") {
      is_first_line = false;
      continue;
    }
    is_first_line = false;

    size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) {
      continue;
    }

    std::string_view key_view = TrimAsciiView(line.substr(0, colon));
    if (key_view.empty()) {
      continue;
    }
    std::string_view value_view = TrimAsciiView(line.substr(colon + 1));

    AssignLowerAscii(key_view, &key_buf);

    auto [it, inserted] = result.try_emplace(
        flutter::EncodableValue(key_buf),
        flutter::EncodableValue(flutter::EncodableList{}));
    std::get<flutter::EncodableList>(it->second)
        .emplace_back(std::string(value_view));

    if (out_content_length != nullptr && key_buf == "content-length" &&
        !value_view.empty()) {
      int64_t parsed_len = 0;
      const char* begin = value_view.data();
      const char* finish = begin + value_view.size();
      auto [ptr, ec] = std::from_chars(begin, finish, parsed_len);
      if (ec == std::errc() && ptr == finish && parsed_len >= 0) {
        *out_content_length = parsed_len;
      }
    }
  }

  return result;
}

std::wstring_view QueryWinHttpHeaderString(HINTERNET h_request,
                                           DWORD info_level,
                                           wchar_t* stack_buf,
                                           DWORD stack_buf_bytes,
                                           std::wstring* heap_fallback) {
  DWORD bytes = stack_buf_bytes;
  if (WinHttpQueryHeaders(h_request, info_level, WINHTTP_HEADER_NAME_BY_INDEX,
                          stack_buf, &bytes, WINHTTP_NO_HEADER_INDEX)) {
    return TrimTrailingNulls(stack_buf, stack_buf_bytes / sizeof(wchar_t),
                             bytes);
  }

  if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && bytes > 0) {
    heap_fallback->resize((bytes + sizeof(wchar_t) - 1) / sizeof(wchar_t),
                          L'\0');
    if (WinHttpQueryHeaders(h_request, info_level, WINHTTP_HEADER_NAME_BY_INDEX,
                            heap_fallback->data(), &bytes,
                            WINHTTP_NO_HEADER_INDEX)) {
      return TrimTrailingNulls(heap_fallback->data(), heap_fallback->size(),
                               bytes);
    }
  }

  return std::wstring_view();
}

TurboHttpResponse MakeErrorResponse(std::string_view code,
                                    std::string_view message) {
  TurboHttpResponse res;
  res.set_error_code(code);
  res.set_error_message(message);
  return res;
}

}  // namespace flutter_turbo_fetch
