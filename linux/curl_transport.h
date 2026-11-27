#ifndef FLUTTER_PLUGIN_CURL_TRANSPORT_H_
#define FLUTTER_PLUGIN_CURL_TRANSPORT_H_

#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "curl_helpers.h"
#include "curl_request_context.h"
#include "turbo_session_manager.h"

namespace flutter_turbo_fetch {

class CurlMultiTransport {
 public:
  static constexpr size_t kMaxPooledEasyHandles = 32;
  static constexpr long kCurlReceiveBufferSizeBytes = 64 * 1024;
  static constexpr long kMaxRedirects = 20L;

  explicit CurlMultiTransport(TurboSessionManager& session_manager);
  ~CurlMultiTransport();

  CurlMultiTransport(const CurlMultiTransport&) = delete;
  CurlMultiTransport& operator=(const CurlMultiTransport&) = delete;

  // Enqueues a validated request without invoking Wakeup() (caller must call
  // Wakeup() outside its own mutex when *out_should_wakeup is true).
  // Returns false if transport has already been shut down.
  bool Enqueue(ValidatedCurlRequest request,
               std::shared_ptr<CurlRequestContext> ctx,
               std::function<void(TurboHttpResponse)> on_complete,
               bool* out_should_wakeup);

  // Wakes curl_multi_poll() outside caller locks.
  void Wakeup();

  // Marks that at least one active request was cancelled and wakes curl_multi_poll().
  void WakeupForCancel();

  // Cancels all pending/in-flight transfers and joins the event-loop thread.
  void Shutdown();

  // Builds the curl_slist of headers (including Expect: suppression when needed).
  static struct curl_slist* BuildCurlHeaderList(
      const ValidatedCurlRequest& request);

  // Builds the final TurboHttpResponse from transfer outcome and context.
  static TurboHttpResponse BuildFinalResponse(long status_code,
                                              CURLcode res,
                                              CurlRequestContext& ctx);

 private:
  struct PendingRequest {
    ValidatedCurlRequest request;
    std::shared_ptr<CurlRequestContext> ctx;
    std::function<void(TurboHttpResponse)> on_complete;
  };

  struct InFlightRequest {
    std::shared_ptr<CurlRequestContext> ctx;
    std::vector<uint8_t> request_body;
    struct curl_slist* header_list = nullptr;
    std::function<void(TurboHttpResponse)> on_complete;
    bool has_timeout = false;

    ~InFlightRequest() {
      if (header_list != nullptr) {
        curl_slist_free_all(header_list);
        header_list = nullptr;
      }
    }
  };

  struct EventLoopState {
    CURLM* multi_handle = nullptr;
    std::unordered_map<CURL*, std::unique_ptr<InFlightRequest>> in_flight;
    std::vector<CURL*> easy_pool;
    std::vector<
        std::pair<std::function<void(TurboHttpResponse)>, TurboHttpResponse>>
        completed_callbacks;
    size_t timed_in_flight_count = 0;
  };

  void EnsureEventLoopStartedLocked();
  void RunEventLoop(CURLM* multi_handle);

  static void RegisterPendingBatch(EventLoopState& state,
                                   std::deque<PendingRequest>& batch,
                                   bool should_stop);
  static void AbortInactiveOrTimedOut(EventLoopState& state,
                                      bool should_stop,
                                      bool cancel_flag);
  static void HarvestCompletedTransfers(EventLoopState& state);
  static void CompleteInFlightEntry(EventLoopState& state,
                                    CURL* easy,
                                    std::unique_ptr<InFlightRequest> entry,
                                    CURLcode res);
  static void FlushCallbacks(EventLoopState& state);

  static CURL* AcquireEasyHandle(std::vector<CURL*>& pool);
  static void ReleaseEasyHandle(std::vector<CURL*>& pool,
                                CURL* easy,
                                bool reusable);
  static CURL* ConfigureEasyHandle(CURL* curl,
                                   ValidatedCurlRequest& request,
                                   InFlightRequest& in_flight,
                                   std::chrono::steady_clock::time_point now);
  static int ComputePollTimeoutMs(const EventLoopState& state,
                                  std::chrono::steady_clock::time_point now);

  static size_t HeaderCallback(char* buffer,
                               size_t size,
                               size_t nitems,
                               void* userdata);
  static size_t WriteCallback(char* ptr,
                              size_t size,
                              size_t nmemb,
                              void* userdata);
  static int XferInfoCallback(void* clientp,
                              curl_off_t dltotal,
                              curl_off_t dlnow,
                              curl_off_t ultotal,
                              curl_off_t ulnow);

  TurboSessionManager& session_manager_;

  std::mutex mutex_;
  std::deque<PendingRequest> pending_requests_;
  std::thread event_loop_thread_;
  bool event_loop_started_ = false;
  bool stop_requested_ = false;
  std::atomic<bool> has_pending_or_stop_{false};
  std::atomic<bool> cancel_requested_{false};
};

}  // namespace flutter_turbo_fetch

#endif  // FLUTTER_PLUGIN_CURL_TRANSPORT_H_
