#include "winhttp_helpers.h"

#include <cctype>
#include <charconv>
#include <climits>

namespace flutter_turbo_fetch {

namespace {

constexpr size_t kUrlStackChars = 512;

bool IsAsciiSpace(wchar_t wc) {
  return wc <= 0x7F && std::isspace(static_cast<unsigned char>(wc)) != 0;
}

std::wstring_view TrimAsciiWideView(std::wstring_view s) {
  size_t first = 0;
  while (first < s.size() && IsAsciiSpace(s[first])) {
    ++first;
  }
  size_t last = s.size();
  while (last > first && IsAsciiSpace(s[last - 1])) {
    --last;
  }
  return s.substr(first, last - first);
}

void AssignLowerAsciiFromWide(std::wstring_view src, std::string* dst) {
  dst->resize(src.size());
  bool is_ascii = true;
  for (size_t i = 0; i < src.size(); ++i) {
    uint16_t wc = static_cast<uint16_t>(src[i]);
    if (wc >= 0x80) {
      is_ascii = false;
      break;
    }
    (*dst)[i] = (wc >= 'A' && wc <= 'Z') ? static_cast<char>(wc + ('a' - 'A'))
                                         : static_cast<char>(wc);
  }
  if (!is_ascii) {
    *dst = WideToUtf8(src);
    for (char& ch : *dst) {
      unsigned char c = static_cast<unsigned char>(ch);
      if (c >= 'A' && c <= 'Z') {
        ch = static_cast<char>(c + ('a' - 'A'));
      }
    }
  }
}

bool ContainsForbiddenHeaderValueChars(std::string_view s) {
  for (unsigned char c : s) {
    if (c == '\r' || c == '\n' || c == '\0') {
      return true;
    }
  }
  return false;
}

bool IsValidHttpMethod(std::string_view method) {
  constexpr std::string_view kDelimiters = "()<>@,;:\\\"/[]?={}";
  if (method.empty()) {
    return false;
  }
  for (unsigned char c : method) {
    if (c <= 0x20 || c >= 0x7f ||
        kDelimiters.find(static_cast<char>(c)) != std::string_view::npos) {
      return false;
    }
  }
  return true;
}

bool ContainsControlChars(std::string_view s) {
  for (unsigned char c : s) {
    if (c < 0x20 || c == 0x7f) {
      return true;
    }
  }
  return false;
}

bool ParseRequestUrl(std::string_view url,
                     ParsedUrl* out_parsed,
                     std::string* out_error_message) {
  if (url.empty()) {
    *out_error_message = "Invalid or empty URL.";
    return false;
  }

  wchar_t url_stack[kUrlStackChars + 1];
  std::wstring url_heap;
  const wchar_t* url_ptr = nullptr;
  size_t url_len = 0;

  if (url.size() <= kUrlStackChars) {
    bool is_ascii = true;
    for (size_t i = 0; i < url.size(); ++i) {
      unsigned char c = static_cast<unsigned char>(url[i]);
      if (c < 0x20 || c == 0x7f) {
        *out_error_message = "URL contains forbidden control characters.";
        return false;
      }
      if (c >= 0x80) {
        is_ascii = false;
        break;
      }
      url_stack[i] = static_cast<wchar_t>(c);
    }
    if (is_ascii) {
      url_stack[url.size()] = L'\0';
      url_ptr = url_stack;
      url_len = url.size();
    }
  }

  if (url_ptr == nullptr) {
    if (ContainsControlChars(url)) {
      *out_error_message = "URL contains forbidden control characters.";
      return false;
    }
    url_heap = Utf8ToWide(url);
    if (url_heap.empty() || url_heap.size() > static_cast<size_t>(MAXDWORD)) {
      *out_error_message = "Invalid or empty URL.";
      return false;
    }
    url_ptr = url_heap.c_str();
    url_len = url_heap.size();
  }

  URL_COMPONENTS url_comp{};
  url_comp.dwStructSize = sizeof(url_comp);
  url_comp.dwSchemeLength = static_cast<DWORD>(-1);
  url_comp.dwHostNameLength = static_cast<DWORD>(-1);
  url_comp.dwUrlPathLength = static_cast<DWORD>(-1);
  url_comp.dwExtraInfoLength = static_cast<DWORD>(-1);

  if (!WinHttpCrackUrl(url_ptr, static_cast<DWORD>(url_len), 0, &url_comp) ||
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

  std::wstring formatted;
  formatted.reserve(headers.size() * 48);
  for (const auto& kv : headers) {
    if (!std::holds_alternative<std::string>(kv.first) ||
        !std::holds_alternative<std::string>(kv.second)) {
      continue;
    }
    const auto& key = std::get<std::string>(kv.first);
    const auto& val = std::get<std::string>(kv.second);
    if (key.empty()) {
      return false;
    }

    for (unsigned char c : key) {
      if (c <= 0x20 || c == ':' || c >= 0x7f) {
        return false;
      }
      formatted.push_back(static_cast<wchar_t>(c));
    }
    formatted.append(L": ");

    const size_t val_start = formatted.size();
    bool val_ascii = true;
    for (unsigned char c : val) {
      if (c == '\r' || c == '\n' || c == '\0') {
        return false;
      }
      if (c >= 0x80) {
        val_ascii = false;
        break;
      }
      formatted.push_back(static_cast<wchar_t>(c));
    }
    if (!val_ascii) {
      if (val.size() > static_cast<size_t>(INT_MAX) ||
          ContainsForbiddenHeaderValueChars(val)) {
        return false;
      }
      formatted.resize(val_start);
      formatted.append(Utf8ToWide(val));
    }
    formatted.append(L"\r\n");
  }

  *out_formatted = std::move(formatted);
  return true;
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

bool ValidateWinHttpRequest(const TurboHttpRequest& request,
                            ValidatedWinHttpRequest* out_validated,
                            std::string* out_error_message) {
  if (!IsValidHttpMethod(request.method())) {
    *out_error_message = "Invalid HTTP method.";
    return false;
  }

  ParsedUrl parsed_url;
  if (!ParseRequestUrl(request.url(), &parsed_url, out_error_message)) {
    return false;
  }

  std::wstring formatted_headers;
  if (!FormatAndValidateHeaders(request.headers(), &formatted_headers)) {
    *out_error_message = "Invalid characters in request header.";
    return false;
  }
  if (formatted_headers.size() > static_cast<size_t>(MAXDWORD)) {
    *out_error_message = "Request headers exceed WinHTTP DWORD size limit.";
    return false;
  }
  if (request.body() != nullptr &&
      request.body()->size() > static_cast<size_t>(MAXDWORD)) {
    *out_error_message = "Request body exceeds WinHTTP DWORD size limit.";
    return false;
  }

  ValidatedWinHttpRequest validated;
  validated.request_id = request.request_id();
  validated.method = request.method();
  validated.parsed_url = std::move(parsed_url);
  validated.formatted_headers = std::move(formatted_headers);
  if (request.body() != nullptr) {
    validated.body = *request.body();
  }
  if (request.connect_timeout_ms() != nullptr) {
    validated.connect_timeout_ms = *request.connect_timeout_ms();
  }
  if (request.send_timeout_ms() != nullptr) {
    validated.send_timeout_ms = *request.send_timeout_ms();
  }
  if (request.receive_timeout_ms() != nullptr) {
    validated.receive_timeout_ms = *request.receive_timeout_ms();
  }
  if (request.max_response_bytes() != nullptr) {
    validated.max_response_bytes = *request.max_response_bytes();
  }
  validated.follow_redirects = request.follow_redirects();
  validated.allow_bad_certificates = request.allow_bad_certificates();

  *out_validated = std::move(validated);
  return true;
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
  std::string key_buf;
  key_buf.reserve(32);

  size_t pos = 0;
  bool is_first_line = true;
  while (pos < raw_headers.size()) {
    size_t end = raw_headers.find(L"\r\n", pos);
    if (end == std::wstring_view::npos) {
      end = raw_headers.size();
    }
    std::wstring_view line = raw_headers.substr(pos, end - pos);
    pos = (end < raw_headers.size()) ? end + 2 : raw_headers.size();

    if (line.empty()) {
      continue;
    }
    if (is_first_line && line.size() >= 5 && line.substr(0, 5) == L"HTTP/") {
      is_first_line = false;
      continue;
    }
    is_first_line = false;

    size_t colon = line.find(L':');
    if (colon == std::wstring_view::npos || colon == 0) {
      continue;
    }

    std::wstring_view key_view = TrimAsciiWideView(line.substr(0, colon));
    if (key_view.empty()) {
      continue;
    }
    std::wstring_view value_view = TrimAsciiWideView(line.substr(colon + 1));

    AssignLowerAsciiFromWide(key_view, &key_buf);
    std::string value_utf8 = WideToUtf8(value_view);

    if (out_content_length != nullptr && key_buf == "content-length" &&
        !value_utf8.empty()) {
      int64_t parsed_len = 0;
      const char* begin = value_utf8.data();
      const char* finish = begin + value_utf8.size();
      auto [ptr, ec] = std::from_chars(begin, finish, parsed_len);
      if (ec == std::errc() && ptr == finish && parsed_len >= 0) {
        *out_content_length = parsed_len;
      }
    }

    auto [it, inserted] = result.try_emplace(
        flutter::EncodableValue(key_buf),
        flutter::EncodableValue(flutter::EncodableList{}));
    std::get<flutter::EncodableList>(it->second)
        .emplace_back(std::move(value_utf8));
  }

  return result;
}

TurboHttpResponse MakeErrorResponse(std::string_view code,
                                    std::string_view message) {
  TurboHttpResponse res;
  res.set_error_code(code);
  res.set_error_message(message);
  return res;
}

TurboHttpResponse MakeCancelResponse() {
  return MakeErrorResponse("cancel", "Request was cancelled.");
}

}  // namespace flutter_turbo_fetch
