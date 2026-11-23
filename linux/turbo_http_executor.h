#ifndef FLUTTER_PLUGIN_TURBO_HTTP_EXECUTOR_H_
#define FLUTTER_PLUGIN_TURBO_HTTP_EXECUTOR_H_

#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "turbo_session_manager.h"

namespace flutter_turbo_fetch {

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
      : request_id_(std::move(request_id)),
        url_(std::move(url)),
        method_(std::move(method)),
        headers_(std::move(headers)),
        body_(body ? std::make_optional(*body) : std::nullopt),
        connect_timeout_ms_(connect_timeout_ms ? std::make_optional(*connect_timeout_ms) : std::nullopt),
        receive_timeout_ms_(receive_timeout_ms ? std::make_optional(*receive_timeout_ms) : std::nullopt),
        send_timeout_ms_(send_timeout_ms ? std::make_optional(*send_timeout_ms) : std::nullopt),
        follow_redirects_(follow_redirects),
        allow_bad_certificates_(allow_bad_certificates),
        max_response_bytes_(max_response_bytes ? std::make_optional(*max_response_bytes) : std::nullopt) {}

  const std::string& request_id() const { return request_id_; }
  const std::string& url() const { return url_; }
  const std::string& method() const { return method_; }
  const std::unordered_map<std::string, std::string>& headers() const { return headers_; }
  const std::vector<uint8_t>* body() const { return body_.has_value() ? &(*body_) : nullptr; }
  const int64_t* connect_timeout_ms() const { return connect_timeout_ms_.has_value() ? &(*connect_timeout_ms_) : nullptr; }
  const int64_t* receive_timeout_ms() const { return receive_timeout_ms_.has_value() ? &(*receive_timeout_ms_) : nullptr; }
  const int64_t* send_timeout_ms() const { return send_timeout_ms_.has_value() ? &(*send_timeout_ms_) : nullptr; }
  bool follow_redirects() const { return follow_redirects_; }
  bool allow_bad_certificates() const { return allow_bad_certificates_; }
  const int64_t* max_response_bytes() const { return max_response_bytes_.has_value() ? &(*max_response_bytes_) : nullptr; }

 private:
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
      const std::unordered_map<std::string, std::vector<std::string>>* headers = nullptr,
      const std::vector<uint8_t>* body = nullptr,
      const std::string* error_code = nullptr,
      const std::string* error_message = nullptr)
      : status_code_(status_code ? std::make_optional(*status_code) : std::nullopt),
        status_message_(status_message ? std::make_optional(*status_message) : std::nullopt),
        headers_(headers ? *headers : std::unordered_map<std::string, std::vector<std::string>>{}),
        body_(body ? *body : std::vector<uint8_t>{}),
        error_code_(error_code ? std::make_optional(*error_code) : std::nullopt),
        error_message_(error_message ? std::make_optional(*error_message) : std::nullopt) {}

  const int64_t* status_code() const { return status_code_.has_value() ? &(*status_code_) : nullptr; }
  const std::string* status_message() const { return status_message_.has_value() ? &(*status_message_) : nullptr; }
  const std::unordered_map<std::string, std::vector<std::string>>& headers() const { return headers_; }
  const std::vector<uint8_t>* body() const { return &body_; }
  const std::string* error_code() const { return error_code_.has_value() ? &(*error_code_) : nullptr; }
  const std::string* error_message() const { return error_message_.has_value() ? &(*error_message_) : nullptr; }

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
inline constexpr const char* kMaxResponseBytesExceeded = "maxResponseBytesExceeded";
inline constexpr const char* kConnectionError = "connectionError";
inline constexpr const char* kUnknown = "unknown";
}  // namespace TurboErrorCodes

enum class ExecutionPhase {
  kConnecting,
  kSendingBody,
  kReceiving,
};

struct LinuxRequestContext {
  std::atomic<bool> is_cancelled{false};
  std::atomic<bool> is_exceeded_limit{false};
  const char* timed_out_error_code = nullptr;

  ExecutionPhase phase = ExecutionPhase::kConnecting;
  std::chrono::steady_clock::time_point start_time;
  std::chrono::steady_clock::time_point connect_end_time;
  std::chrono::steady_clock::time_point body_send_end_time;

  std::optional<int64_t> connect_timeout_ms;
  std::optional<int64_t> send_timeout_ms;
  std::optional<int64_t> receive_timeout_ms;
  std::optional<int64_t> max_response_bytes;

  std::string response_headers;
  std::vector<uint8_t> response_body;
};

class TurboHttpExecutor {
 public:
  explicit TurboHttpExecutor(TurboSessionManager& session_manager);
  virtual ~TurboHttpExecutor();

  TurboHttpExecutor(const TurboHttpExecutor&) = delete;
  TurboHttpExecutor& operator=(const TurboHttpExecutor&) = delete;

  virtual void Execute(const TurboHttpRequest& request,
                       std::function<void(TurboHttpResponse)> on_result);

  virtual void CancelRequest(const std::string& request_id);

  virtual void Shutdown();

  size_t active_request_count() const;
  bool has_active_request(const std::string& request_id) const;

  // Classifies a CURLcode / phase / cancel / timeout into a platform-unified error code and message.
  static std::pair<std::string, std::string> ClassifyError(
      CURLcode curl_code, bool is_cancelled, ExecutionPhase phase,
      const char* timed_out_error_code);

  // Parses raw response headers into lowercase multi-value map and extracts Content-Length.
  static std::pair<std::unordered_map<std::string, std::vector<std::string>>, std::optional<int64_t>> ParseRawHeaders(
      const std::string& raw_headers);

  // Validates maxResponseBytes against Content-Length header and cumulative body chunks.
  static TurboHttpResponse ValidateAndAssembleChunks(
      int64_t status_code, const std::string& status_message,
      const std::string& raw_headers, const std::vector<uint8_t>& body,
      std::optional<int64_t> max_response_bytes);

 private:
  struct PendingRequest {
    TurboHttpRequest request;
    std::shared_ptr<LinuxRequestContext> ctx;
    std::function<void(TurboHttpResponse)> on_result;
  };

  struct InFlightRequest {
    std::string request_id;
    std::shared_ptr<LinuxRequestContext> ctx;
    std::vector<uint8_t> request_body;
    struct curl_slist* header_list = nullptr;
    std::function<void(TurboHttpResponse)> on_result;

    ~InFlightRequest() {
      if (header_list != nullptr) {
        curl_slist_free_all(header_list);
        header_list = nullptr;
      }
    }
  };

  void EnsureEventLoopStartedLocked();
  void RunEventLoop(CURLM* multi_handle);
  CURL* CreateEasyHandle(const TurboHttpRequest& request,
                         InFlightRequest& in_flight);
  static bool CheckPhaseTimeout(LinuxRequestContext& ctx,
                                std::chrono::steady_clock::time_point now);
  static TurboHttpResponse BuildFinalResponse(CURL* curl, CURLcode res,
                                              LinuxRequestContext& ctx);

  static size_t HeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata);
  static size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata);
  static int XferInfoCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow,
                              curl_off_t ultotal, curl_off_t ulnow);

  TurboSessionManager& session_manager_;

  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<LinuxRequestContext>> active_requests_;
  std::deque<PendingRequest> pending_requests_;
  std::thread event_loop_thread_;
  bool event_loop_started_ = false;
  bool stop_requested_ = false;
  std::atomic<bool> is_shutdown_{false};
};

}  // namespace flutter_turbo_fetch

#endif  // FLUTTER_PLUGIN_TURBO_HTTP_EXECUTOR_H_

