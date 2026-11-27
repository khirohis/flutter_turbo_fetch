#include "curl_helpers.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <climits>

namespace flutter_turbo_fetch {

namespace {

bool IsAsciiSpace(char ch) {
  const unsigned char c = static_cast<unsigned char>(ch);
  return c <= 0x7f && std::isspace(c) != 0;
}

bool ContainsControlChars(std::string_view s) {
  for (unsigned char c : s) {
    if (c < 0x20 || c == 0x7f) {
      return true;
    }
  }
  return false;
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

bool IsValidHeaderName(std::string_view name) {
  if (name.empty()) {
    return false;
  }
  for (unsigned char c : name) {
    if (c <= 0x20 || c == ':' || c >= 0x7f) {
      return false;
    }
  }
  return true;
}

bool IsValidPort(std::string_view port_str) {
  if (port_str.empty()) {
    return false;
  }
  for (unsigned char c : port_str) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  int port = 0;
  const char* begin = port_str.data();
  const char* finish = begin + port_str.size();
  auto [ptr, ec] = std::from_chars(begin, finish, port);
  return ec == std::errc() && ptr == finish && port >= 1 && port <= 65535;
}

bool ValidateHttpUrl(std::string_view url, std::string* out_error_message) {
  if (url.empty()) {
    *out_error_message = "Invalid or empty URL.";
    return false;
  }
  if (ContainsControlChars(url)) {
    *out_error_message = "URL contains forbidden control characters.";
    return false;
  }

  size_t scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos) {
    *out_error_message = "Invalid URL: missing scheme.";
    return false;
  }

  std::string_view scheme = url.substr(0, scheme_end);
  if (!EqualsIgnoreCaseAscii(scheme, "http") &&
      !EqualsIgnoreCaseAscii(scheme, "https")) {
    *out_error_message = "Unsupported URL scheme: " + std::string(scheme);
    return false;
  }

  std::string_view rest = url.substr(scheme_end + 3);
  size_t authority_end = rest.find_first_of("/?#");
  std::string_view authority =
      (authority_end == std::string_view::npos) ? rest : rest.substr(0, authority_end);
  if (authority.empty()) {
    *out_error_message = "Invalid URL: missing host.";
    return false;
  }

  // Reject whitespace inside authority (host/port/userinfo).
  for (unsigned char c : authority) {
    if (c <= 0x20 || c == 0x7f) {
      *out_error_message = "Invalid URL: authority contains whitespace.";
      return false;
    }
  }

  // Strip optional userinfo ("user:pass@").
  size_t at_pos = authority.rfind('@');
  std::string_view hostport =
      (at_pos == std::string_view::npos) ? authority : authority.substr(at_pos + 1);
  if (hostport.empty()) {
    *out_error_message = "Invalid URL: missing host.";
    return false;
  }

  if (hostport.front() == '[') {
    // IPv6 literal: [::1] or [::1]:8080
    size_t close_bracket = hostport.find(']');
    if (close_bracket == std::string_view::npos || close_bracket <= 1) {
      *out_error_message = "Invalid URL: malformed IPv6 host.";
      return false;
    }
    std::string_view suffix = hostport.substr(close_bracket + 1);
    if (!suffix.empty()) {
      if (suffix.front() != ':' || !IsValidPort(suffix.substr(1))) {
        *out_error_message = "Invalid URL: invalid port after IPv6 host.";
        return false;
      }
    }
  } else {
    size_t colon_pos = hostport.find(':');
    std::string_view host =
        (colon_pos == std::string_view::npos) ? hostport : hostport.substr(0, colon_pos);
    if (host.empty()) {
      *out_error_message = "Invalid URL: missing host.";
      return false;
    }
    if (colon_pos != std::string_view::npos) {
      if (!IsValidPort(hostport.substr(colon_pos + 1))) {
        *out_error_message = "Invalid URL: invalid port number.";
        return false;
      }
    }
  }

  return true;
}

bool ValidateCommonFields(const TurboHttpRequest& request,
                          ValidatedCurlRequest* validated,
                          std::string* out_error_message) {
  if (!IsValidHttpMethod(request.method())) {
    *out_error_message = "Invalid HTTP method.";
    return false;
  }

  if (!ValidateHttpUrl(request.url(), out_error_message)) {
    return false;
  }

  validated->headers.reserve(request.headers().size());
  for (const auto& [key, val] : request.headers()) {
    if (!IsValidHeaderName(key)) {
      *out_error_message = "Invalid characters in request header name.";
      return false;
    }
    if (ContainsForbiddenHeaderValueChars(val)) {
      *out_error_message = "Invalid characters in request header value.";
      return false;
    }
    if (EqualsIgnoreCaseAscii(key, "accept-encoding")) {
      validated->has_accept_encoding_header = true;
    } else if (EqualsIgnoreCaseAscii(key, "expect")) {
      validated->has_expect_header = true;
    }
    validated->headers.emplace_back(key, val);
  }

  if (request.connect_timeout_ms() != nullptr) {
    validated->connect_timeout_ms = *request.connect_timeout_ms();
  }
  if (request.send_timeout_ms() != nullptr) {
    validated->send_timeout_ms = *request.send_timeout_ms();
  }
  if (request.receive_timeout_ms() != nullptr) {
    validated->receive_timeout_ms = *request.receive_timeout_ms();
  }
  if (request.max_response_bytes() != nullptr) {
    validated->max_response_bytes =
        std::max<int64_t>(0, *request.max_response_bytes());
  }
  validated->follow_redirects = request.follow_redirects();
  validated->allow_bad_certificates = request.allow_bad_certificates();
  return true;
}

}  // namespace

std::string_view TrimAsciiView(std::string_view str) {
  size_t first = 0;
  while (first < str.size() && IsAsciiSpace(str[first])) {
    ++first;
  }
  size_t last = str.size();
  while (last > first && IsAsciiSpace(str[last - 1])) {
    --last;
  }
  return str.substr(first, last - first);
}

std::string ToLowerAscii(std::string_view str) {
  std::string out(str.size(), '\0');
  for (size_t i = 0; i < str.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(str[i]);
    out[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A'))
                                    : static_cast<char>(c);
  }
  return out;
}

bool EqualsIgnoreCaseAscii(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (size_t i = 0; i < lhs.size(); ++i) {
    unsigned char a = static_cast<unsigned char>(lhs[i]);
    unsigned char b = static_cast<unsigned char>(rhs[i]);
    if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a + ('a' - 'A'));
    if (b >= 'A' && b <= 'Z') b = static_cast<unsigned char>(b + ('a' - 'A'));
    if (a != b) {
      return false;
    }
  }
  return true;
}

bool ValidateCurlRequest(const TurboHttpRequest& request,
                         ValidatedCurlRequest* out_validated,
                         std::string* out_error_message) {
  ValidatedCurlRequest validated;
  if (!ValidateCommonFields(request, &validated, out_error_message)) {
    return false;
  }

  validated.request_id = request.request_id();
  validated.url = request.url();
  validated.method = request.method();
  if (request.body() != nullptr) {
    validated.body = *request.body();
  }

  *out_validated = std::move(validated);
  return true;
}

bool ValidateCurlRequest(TurboHttpRequest&& request,
                         ValidatedCurlRequest* out_validated,
                         std::string* out_error_message) {
  ValidatedCurlRequest validated;
  if (!ValidateCommonFields(request, &validated, out_error_message)) {
    return false;
  }

  validated.request_id = std::move(request.request_id_);
  validated.url = std::move(request.url_);
  validated.method = std::move(request.method_);
  validated.body = std::move(request.body_);

  *out_validated = std::move(validated);
  return true;
}

long ClampCurlTimeoutMs(int64_t timeout_ms) {
  if (timeout_ms <= 0) {
    return 0L;
  }
  if (timeout_ms > static_cast<int64_t>(LONG_MAX)) {
    return LONG_MAX;
  }
  return static_cast<long>(timeout_ms);
}

std::pair<std::string, std::string> ClassifyCurlError(
    CURLcode curl_code,
    bool is_cancelled,
    ExecutionPhase phase,
    const char* timed_out_error_code) {
  if (timed_out_error_code != nullptr) {
    return {timed_out_error_code, "Operation timed out during transfer."};
  }

  if (is_cancelled || curl_code == CURLE_ABORTED_BY_CALLBACK) {
    return {TurboErrorCodes::kCancel, "Request was cancelled."};
  }

  if (curl_code == CURLE_OPERATION_TIMEDOUT) {
    switch (phase) {
      case ExecutionPhase::kConnecting:
        return {TurboErrorCodes::kConnectionTimeout, "Connection timed out."};
      case ExecutionPhase::kSendingBody:
        return {TurboErrorCodes::kSendTimeout, "Send request body timed out."};
      case ExecutionPhase::kReceiving:
      default:
        return {TurboErrorCodes::kReceiveTimeout,
                "Receive response timed out."};
    }
  }

  switch (curl_code) {
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_SHUTDOWN_FAILED:
    case CURLE_SSL_CRL_BADFILE:
    case CURLE_SSL_ISSUER_ERROR:
      return {TurboErrorCodes::kBadCertificate, curl_easy_strerror(curl_code)};

    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_HTTP2:
    case CURLE_HTTP2_STREAM:
    case CURLE_UNSUPPORTED_PROTOCOL:
    case CURLE_TOO_MANY_REDIRECTS:
    case CURLE_URL_MALFORMAT:
      return {TurboErrorCodes::kConnectionError, curl_easy_strerror(curl_code)};

    default:
      return {TurboErrorCodes::kUnknown, curl_easy_strerror(curl_code)};
  }
}

bool ParseStatusLine(std::string_view line, ParsedStatusLine* out_status) {
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
    line.remove_suffix(1);
  }
  if (line.size() < 9 || line.substr(0, 5) != "HTTP/") {
    return false;
  }

  size_t space_pos = line.find(' ');
  if (space_pos == std::string_view::npos) {
    return false;
  }
  while (space_pos < line.size() && line[space_pos] == ' ') {
    ++space_pos;
  }
  if (space_pos >= line.size()) {
    return false;
  }

  size_t code_end = line.find(' ', space_pos);
  std::string_view code_view =
      (code_end == std::string_view::npos)
          ? line.substr(space_pos)
          : line.substr(space_pos, code_end - space_pos);

  int parsed_code = 0;
  const char* begin = code_view.data();
  const char* finish = begin + code_view.size();
  auto [ptr, ec] = std::from_chars(begin, finish, parsed_code);
  if (ec != std::errc() || ptr != finish || parsed_code < 100 ||
      parsed_code > 599) {
    return false;
  }

  std::string message;
  if (code_end != std::string_view::npos) {
    std::string_view reason_view = TrimAsciiView(line.substr(code_end + 1));
    message.assign(reason_view.data(), reason_view.size());
  }

  if (out_status != nullptr) {
    out_status->status_code = parsed_code;
    out_status->status_message = std::move(message);
  }
  return true;
}

bool ParseHeaderLine(std::string_view line,
                     std::string* out_lower_key,
                     std::string_view* out_value) {
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
    line.remove_suffix(1);
  }
  if (line.empty()) {
    return false;
  }

  size_t colon_pos = line.find(':');
  if (colon_pos == std::string_view::npos || colon_pos == 0) {
    return false;
  }

  std::string_view key_view = TrimAsciiView(line.substr(0, colon_pos));
  if (key_view.empty()) {
    return false;
  }

  *out_lower_key = ToLowerAscii(key_view);
  *out_value = TrimAsciiView(line.substr(colon_pos + 1));
  return true;
}

std::optional<int64_t> ParseContentLength(std::string_view value) {
  std::string_view trimmed = TrimAsciiView(value);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  int64_t parsed = 0;
  const char* begin = trimmed.data();
  const char* finish = begin + trimmed.size();
  auto [ptr, ec] = std::from_chars(begin, finish, parsed);
  if (ec != std::errc() || ptr != finish || parsed < 0) {
    return std::nullopt;
  }
  return parsed;
}

TurboHttpResponse MakeErrorResponse(std::string_view code,
                                    std::string_view message) {
  const int64_t zero_status = 0;
  const std::string err_code(code);
  const std::string err_msg(message);
  return TurboHttpResponse(&zero_status, nullptr, nullptr, nullptr, &err_code,
                           &err_msg);
}

TurboHttpResponse MakeCancelResponse() {
  return MakeErrorResponse(TurboErrorCodes::kCancel, "Request was cancelled.");
}

TurboHttpResponse MakeMaxResponseBytesExceededResponse(
    int64_t limit,
    std::optional<int64_t> observed_bytes) {
  std::string message;
  if (observed_bytes.has_value()) {
    message = "Response size (" + std::to_string(*observed_bytes) +
              " bytes) exceeded maxResponseBytes limit (" +
              std::to_string(limit) + " bytes).";
  } else {
    message = "Response payload exceeded maxResponseBytes limit (" +
              std::to_string(limit) + " bytes).";
  }
  return MakeErrorResponse(TurboErrorCodes::kMaxResponseBytesExceeded, message);
}

}  // namespace flutter_turbo_fetch
