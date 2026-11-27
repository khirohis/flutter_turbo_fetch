#ifndef FLUTTER_PLUGIN_CURL_REQUEST_CONTEXT_H_
#define FLUTTER_PLUGIN_CURL_REQUEST_CONTEXT_H_

#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "curl_helpers.h"

namespace flutter_turbo_fetch {

// Per-request state machine managing response headers/body aggregation,
// two-stage maxResponseBytes enforcement, redirect resets, and phase timeouts.
struct CurlRequestContext {
  std::atomic<bool> is_cancelled{false};
  std::atomic<bool> is_exceeded_limit{false};
  std::atomic<bool> completed{false};
  std::atomic<bool> callback_exception{false};
  const char* timed_out_error_code = nullptr;
  std::string callback_exception_message;

  // User completion callback stored inline to avoid extra shared_ptr allocations.
  std::function<void(TurboHttpResponse)> on_result;

  ExecutionPhase phase = ExecutionPhase::kConnecting;
  bool is_head_request = false;
  bool has_request_body = false;
  bool follow_redirects = true;

  int current_status_code = 0;
  bool has_location_header = false;
  std::string status_message;
  std::unordered_map<std::string, std::vector<std::string>> response_headers;
  std::optional<int64_t> content_length;
  std::optional<int64_t> exceeded_observed_bytes;
  std::vector<uint8_t> response_body;
  bool body_reserved = false;

  std::chrono::steady_clock::time_point start_time{};
  std::chrono::steady_clock::time_point last_send_activity{};
  std::chrono::steady_clock::time_point last_receive_activity{};
  curl_off_t last_ulnow = 0;
  curl_off_t last_dlnow = 0;

  std::optional<int64_t> connect_timeout_ms;
  std::optional<int64_t> send_timeout_ms;
  std::optional<int64_t> receive_timeout_ms;
  std::optional<int64_t> max_response_bytes;

  // Owned and accessed exclusively on the event-loop thread while in flight.
  CURL* easy_handle = nullptr;

  bool TryMarkCompleted() {
    return !completed.exchange(true, std::memory_order_acq_rel);
  }

  bool has_any_timeout() const {
    return connect_timeout_ms.has_value() || send_timeout_ms.has_value() ||
           receive_timeout_ms.has_value();
  }

  bool has_receive_timeout() const { return receive_timeout_ms.has_value(); }

  bool IsRedirectStatusCode() const {
    return current_status_code == 301 || current_status_code == 302 ||
           current_status_code == 303 || current_status_code == 307 ||
           current_status_code == 308;
  }

  bool ShouldDeferContentLengthLimit() const {
    return follow_redirects && IsRedirectStatusCode();
  }

  bool IsRedirectIntermediate() const {
    return follow_redirects && IsRedirectStatusCode() && has_location_header;
  }

  bool IsInformationalResponse() const {
    return current_status_code >= 100 && current_status_code < 200;
  }

  void RecordCallbackException(std::string_view message);

  void InitializeFromRequest(const ValidatedCurlRequest& request,
                             std::chrono::steady_clock::time_point now);

  // Processes a single header line from libcurl; returns false to abort.
  bool OnHeaderLine(std::string_view line,
                    std::chrono::steady_clock::time_point now);

  // Processes a received response body chunk; returns false to abort.
  bool OnBodyChunk(const uint8_t* data,
                   size_t len,
                   std::chrono::steady_clock::time_point now);

  // Updates phase transitions and checks phase idle/connect timeouts.
  // Returns false to abort transfer.
  bool OnProgress(curl_off_t dlnow,
                  curl_off_t ulnow,
                  curl_off_t ultotal,
                  bool connect_completed,
                  std::chrono::steady_clock::time_point now);

  // Checks if the active phase has exceeded its configured timeout.
  bool CheckPhaseTimeout(std::chrono::steady_clock::time_point now);

  // Computes milliseconds until the active phase times out, or nullopt if none.
  std::optional<int> RemainingTimeoutMs(
      std::chrono::steady_clock::time_point now) const;

 private:
  struct ActiveTimeout {
    int64_t limit_ms = 0;
    std::chrono::steady_clock::time_point origin{};
    const char* error_code = nullptr;
  };

  std::optional<ActiveTimeout> ResolveActiveTimeout() const;
};

}  // namespace flutter_turbo_fetch

#endif  // FLUTTER_PLUGIN_CURL_REQUEST_CONTEXT_H_
