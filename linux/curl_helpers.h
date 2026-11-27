#ifndef FLUTTER_PLUGIN_CURL_HELPERS_H_
#define FLUTTER_PLUGIN_CURL_HELPERS_H_

#include <curl/curl.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace flutter_turbo_fetch {

struct ValidatedCurlRequest;

class TurboHttpRequest {
 public:
  TurboHttpRequest(
      std::string request_id,
      std::string url,
      std::string method,
      std::unordered_map<std::string, std::string> headers = {},
      const std::vector<uint8_t>* body = nullptr,
      const int64_t* connect_timeout_ms = nullptr,
      const int64_t* receive_timeout_ms = nullptr,
      const int64_t* send_timeout_ms = nullptr,
      bool follow_redirects = true,
      bool allow_bad_certificates = false,
      const int64_t* max_response_bytes = nullptr)
      : TurboHttpRequest(std::move(request_id),
                         std::move(url),
                         std::move(method),
                         std::move(headers),
                         body ? std::make_optional(*body) : std::nullopt,
                         connect_timeout_ms,
                         receive_timeout_ms,
                         send_timeout_ms,
                         follow_redirects,
                         allow_bad_certificates,
                         max_response_bytes) {}

  TurboHttpRequest(
      std::string request_id,
      std::string url,
      std::string method,
      std::unordered_map<std::string, std::string> headers,
      std::optional<std::vector<uint8_t>> body,
      const int64_t* connect_timeout_ms,
      const int64_t* receive_timeout_ms,
      const int64_t* send_timeout_ms,
      bool follow_redirects,
      bool allow_bad_certificates,
      const int64_t* max_response_bytes)
      : request_id_(std::move(request_id)),
        url_(std::move(url)),
        method_(std::move(method)),
        headers_(std::move(headers)),
        body_(std::move(body)),
        connect_timeout_ms_(connect_timeout_ms
                                ? std::make_optional(*connect_timeout_ms)
                                : std::nullopt),
        receive_timeout_ms_(receive_timeout_ms
                                ? std::make_optional(*receive_timeout_ms)
                                : std::nullopt),
        send_timeout_ms_(send_timeout_ms ? std::make_optional(*send_timeout_ms)
                                         : std::nullopt),
        follow_redirects_(follow_redirects),
        allow_bad_certificates_(allow_bad_certificates),
        max_response_bytes_(max_response_bytes
                                ? std::make_optional(*max_response_bytes)
                                : std::nullopt) {}

  const std::string& request_id() const { return request_id_; }
  const std::string& url() const { return url_; }
  const std::string& method() const { return method_; }
  const std::unordered_map<std::string, std::string>& headers() const {
    return headers_;
  }
  const std::vector<uint8_t>* body() const {
    return body_.has_value() ? &(*body_) : nullptr;
  }
  const int64_t* connect_timeout_ms() const {
    return connect_timeout_ms_.has_value() ? &(*connect_timeout_ms_) : nullptr;
  }
  const int64_t* receive_timeout_ms() const {
    return receive_timeout_ms_.has_value() ? &(*receive_timeout_ms_) : nullptr;
  }
  const int64_t* send_timeout_ms() const {
    return send_timeout_ms_.has_value() ? &(*send_timeout_ms_) : nullptr;
  }
  bool follow_redirects() const { return follow_redirects_; }
  bool allow_bad_certificates() const { return allow_bad_certificates_; }
  const int64_t* max_response_bytes() const {
    return max_response_bytes_.has_value() ? &(*max_response_bytes_) : nullptr;
  }

 private:
  friend bool ValidateCurlRequest(TurboHttpRequest&& request,
                                  ValidatedCurlRequest* out_validated,
                                  std::string* out_error_message);

  std::string request_id_;
  std::string url_;
  std::string method_;
  std::unordered_map<std::string, std::string> headers_;
  std::optional<std::vector<uint8_t>> body_;
  std::optional<int64_t> connect_timeout_ms_;
  std::optional<int64_t> receive_timeout_ms_;
  std::optional<int64_t> send_timeout_ms_;
  bool follow_redirects_ = true;
  bool allow_bad_certificates_ = false;
  std::optional<int64_t> max_response_bytes_;
};

class TurboHttpResponse {
 public:
  TurboHttpResponse(
      const int64_t* status_code = nullptr,
      const std::string* status_message = nullptr,
      const std::unordered_map<std::string, std::vector<std::string>>* headers =
          nullptr,
      const std::vector<uint8_t>* body = nullptr,
      const std::string* error_code = nullptr,
      const std::string* error_message = nullptr)
      : status_code_(status_code ? std::make_optional(*status_code)
                                 : std::nullopt),
        status_message_(status_message ? std::make_optional(*status_message)
                                       : std::nullopt),
        headers_(headers
                     ? *headers
                     : std::unordered_map<std::string,
                                          std::vector<std::string>>{}),
        body_(body ? *body : std::vector<uint8_t>{}),
        error_code_(error_code ? std::make_optional(*error_code) : std::nullopt),
        error_message_(error_message ? std::make_optional(*error_message)
                                     : std::nullopt) {}

  TurboHttpResponse(
      int64_t status_code,
      std::string status_message,
      std::unordered_map<std::string, std::vector<std::string>> headers,
      std::vector<uint8_t> body)
      : status_code_(status_code),
        status_message_(std::move(status_message)),
        headers_(std::move(headers)),
        body_(std::move(body)) {}

  const int64_t* status_code() const {
    return status_code_.has_value() ? &(*status_code_) : nullptr;
  }
  const std::string* status_message() const {
    return status_message_.has_value() ? &(*status_message_) : nullptr;
  }
  const std::unordered_map<std::string, std::vector<std::string>>& headers()
      const {
    return headers_;
  }
  const std::vector<uint8_t>* body() const { return &body_; }
  const std::string* error_code() const {
    return error_code_.has_value() ? &(*error_code_) : nullptr;
  }
  const std::string* error_message() const {
    return error_message_.has_value() ? &(*error_message_) : nullptr;
  }

 private:
  std::optional<int64_t> status_code_;
  std::optional<std::string> status_message_;
  std::unordered_map<std::string, std::vector<std::string>> headers_;
  std::vector<uint8_t> body_;
  std::optional<std::string> error_code_;
  std::optional<std::string> error_message_;
};

namespace TurboErrorCodes {
inline constexpr const char* kCancel = "cancel";
inline constexpr const char* kConnectionTimeout = "connectionTimeout";
inline constexpr const char* kSendTimeout = "sendTimeout";
inline constexpr const char* kReceiveTimeout = "receiveTimeout";
inline constexpr const char* kBadCertificate = "badCertificate";
inline constexpr const char* kMaxResponseBytesExceeded =
    "maxResponseBytesExceeded";
inline constexpr const char* kConnectionError = "connectionError";
inline constexpr const char* kUnknown = "unknown";
}  // namespace TurboErrorCodes

enum class ExecutionPhase {
  kConnecting,
  kSendingBody,
  kReceiving,
};

struct ValidatedCurlRequest {
  std::string request_id;
  std::string url;
  std::string method;
  std::vector<std::pair<std::string, std::string>> headers;
  std::optional<std::vector<uint8_t>> body;
  std::optional<int64_t> connect_timeout_ms;
  std::optional<int64_t> send_timeout_ms;
  std::optional<int64_t> receive_timeout_ms;
  std::optional<int64_t> max_response_bytes;
  bool follow_redirects = true;
  bool allow_bad_certificates = false;
  bool has_accept_encoding_header = false;
  bool has_expect_header = false;
};

struct ParsedStatusLine {
  int status_code = 0;
  std::string status_message;
};

// Validates and normalizes an incoming request before dispatch.
bool ValidateCurlRequest(const TurboHttpRequest& request,
                         ValidatedCurlRequest* out_validated,
                         std::string* out_error_message);

// Move overload transferring body and string buffers without extra copies.
bool ValidateCurlRequest(TurboHttpRequest&& request,
                         ValidatedCurlRequest* out_validated,
                         std::string* out_error_message);

// Safely clamps a timeout in milliseconds to libcurl's signed long range.
long ClampCurlTimeoutMs(int64_t timeout_ms);

// Maps libcurl error codes, cancellation state, and phase timeouts to unified
// turbo-fetch error codes and messages.
std::pair<std::string, std::string> ClassifyCurlError(
    CURLcode curl_code,
    bool is_cancelled,
    ExecutionPhase phase,
    const char* timed_out_error_code);

// Parses an HTTP status line (e.g., "HTTP/1.1 200 OK" or "HTTP/2 204").
bool ParseStatusLine(std::string_view line, ParsedStatusLine* out_status);

// Parses a single "Name: Value" HTTP header line into a lowercase key and
// trimmed value view. Returns false for blank or malformed lines.
bool ParseHeaderLine(std::string_view line,
                     std::string* out_lower_key,
                     std::string_view* out_value);

// Strictly parses a non-negative integer Content-Length header value.
std::optional<int64_t> ParseContentLength(std::string_view value);

// Trims leading/trailing ASCII whitespace without allocating.
std::string_view TrimAsciiView(std::string_view str);

// Converts ASCII uppercase letters to lowercase.
std::string ToLowerAscii(std::string_view str);

// Case-insensitive ASCII equality check without allocation.
bool EqualsIgnoreCaseAscii(std::string_view lhs, std::string_view rhs);

// Response constructors for error, cancellation, and size-limit outcomes.
TurboHttpResponse MakeErrorResponse(std::string_view code,
                                    std::string_view message);
TurboHttpResponse MakeCancelResponse();
TurboHttpResponse MakeMaxResponseBytesExceededResponse(
    int64_t limit,
    std::optional<int64_t> observed_bytes = std::nullopt);

}  // namespace flutter_turbo_fetch

#endif  // FLUTTER_PLUGIN_CURL_HELPERS_H_
