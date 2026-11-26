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
#include <string_view>
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
  using WriteDataFn =
      std::function<bool(const uint8_t* data, DWORD len, DWORD* bytes_written)>;
  using ReadDataFn =
      std::function<bool(uint8_t* dst, DWORD max_len, DWORD* bytes_read)>;

  TurboHttpExecutor();
  explicit TurboHttpExecutor(TransportOverride transport_override);
  virtual ~TurboHttpExecutor();

  TurboHttpExecutor(const TurboHttpExecutor&) = delete;
  TurboHttpExecutor& operator=(const TurboHttpExecutor&) = delete;

  virtual void Execute(const TurboHttpRequest& request,
                       std::function<void(TurboHttpResponse)> on_result);

  virtual void Cancel(const std::string& request_id);

  virtual void Shutdown();

  size_t active_request_count() const;
  bool has_active_request(const std::string& request_id) const;

  // Classifies a Win32 / WinHTTP error into the platform-unified error code.
  static std::string ClassifyError(const RequestContext& ctx,
                                   DWORD win32_error,
                                   RequestPhase phase);

  // Parses CRLF-delimited raw HTTP response headers into a lowercase-keyed
  // multi-value EncodableMap, optionally extracting Content-Length.
  static flutter::EncodableMap ParseRawHeaders(
      std::wstring_view raw_headers,
      std::optional<int64_t>* out_content_length);

  // Writes the entire request body via write_fn, handling partial writes and
  // cancellation between chunks.
  static bool WriteAllRequestBody(const std::vector<uint8_t>& body,
                                  const RequestContext& ctx,
                                  const WriteDataFn& write_fn,
                                  DWORD* out_win32_error);

  // Reads the response body directly into out_body without intermediate chunk
  // copies, enforcing maxResponseBytes and cancellation checks.
  static bool ReadResponseBodyDirect(
      std::optional<int64_t> content_length,
      std::optional<int64_t> max_response_bytes,
      const RequestContext& ctx,
      const ReadDataFn& read_fn,
      std::vector<uint8_t>* out_body,
      std::string* out_error_code,
      std::string* out_error_message,
      DWORD* out_win32_error);

  // Helper used by unit tests to validate maxResponseBytes both via
  // Content-Length and cumulative chunk byte counts.
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

  struct RequestParams {
    std::string request_id;
    std::string method;
    std::optional<std::vector<uint8_t>> body;
    std::optional<int64_t> connect_timeout_ms;
    std::optional<int64_t> send_timeout_ms;
    std::optional<int64_t> receive_timeout_ms;
    std::optional<int64_t> max_response_bytes;
    bool follow_redirects = true;
    bool allow_bad_certificates = false;
  };

  struct WorkTask {
    TurboHttpExecutor* executor = nullptr;
    RequestParams params;
    std::optional<TurboHttpRequest> override_request;
    std::shared_ptr<RequestContext> ctx;
    ParsedUrl parsed_url;
    std::wstring formatted_headers;
    std::function<void(TurboHttpResponse)> on_result;
  };

  static void CALLBACK WorkCallback(PTP_CALLBACK_INSTANCE instance,
                                    PVOID parameter);

  void InitThreadPool();
  TurboHttpResponse PerformWinHttpRequest(
      const ParsedUrl& parsed_url,
      const std::wstring& formatted_headers,
      const RequestParams& params,
      RequestContext& ctx);

  TurboSessionManager session_manager_;
  TransportOverride transport_override_;

  PTP_POOL pool_ = nullptr;
  PTP_CLEANUP_GROUP cleanup_group_ = nullptr;
  TP_CALLBACK_ENVIRON callback_env_{};
  bool env_initialized_ = false;

  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<RequestContext>>
      active_requests_;
  std::atomic<bool> is_shutdown_{false};
};

}  // namespace flutter_turbo_fetch

#endif  // FLUTTER_PLUGIN_TURBO_HTTP_EXECUTOR_H_
