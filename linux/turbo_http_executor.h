#ifndef FLUTTER_PLUGIN_TURBO_HTTP_EXECUTOR_H_
#define FLUTTER_PLUGIN_TURBO_HTTP_EXECUTOR_H_

#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "messages.g.h"
#include "turbo_session_manager.h"

namespace flutter_turbo_fetch {

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
  std::mutex mutex;
  CURL* curl_handle = nullptr;
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

  bool SetCurlHandle(CURL* handle) {
    std::lock_guard<std::mutex> lock(mutex);
    if (is_cancelled.load()) {
      return false;
    }
    curl_handle = handle;
    return true;
  }
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

  // Parses raw response headers into lowercase multi-value EncodableMap and extracts Content-Length.
  static std::pair<flutter::EncodableMap, std::optional<int64_t>> ParseRawHeaders(
      const std::string& raw_headers);

  // Validates maxResponseBytes against Content-Length header and cumulative body chunks.
  static TurboHttpResponse ValidateAndAssembleChunks(
      int64_t status_code, const std::string& status_message,
      const std::string& raw_headers, const std::vector<uint8_t>& body,
      std::optional<int64_t> max_response_bytes);

 private:
  TurboHttpResponse PerformCurlRequest(const TurboHttpRequest& request,
                                       LinuxRequestContext& ctx);

  static size_t HeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata);
  static size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata);
  static size_t ReadCallback(char* buffer, size_t size, size_t nitems, void* userdata);
  static int XferInfoCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow,
                              curl_off_t ultotal, curl_off_t ulnow);

  TurboSessionManager& session_manager_;

  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<LinuxRequestContext>> active_requests_;
  std::atomic<bool> is_shutdown_{false};
};

}  // namespace flutter_turbo_fetch

#endif  // FLUTTER_PLUGIN_TURBO_HTTP_EXECUTOR_H_

