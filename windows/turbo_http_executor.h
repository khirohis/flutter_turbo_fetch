#ifndef FLUTTER_PLUGIN_TURBO_HTTP_EXECUTOR_H_
#define FLUTTER_PLUGIN_TURBO_HTTP_EXECUTOR_H_

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "messages.g.h"
#include "turbo_session_manager.h"

namespace flutter_turbo_fetch {

enum class RequestPhase {
  kConnecting,
  kSendingBody,
  kReceiving,
};

struct RequestContext {
  std::mutex handle_mutex;
  HINTERNET h_request = nullptr;
  std::atomic<bool> is_cancelled{false};

  bool SetRequestHandle(HINTERNET handle) {
    std::lock_guard<std::mutex> lock(handle_mutex);
    if (is_cancelled.load()) {
      if (handle != nullptr) {
        WinHttpCloseHandle(handle);
      }
      return false;
    }
    h_request = handle;
    return true;
  }

  void CloseRequestHandle() {
    std::lock_guard<std::mutex> lock(handle_mutex);
    if (h_request != nullptr) {
      WinHttpCloseHandle(h_request);
      h_request = nullptr;
    }
  }
};

class TurboHttpExecutor {
 public:
  using TransportOverride =
      std::function<TurboHttpResponse(const TurboHttpRequest&, RequestContext&)>;

  TurboHttpExecutor();
  explicit TurboHttpExecutor(TransportOverride transport_override);
  virtual ~TurboHttpExecutor();

  TurboHttpExecutor(const TurboHttpExecutor&) = delete;
  TurboHttpExecutor& operator=(const TurboHttpExecutor&) = delete;

  virtual void Execute(const TurboHttpRequest& request,
                       std::function<void(TurboHttpResponse)> on_result);

  virtual void Cancel(const std::string& request_id);

  virtual void Shutdown();

  // Classifies a Win32 / WinHTTP error into the platform-unified error code.
  static std::string ClassifyError(const RequestContext& ctx,
                                   DWORD win32_error,
                                   RequestPhase phase);

  // Parses CRLF-delimited raw HTTP response headers into a lowercase-keyed
  // multi-value EncodableMap, optionally extracting Content-Length.
  static flutter::EncodableMap ParseRawHeaders(
      const std::wstring& raw_headers,
      std::optional<int64_t>* out_content_length);

  // Helper used by unit tests and chunk reader to validate maxResponseBytes
  // both via Content-Length and cumulative chunk byte counts.
  static bool ValidateAndAssembleChunks(
      std::optional<int64_t> content_length,
      std::optional<int64_t> max_response_bytes,
      const std::vector<std::vector<uint8_t>>& chunks,
      std::vector<uint8_t>* out_body,
      std::string* out_error_code,
      std::string* out_error_message);

 private:
  struct ParsedUrl {
    bool is_https = false;
    INTERNET_PORT port = 0;
    std::wstring host;
    std::wstring path_and_query;
  };

  struct WorkTask {
    TurboHttpExecutor* executor = nullptr;
    TurboHttpRequest request;
    std::shared_ptr<RequestContext> ctx;
    ParsedUrl parsed_url;
    std::wstring formatted_headers;
    std::function<void(TurboHttpResponse)> on_result;
  };

  static void CALLBACK WorkCallback(PTP_CALLBACK_INSTANCE instance,
                                    PVOID parameter,
                                    PTP_WORK work);

  void InitThreadPool();
  TurboHttpResponse PerformWinHttpRequest(
      const ParsedUrl& parsed_url,
      const std::wstring& formatted_headers,
      const TurboHttpRequest& request,
      RequestContext& ctx);

  TurboSessionManager session_manager_;
  TransportOverride transport_override_;

  PTP_POOL pool_ = nullptr;
  PTP_CLEANUP_GROUP cleanup_group_ = nullptr;
  TP_CALLBACK_ENVIRON callback_env_{};
  bool env_initialized_ = false;

  std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<RequestContext>>
      active_requests_;
  std::atomic<bool> is_shutdown_{false};
};

}  // namespace flutter_turbo_fetch

#endif  // FLUTTER_PLUGIN_TURBO_HTTP_EXECUTOR_H_

