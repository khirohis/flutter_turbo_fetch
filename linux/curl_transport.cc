#include "curl_transport.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

namespace flutter_turbo_fetch {

namespace {

constexpr int kMaxPollTimeoutMs = 1000;

struct EasyPoolGuard {
  std::vector<CURL*>& pool;
  ~EasyPoolGuard() {
    for (CURL* easy : pool) {
      if (easy != nullptr) {
        curl_easy_cleanup(easy);
      }
    }
    pool.clear();
  }
};

bool TryMultiplySize(size_t a, size_t b, size_t* out) {
  if (a != 0 && b > std::numeric_limits<size_t>::max() / a) {
    return false;
  }
  *out = a * b;
  return true;
}

template <typename ReturnT, typename Fn>
ReturnT RunGuardedCallback(CurlRequestContext* ctx,
                           ReturnT error_return,
                           const char* fallback_msg,
                           Fn&& fn) {
  try {
    return fn();
  } catch (const std::exception& e) {
    ctx->RecordCallbackException(e.what());
    return error_return;
  } catch (...) {
    ctx->RecordCallbackException(fallback_msg);
    return error_return;
  }
}

}  // namespace

CurlMultiTransport::CurlMultiTransport(TurboSessionManager& session_manager)
    : session_manager_(session_manager) {}

CurlMultiTransport::~CurlMultiTransport() { Shutdown(); }

bool CurlMultiTransport::Enqueue(
    ValidatedCurlRequest request,
    std::shared_ptr<CurlRequestContext> ctx,
    std::function<void(TurboHttpResponse)> on_complete,
    bool* out_should_wakeup) {
  if (out_should_wakeup != nullptr) {
    *out_should_wakeup = false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (stop_requested_) {
    return false;
  }
  // Only issue a wakeup syscall on the empty -> non-empty transition to
  // avoid redundant wakeups during burst concurrent submissions.
  if (out_should_wakeup != nullptr) {
    *out_should_wakeup = pending_requests_.empty();
  }
  pending_requests_.push_back(PendingRequest{
      std::move(request), std::move(ctx), std::move(on_complete)});
  has_pending_or_stop_.store(true, std::memory_order_release);
  EnsureEventLoopStartedLocked();
  return true;
}

void CurlMultiTransport::Wakeup() { session_manager_.Wakeup(); }

void CurlMultiTransport::WakeupForCancel() {
  cancel_requested_.store(true, std::memory_order_release);
  session_manager_.Wakeup();
}

void CurlMultiTransport::Shutdown() {
  std::thread thread_to_join;
  std::deque<PendingRequest> orphaned_pending;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_requested_ = true;
    has_pending_or_stop_.store(true, std::memory_order_release);
    if (event_loop_thread_.joinable()) {
      thread_to_join = std::move(event_loop_thread_);
    } else {
      orphaned_pending.swap(pending_requests_);
    }
  }

  session_manager_.Wakeup();

  if (thread_to_join.joinable()) {
    thread_to_join.join();
    std::lock_guard<std::mutex> lock(mutex_);
    orphaned_pending.swap(pending_requests_);
  }

  for (auto& pending : orphaned_pending) {
    try {
      pending.on_complete(MakeCancelResponse());
    } catch (...) {
    }
  }
}

// static
struct curl_slist* CurlMultiTransport::BuildCurlHeaderList(
    const ValidatedCurlRequest& request) {
  struct curl_slist* list = nullptr;
  auto append_or_cleanup = [&](const char* item) -> bool {
    struct curl_slist* next = curl_slist_append(list, item);
    if (next == nullptr) {
      curl_slist_free_all(list);
      list = nullptr;
      return false;
    }
    list = next;
    return true;
  };

  std::string line;
  for (const auto& [key, val] : request.headers) {
    line.clear();
    line.reserve(key.size() + 2 + val.size());
    line.append(key);
    line.append(": ");
    line.append(val);
    if (!append_or_cleanup(line.c_str())) {
      return nullptr;
    }
  }
  // Suppress libcurl's automatic "Expect: 100-continue" delay unless requested.
  if (!request.has_expect_header && !append_or_cleanup("Expect:")) {
    return nullptr;
  }
  return list;
}

// static
TurboHttpResponse CurlMultiTransport::BuildFinalResponse(
    long status_code,
    CURLcode res,
    CurlRequestContext& ctx) {
  if (ctx.is_exceeded_limit.load()) {
    const int64_t limit = ctx.max_response_bytes.value_or(0);
    return MakeMaxResponseBytesExceededResponse(limit,
                                                ctx.exceeded_observed_bytes);
  }

  if (ctx.callback_exception.load()) {
    const std::string_view msg =
        ctx.callback_exception_message.empty()
            ? std::string_view("Unexpected native callback exception.")
            : std::string_view(ctx.callback_exception_message);
    return MakeErrorResponse(TurboErrorCodes::kUnknown, msg);
  }

  if (res != CURLE_OK || ctx.is_cancelled.load() ||
      ctx.timed_out_error_code != nullptr) {
    auto [err_code, err_msg] = ClassifyCurlError(
        res, ctx.is_cancelled.load(), ctx.phase, ctx.timed_out_error_code);
    return MakeErrorResponse(err_code, err_msg);
  }

  if (ctx.max_response_bytes.has_value() &&
      static_cast<uint64_t>(ctx.response_body.size()) >
          static_cast<uint64_t>(*ctx.max_response_bytes)) {
    const int64_t actual = static_cast<int64_t>(ctx.response_body.size());
    ctx.response_body.clear();
    return MakeMaxResponseBytesExceededResponse(*ctx.max_response_bytes,
                                                actual);
  }

  return TurboHttpResponse(static_cast<int64_t>(status_code),
                           std::move(ctx.status_message),
                           std::move(ctx.response_headers),
                           std::move(ctx.response_body));
}

// static
size_t CurlMultiTransport::HeaderCallback(char* buffer,
                                          size_t size,
                                          size_t nitems,
                                          void* userdata) {
  size_t total_size = 0;
  if (!TryMultiplySize(size, nitems, &total_size)) {
    return 0;
  }
  auto* ctx = static_cast<CurlRequestContext*>(userdata);
  if (ctx == nullptr) {
    return total_size;
  }
  return RunGuardedCallback<size_t>(
      ctx, 0, "Unexpected exception in header callback.", [&]() -> size_t {
        const auto now = ctx->has_receive_timeout()
                             ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};
        return ctx->OnHeaderLine(std::string_view(buffer, total_size), now)
                   ? total_size
                   : 0;
      });
}

// static
size_t CurlMultiTransport::WriteCallback(char* ptr,
                                         size_t size,
                                         size_t nmemb,
                                         void* userdata) {
  size_t total_size = 0;
  if (!TryMultiplySize(size, nmemb, &total_size)) {
    return 0;
  }
  auto* ctx = static_cast<CurlRequestContext*>(userdata);
  if (ctx == nullptr) {
    return total_size;
  }
  return RunGuardedCallback<size_t>(
      ctx, 0, "Unexpected exception in write callback.", [&]() -> size_t {
        const auto now = ctx->has_receive_timeout()
                             ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};
        return ctx->OnBodyChunk(reinterpret_cast<const uint8_t*>(ptr),
                                total_size, now)
                   ? total_size
                   : 0;
      });
}

// static
int CurlMultiTransport::XferInfoCallback(void* clientp,
                                         curl_off_t /*dltotal*/,
                                         curl_off_t dlnow,
                                         curl_off_t ultotal,
                                         curl_off_t ulnow) {
  auto* ctx = static_cast<CurlRequestContext*>(clientp);
  if (ctx == nullptr) {
    return 0;
  }
  return RunGuardedCallback<int>(
      ctx, 1, "Unexpected exception in progress callback.", [&]() -> int {
        bool connect_completed = false;
        if (ctx->phase == ExecutionPhase::kConnecting &&
            ctx->easy_handle != nullptr) {
          // Use PRETRANSFER_TIME_T so TLS handshake stays within kConnecting
          // (connectionTimeout) rather than transitioning early after TCP connect.
          curl_off_t pretransfer_time_us = 0;
          if (curl_easy_getinfo(ctx->easy_handle, CURLINFO_PRETRANSFER_TIME_T,
                                &pretransfer_time_us) == CURLE_OK &&
              pretransfer_time_us > 0) {
            connect_completed = true;
          }
        }
        return ctx->OnProgress(dlnow, ulnow, ultotal, connect_completed,
                               std::chrono::steady_clock::now())
                   ? 0
                   : 1;
      });
}

// static
CURL* CurlMultiTransport::AcquireEasyHandle(std::vector<CURL*>& pool) {
  if (!pool.empty()) {
    CURL* easy = pool.back();
    pool.pop_back();
    return easy;
  }
  return curl_easy_init();
}

// static
void CurlMultiTransport::ReleaseEasyHandle(std::vector<CURL*>& pool,
                                           CURL* easy,
                                           bool reusable) {
  if (easy == nullptr) {
    return;
  }
  if (reusable && pool.size() < kMaxPooledEasyHandles) {
    // Reset immediately so no option retains pointers into finished contexts.
    curl_easy_reset(easy);
    pool.push_back(easy);
    return;
  }
  curl_easy_cleanup(easy);
}

// static
CURL* CurlMultiTransport::ConfigureEasyHandle(
    CURL* curl,
    ValidatedCurlRequest& request,
    InFlightRequest& in_flight,
    std::chrono::steady_clock::time_point now) {
  if (curl == nullptr) {
    return nullptr;
  }

  CurlRequestContext& ctx = *in_flight.ctx;
  ctx.InitializeFromRequest(request, now);
  ctx.easy_handle = curl;
  in_flight.has_timeout = ctx.has_any_timeout();

  curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
#if LIBCURL_VERSION_NUM >= 0x075500
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS,
                   CURLPROTO_HTTP | CURLPROTO_HTTPS);
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS,
                   CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
  curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
  curl_easy_setopt(curl, CURLOPT_PIPEWAIT, 1L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, kCurlReceiveBufferSizeBytes);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,
                   request.follow_redirects ? 1L : 0L);
  if (request.follow_redirects) {
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, kMaxRedirects);
  }

  if (!request.has_accept_encoding_header) {
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  }

  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,
                   request.allow_bad_certificates ? 0L : 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST,
                   request.allow_bad_certificates ? 0L : 2L);

  const bool has_non_empty_body =
      request.body.has_value() && !request.body->empty();
  const bool is_post = EqualsIgnoreCaseAscii(request.method, "POST");
  const bool is_head = EqualsIgnoreCaseAscii(request.method, "HEAD");
  const bool is_get = EqualsIgnoreCaseAscii(request.method, "GET");

  if (is_post) {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    if (!has_non_empty_body) {
      // Explicitly set zero body length so libcurl never reads from stdin.
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                       static_cast<curl_off_t>(0));
    }
  } else if (is_head) {
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  } else if (!is_get) {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
  } else if (has_non_empty_body) {
    // Preserve GET method when sending a body; CURLOPT_POSTFIELDS alone would
    // otherwise implicitly switch the method to POST.
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "GET");
  }

  if (has_non_empty_body && !is_head) {
    in_flight.request_body = std::move(*request.body);
    curl_easy_setopt(
        curl, CURLOPT_POSTFIELDS,
        reinterpret_cast<const char*>(in_flight.request_body.data()));
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(in_flight.request_body.size()));
  }

  const bool expects_header_list =
      !request.headers.empty() || !request.has_expect_header;
  in_flight.header_list = BuildCurlHeaderList(request);
  if (expects_header_list && in_flight.header_list == nullptr) {
    ctx.easy_handle = nullptr;
    return nullptr;
  }
  if (in_flight.header_list != nullptr) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, in_flight.header_list);
  }

  if (ctx.connect_timeout_ms.has_value() && *ctx.connect_timeout_ms > 0) {
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                     ClampCurlTimeoutMs(*ctx.connect_timeout_ms));
  }

  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,
                   &CurlMultiTransport::HeaderCallback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                   &CurlMultiTransport::WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

  // Only enable the per-chunk progress callback when phase timeouts are active;
  // cancellations are handled directly by the event loop on wakeup.
  if (in_flight.has_timeout) {
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                     &CurlMultiTransport::XferInfoCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  } else {
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
  }

  return curl;
}

// static
int CurlMultiTransport::ComputePollTimeoutMs(
    const EventLoopState& state,
    std::chrono::steady_clock::time_point now) {
  if (state.in_flight.empty() || state.timed_in_flight_count == 0) {
    return kMaxPollTimeoutMs;
  }

  int timeout_ms = kMaxPollTimeoutMs;
  for (const auto& [easy, entry] : state.in_flight) {
    (void)easy;
    if (!entry->has_timeout) {
      continue;
    }
    std::optional<int> remaining = entry->ctx->RemainingTimeoutMs(now);
    if (remaining.has_value()) {
      timeout_ms = std::min(timeout_ms, *remaining);
      if (timeout_ms == 0) {
        return 0;
      }
    }
  }
  return timeout_ms;
}

// static
void CurlMultiTransport::FlushCallbacks(EventLoopState& state) {
  for (auto& [cb, response] : state.completed_callbacks) {
    try {
      cb(std::move(response));
    } catch (...) {
    }
  }
  state.completed_callbacks.clear();
}

// static
void CurlMultiTransport::CompleteInFlightEntry(
    EventLoopState& state,
    CURL* easy,
    std::unique_ptr<InFlightRequest> entry,
    CURLcode res) {
  long status_code = 0;
  curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status_code);
  TurboHttpResponse response =
      BuildFinalResponse(status_code, res, *entry->ctx);
  entry->ctx->easy_handle = nullptr;
  curl_multi_remove_handle(state.multi_handle, easy);
  ReleaseEasyHandle(state.easy_pool, easy, /*reusable=*/true);
  if (entry->has_timeout && state.timed_in_flight_count > 0) {
    --state.timed_in_flight_count;
  }
  state.completed_callbacks.emplace_back(std::move(entry->on_complete),
                                         std::move(response));
}

// static
void CurlMultiTransport::RegisterPendingBatch(EventLoopState& state,
                                              std::deque<PendingRequest>& batch,
                                              bool should_stop) {
  if (batch.empty()) {
    return;
  }

  const auto batch_now = std::chrono::steady_clock::now();
  for (auto& pending : batch) {
    if (pending.ctx->is_cancelled.load() || should_stop) {
      state.completed_callbacks.emplace_back(std::move(pending.on_complete),
                                             MakeCancelResponse());
      continue;
    }

    if (state.multi_handle == nullptr) {
      state.completed_callbacks.emplace_back(
          std::move(pending.on_complete),
          MakeErrorResponse(TurboErrorCodes::kUnknown,
                            "Failed to initialize CURLM handle."));
      continue;
    }

    auto in_flight_req = std::make_unique<InFlightRequest>();
    in_flight_req->ctx = std::move(pending.ctx);
    in_flight_req->on_complete = std::move(pending.on_complete);

    CURL* acquired_easy = AcquireEasyHandle(state.easy_pool);
    CURL* easy = ConfigureEasyHandle(acquired_easy, pending.request,
                                     *in_flight_req, batch_now);
    if (easy == nullptr) {
      ReleaseEasyHandle(state.easy_pool, acquired_easy, /*reusable=*/false);
      state.completed_callbacks.emplace_back(
          std::move(in_flight_req->on_complete),
          MakeErrorResponse(TurboErrorCodes::kUnknown,
                            "Failed to initialize CURL handle."));
      continue;
    }

    CURLMcode add_res = curl_multi_add_handle(state.multi_handle, easy);
    if (add_res != CURLM_OK) {
      in_flight_req->ctx->easy_handle = nullptr;
      ReleaseEasyHandle(state.easy_pool, easy, /*reusable=*/false);
      state.completed_callbacks.emplace_back(
          std::move(in_flight_req->on_complete),
          MakeErrorResponse(TurboErrorCodes::kUnknown,
                            curl_multi_strerror(add_res)));
      continue;
    }

    if (in_flight_req->has_timeout) {
      ++state.timed_in_flight_count;
    }
    state.in_flight.emplace(easy, std::move(in_flight_req));
  }
}

// static
void CurlMultiTransport::AbortInactiveOrTimedOut(EventLoopState& state,
                                                 bool should_stop,
                                                 bool cancel_flag) {
  if (state.in_flight.empty() ||
      (!should_stop && !cancel_flag && state.timed_in_flight_count == 0)) {
    return;
  }

  const auto now = (state.timed_in_flight_count > 0)
                       ? std::chrono::steady_clock::now()
                       : std::chrono::steady_clock::time_point{};
  for (auto it = state.in_flight.begin(); it != state.in_flight.end();) {
    CURL* easy = it->first;
    auto& entry = it->second;
    const bool timed_out =
        entry->has_timeout && entry->ctx->CheckPhaseTimeout(now);
    if (entry->ctx->is_cancelled.load() || should_stop || timed_out) {
      if (should_stop && !timed_out) {
        entry->ctx->is_cancelled.store(true);
      }
      auto owned_entry = std::move(entry);
      it = state.in_flight.erase(it);
      CompleteInFlightEntry(state, easy, std::move(owned_entry),
                            CURLE_ABORTED_BY_CALLBACK);
    } else {
      ++it;
    }
  }
}

// static
void CurlMultiTransport::HarvestCompletedTransfers(EventLoopState& state) {
  if (state.multi_handle == nullptr || state.in_flight.empty()) {
    return;
  }

  int still_running = 0;
  curl_multi_perform(state.multi_handle, &still_running);

  int msgs_left = 0;
  while (CURLMsg* msg = curl_multi_info_read(state.multi_handle, &msgs_left)) {
    if (msg->msg != CURLMSG_DONE) {
      continue;
    }
    CURL* easy = msg->easy_handle;
    CURLcode res = msg->data.result;
    auto it = state.in_flight.find(easy);
    if (it == state.in_flight.end()) {
      curl_multi_remove_handle(state.multi_handle, easy);
      ReleaseEasyHandle(state.easy_pool, easy, /*reusable=*/false);
      continue;
    }
    auto entry = std::move(it->second);
    state.in_flight.erase(it);
    CompleteInFlightEntry(state, easy, std::move(entry), res);
  }
}

void CurlMultiTransport::EnsureEventLoopStartedLocked() {
  if (event_loop_started_) {
    return;
  }
  CURLM* multi_handle = session_manager_.AcquireMultiHandle();
  event_loop_started_ = true;
  event_loop_thread_ =
      std::thread(&CurlMultiTransport::RunEventLoop, this, multi_handle);
}

void CurlMultiTransport::RunEventLoop(CURLM* multi_handle) {
  EventLoopState state;
  state.multi_handle = multi_handle;
  state.easy_pool.reserve(kMaxPooledEasyHandles);
  EasyPoolGuard easy_pool_guard{state.easy_pool};

  bool should_stop = false;

  while (true) {
    std::deque<PendingRequest> batch;
    if (has_pending_or_stop_.exchange(false, std::memory_order_acq_rel)) {
      std::lock_guard<std::mutex> lock(mutex_);
      batch.swap(pending_requests_);
      if (stop_requested_) {
        should_stop = true;
      }
    }

    // 1. Register newly queued requests onto the CURLM handle.
    RegisterPendingBatch(state, batch, should_stop);

    // 2. Abort cancelled, timed-out, or shutting-down transfers.
    const bool cancel_flag =
        cancel_requested_.exchange(false, std::memory_order_acq_rel);
    AbortInactiveOrTimedOut(state, should_stop, cancel_flag);
    FlushCallbacks(state);

    if (should_stop && state.in_flight.empty()) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (pending_requests_.empty()) {
        break;
      }
      has_pending_or_stop_.store(true, std::memory_order_release);
      continue;
    }

    // 3. Drive active transfers and harvest completed messages.
    HarvestCompletedTransfers(state);
    FlushCallbacks(state);

    // 4. Wait for network I/O, nearest phase deadline, or wakeup.
    if (has_pending_or_stop_.load(std::memory_order_acquire) ||
        cancel_requested_.load(std::memory_order_acquire)) {
      continue;
    }

    if (state.multi_handle == nullptr) {
      std::deque<PendingRequest> remaining;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = true;
        remaining.swap(pending_requests_);
      }
      RegisterPendingBatch(state, remaining, /*should_stop=*/false);
      FlushCallbacks(state);
      break;
    }

    const auto poll_now = (state.timed_in_flight_count > 0)
                              ? std::chrono::steady_clock::now()
                              : std::chrono::steady_clock::time_point{};
    const int poll_timeout_ms = ComputePollTimeoutMs(state, poll_now);
    if (poll_timeout_ms == 0) {
      continue;
    }
    curl_multi_poll(state.multi_handle, nullptr, 0, poll_timeout_ms, nullptr);
  }
}

}  // namespace flutter_turbo_fetch
