#ifndef FLUTTER_PLUGIN_WINHTTP_TRANSPORT_H_
#define FLUTTER_PLUGIN_WINHTTP_TRANSPORT_H_

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "messages.g.h"
#include "turbo_session_manager.h"
#include "winhttp_helpers.h"

namespace flutter_turbo_fetch {

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

  void Cancel() {
    is_cancelled = true;
    CloseRequestHandle();
  }
};

struct ReadBodyResult {
  bool ok = false;
  std::vector<uint8_t> body;
  std::string error_code;
  std::string error_message;
};

class WinHttpTransport {
 public:
  using WriteDataFn =
      std::function<bool(const uint8_t* data, DWORD len, DWORD* bytes_written)>;
  using ReadDataFn =
      std::function<bool(uint8_t* dst, DWORD max_len, DWORD* bytes_read)>;

  WinHttpTransport() = default;
  ~WinHttpTransport() = default;

  WinHttpTransport(const WinHttpTransport&) = delete;
  WinHttpTransport& operator=(const WinHttpTransport&) = delete;

  // Executes a single validated HTTP request synchronously via WinHTTP.
  TurboHttpResponse Execute(const ValidatedWinHttpRequest& request,
                            RequestContext& ctx);

  // Shuts down cached connections and the shared WinHTTP session handle.
  void Shutdown();

  // Writes the entire request body via write_fn, handling partial writes and
  // cancellation between chunks.
  static bool WriteAllRequestBody(const std::vector<uint8_t>& body,
                                  const RequestContext& ctx,
                                  const WriteDataFn& write_fn,
                                  DWORD* out_win32_error);

  // Reads the response body directly into the result buffer without
  // intermediate chunk copies, enforcing maxResponseBytes and cancellation.
  static ReadBodyResult ReadResponseBodyDirect(
      std::optional<int64_t> content_length,
      std::optional<int64_t> max_response_bytes,
      const RequestContext& ctx,
      const ReadDataFn& read_fn);

 private:
  static bool ConfigureRequestOptions(HINTERNET raw_request,
                                      const ValidatedWinHttpRequest& request,
                                      const char** out_failed_api,
                                      DWORD* out_win32_error);

  TurboSessionManager session_manager_;
};

}  // namespace flutter_turbo_fetch

#endif  // FLUTTER_PLUGIN_WINHTTP_TRANSPORT_H_
