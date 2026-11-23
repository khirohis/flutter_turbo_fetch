#include "turbo_http_executor.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace flutter_turbo_fetch {

namespace {

bool IsValidHttpUrl(const std::string& url) {
  if (url.empty()) return false;
  return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

bool HasInvalidHeaderChars(const std::unordered_map<std::string, std::string>& headers) {
  for (const auto& [key, val] : headers) {
    if (key.find('\r') != std::string::npos || key.find('\n') != std::string::npos) {
      return true;
    }
    if (val.find('\r') != std::string::npos || val.find('\n') != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string Trim(const std::string& str) {
  size_t start = 0;
  while (start < str.size() && std::isspace(static_cast<unsigned char>(str[start]))) {
    ++start;
  }
  size_t end = str.size();
  while (end > start && std::isspace(static_cast<unsigned char>(str[end - 1]))) {
    --end;
  }
  return str.substr(start, end - start);
}

std::string ToLower(const std::string& str) {
  std::string lower = str;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return lower;
}

}  // namespace

TurboHttpExecutor::TurboHttpExecutor(TurboSessionManager& session_manager)
    : session_manager_(session_manager) {}

TurboHttpExecutor::~TurboHttpExecutor() {
  Shutdown();
}

size_t TurboHttpExecutor::active_request_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_requests_.size();
}

bool TurboHttpExecutor::has_active_request(const std::string& request_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_requests_.find(request_id) != active_requests_.end();
}

void TurboHttpExecutor::CancelRequest(const std::string& request_id) {
  std::shared_ptr<LinuxRequestContext> ctx;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = active_requests_.find(request_id);
    if (it != active_requests_.end()) {
      ctx = it->second;
    }
  }

  if (ctx) {
    ctx->is_cancelled.store(true);
    session_manager_.Wakeup();
  }
}

void TurboHttpExecutor::Shutdown() {
  std::thread thread_to_join;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    is_shutdown_.store(true);
    stop_requested_ = true;
    for (auto& [id, ctx] : active_requests_) {
      ctx->is_cancelled.store(true);
    }
    if (event_loop_thread_.joinable()) {
      thread_to_join = std::move(event_loop_thread_);
    }
  }

  session_manager_.Wakeup();

  if (thread_to_join.joinable()) {
    thread_to_join.join();
  }
}

std::pair<std::string, std::string> TurboHttpExecutor::ClassifyError(
    CURLcode curl_code, bool is_cancelled, ExecutionPhase phase,
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
        return {TurboErrorCodes::kReceiveTimeout, "Receive response timed out."};
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

std::pair<std::unordered_map<std::string, std::vector<std::string>>, std::optional<int64_t>>
TurboHttpExecutor::ParseRawHeaders(const std::string& raw_headers) {
  std::unordered_map<std::string, std::vector<std::string>> headers_map;
  std::optional<int64_t> content_length;

  std::istringstream stream(raw_headers);
  std::string line;

  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty() || line.rfind("HTTP/", 0) == 0) {
      continue;
    }

    size_t colon_pos = line.find(':');
    if (colon_pos == std::string::npos || colon_pos == 0) {
      continue;
    }

    std::string key = ToLower(Trim(line.substr(0, colon_pos)));
    std::string val = Trim(line.substr(colon_pos + 1));

    if (key.empty()) continue;

    if (key == "content-length") {
      try {
        content_length = std::stoll(val);
      } catch (...) {
        content_length = std::nullopt;
      }
    }

    headers_map[key].push_back(val);
  }

  return {std::move(headers_map), content_length};
}

TurboHttpResponse TurboHttpExecutor::ValidateAndAssembleChunks(
    int64_t status_code, const std::string& status_message,
    const std::string& raw_headers, const std::vector<uint8_t>& body,
    std::optional<int64_t> max_response_bytes) {
  auto [headers_map, content_length] = ParseRawHeaders(raw_headers);

  const int64_t zero_status = 0;

  if (max_response_bytes.has_value() && *max_response_bytes > 0) {
    const int64_t limit = *max_response_bytes;

    if (content_length.has_value() && *content_length > limit) {
      const std::string err_code = TurboErrorCodes::kMaxResponseBytesExceeded;
      const std::string err_msg =
          "Response Content-Length (" + std::to_string(*content_length) +
          " bytes) exceeded maxResponseBytes (" + std::to_string(limit) +
          " bytes).";
      return TurboHttpResponse(&zero_status, &status_message, &headers_map, nullptr,
                               &err_code, &err_msg);
    }

    if (static_cast<int64_t>(body.size()) > limit) {
      const std::string err_code = TurboErrorCodes::kMaxResponseBytesExceeded;
      const std::string err_msg =
          "Response payload size (" + std::to_string(body.size()) +
          " bytes) exceeded maxResponseBytes (" + std::to_string(limit) +
          " bytes).";
      return TurboHttpResponse(&zero_status, &status_message, &headers_map, nullptr,
                               &err_code, &err_msg);
    }
  }

  return TurboHttpResponse(&status_code, &status_message, &headers_map, &body,
                           nullptr, nullptr);
}

size_t TurboHttpExecutor::HeaderCallback(char* buffer, size_t size,
                                        size_t nitems, void* userdata) {
  size_t total_size = size * nitems;
  auto* ctx = static_cast<LinuxRequestContext*>(userdata);
  if (ctx == nullptr) return total_size;

  std::string line(buffer, total_size);
  ctx->response_headers.append(line);

  if (ctx->max_response_bytes.has_value() && *ctx->max_response_bytes > 0) {
    size_t colon_pos = line.find(':');
    if (colon_pos != std::string::npos) {
      std::string key = ToLower(Trim(line.substr(0, colon_pos)));
      if (key == "content-length") {
        try {
          int64_t len = std::stoll(Trim(line.substr(colon_pos + 1)));
          if (len > *ctx->max_response_bytes) {
            ctx->is_exceeded_limit.store(true);
            return 0;  // Abort transfer
          }
        } catch (...) {}
      }
    }
  }

  return total_size;
}

size_t TurboHttpExecutor::WriteCallback(char* ptr, size_t size, size_t nmemb,
                                       void* userdata) {
  size_t total_size = size * nmemb;
  auto* ctx = static_cast<LinuxRequestContext*>(userdata);
  if (ctx == nullptr) return total_size;

  if (ctx->max_response_bytes.has_value() && *ctx->max_response_bytes > 0) {
    if (static_cast<int64_t>(ctx->response_body.size() + total_size) >
        *ctx->max_response_bytes) {
      ctx->is_exceeded_limit.store(true);
      return 0;  // Abort with CURLE_WRITE_ERROR
    }
  }

  const auto* byte_ptr = reinterpret_cast<const uint8_t*>(ptr);
  ctx->response_body.insert(ctx->response_body.end(), byte_ptr,
                            byte_ptr + total_size);
  return total_size;
}

bool TurboHttpExecutor::CheckPhaseTimeout(
    LinuxRequestContext& ctx, std::chrono::steady_clock::time_point now) {
  if (ctx.timed_out_error_code != nullptr) {
    return true;
  }

  if (ctx.phase == ExecutionPhase::kConnecting) {
    if (ctx.connect_timeout_ms.has_value() && *ctx.connect_timeout_ms > 0) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now - ctx.start_time)
                         .count();
      if (elapsed > *ctx.connect_timeout_ms) {
        ctx.timed_out_error_code = TurboErrorCodes::kConnectionTimeout;
        return true;
      }
    }
  } else if (ctx.phase == ExecutionPhase::kSendingBody) {
    if (ctx.send_timeout_ms.has_value() && *ctx.send_timeout_ms > 0) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now - ctx.connect_end_time)
                         .count();
      if (elapsed > *ctx.send_timeout_ms) {
        ctx.timed_out_error_code = TurboErrorCodes::kSendTimeout;
        return true;
      }
    }
  } else if (ctx.phase == ExecutionPhase::kReceiving) {
    if (ctx.receive_timeout_ms.has_value() && *ctx.receive_timeout_ms > 0) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now - ctx.body_send_end_time)
                         .count();
      if (elapsed > *ctx.receive_timeout_ms) {
        ctx.timed_out_error_code = TurboErrorCodes::kReceiveTimeout;
        return true;
      }
    }
  }

  return false;
}

int TurboHttpExecutor::XferInfoCallback(void* clientp, curl_off_t /*dltotal*/,
                                        curl_off_t dlnow, curl_off_t /*ultotal*/,
                                        curl_off_t ulnow) {
  auto* ctx = static_cast<LinuxRequestContext*>(clientp);
  if (ctx == nullptr) return 0;

  if (ctx->is_cancelled.load() || ctx->is_exceeded_limit.load()) {
    return 1;  // Non-zero aborts transfer (CURLE_ABORTED_BY_CALLBACK)
  }

  auto now = std::chrono::steady_clock::now();

  // Phase transition detection
  if (ctx->phase == ExecutionPhase::kConnecting) {
    if (ulnow > 0 || dlnow > 0) {
      ctx->phase = ExecutionPhase::kSendingBody;
      ctx->connect_end_time = now;
    }
  }

  if (ctx->phase == ExecutionPhase::kSendingBody) {
    if (dlnow > 0) {
      ctx->phase = ExecutionPhase::kReceiving;
      ctx->body_send_end_time = now;
    }
  }

  if (CheckPhaseTimeout(*ctx, now)) {
    return 1;
  }

  return 0;
}

CURL* TurboHttpExecutor::CreateEasyHandle(const TurboHttpRequest& request,
                                          InFlightRequest& in_flight) {
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    return nullptr;
  }

  LinuxRequestContext& ctx = *in_flight.ctx;

  curl_easy_setopt(curl, CURLOPT_URL, request.url().c_str());
  curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
  curl_easy_setopt(curl, CURLOPT_PIPEWAIT, 1L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, request.follow_redirects() ? 1L : 0L);

  if (request.allow_bad_certificates()) {
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  } else {
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  }

  // Method and Body
  const std::string& method = request.method();
  if (method == "POST") {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
  } else if (method == "HEAD") {
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  } else if (method != "GET") {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
  }

  if (request.body() != nullptr && !request.body()->empty()) {
    in_flight.request_body = *request.body();
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,
                     reinterpret_cast<const char*>(in_flight.request_body.data()));
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(in_flight.request_body.size()));
  }

  // Headers (owned by in_flight until handle cleanup)
  for (const auto& [key, val] : request.headers()) {
    std::string header_line = key + ": " + val;
    in_flight.header_list = curl_slist_append(in_flight.header_list, header_line.c_str());
  }
  if (in_flight.header_list != nullptr) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, in_flight.header_list);
  }

  // Timeouts and response limit
  if (request.connect_timeout_ms() != nullptr && *request.connect_timeout_ms() > 0) {
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                     static_cast<long>(*request.connect_timeout_ms()));
    ctx.connect_timeout_ms = *request.connect_timeout_ms();
  }
  if (request.send_timeout_ms() != nullptr && *request.send_timeout_ms() > 0) {
    ctx.send_timeout_ms = *request.send_timeout_ms();
  }
  if (request.receive_timeout_ms() != nullptr && *request.receive_timeout_ms() > 0) {
    ctx.receive_timeout_ms = *request.receive_timeout_ms();
  }
  if (request.max_response_bytes() != nullptr && *request.max_response_bytes() > 0) {
    ctx.max_response_bytes = *request.max_response_bytes();
  }

  // Callbacks
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &TurboHttpExecutor::HeaderCallback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &TurboHttpExecutor::WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &TurboHttpExecutor::XferInfoCallback);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

  ctx.start_time = std::chrono::steady_clock::now();
  ctx.connect_end_time = ctx.start_time;
  ctx.body_send_end_time = ctx.start_time;

  return curl;
}

TurboHttpResponse TurboHttpExecutor::BuildFinalResponse(
    CURL* curl, CURLcode res, LinuxRequestContext& ctx) {
  const int64_t zero_status = 0;

  if (ctx.is_exceeded_limit.load()) {
    const std::string err_code = TurboErrorCodes::kMaxResponseBytesExceeded;
    const std::string err_msg = "Response payload exceeded maxResponseBytes.";
    return TurboHttpResponse(&zero_status, nullptr, nullptr, nullptr, &err_code, &err_msg);
  }

  if (res != CURLE_OK || ctx.is_cancelled.load() || ctx.timed_out_error_code != nullptr) {
    auto [err_code, err_msg] =
        ClassifyError(res, ctx.is_cancelled.load(), ctx.phase, ctx.timed_out_error_code);
    return TurboHttpResponse(&zero_status, nullptr, nullptr, nullptr, &err_code, &err_msg);
  }

  long status_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
  return ValidateAndAssembleChunks(status_code, "OK", ctx.response_headers,
                                   ctx.response_body, ctx.max_response_bytes);
}

void TurboHttpExecutor::EnsureEventLoopStartedLocked() {
  if (event_loop_started_) {
    return;
  }
  CURLM* multi_handle = session_manager_.AcquireMultiHandle();
  event_loop_started_ = true;
  event_loop_thread_ =
      std::thread(&TurboHttpExecutor::RunEventLoop, this, multi_handle);
}

void TurboHttpExecutor::RunEventLoop(CURLM* multi_handle) {
  const int64_t zero_status = 0;
  std::unordered_map<CURL*, std::unique_ptr<InFlightRequest>> in_flight;
  std::vector<std::pair<std::function<void(TurboHttpResponse)>, TurboHttpResponse>>
      completed_callbacks;

  auto flush_callbacks = [&]() {
    for (auto& [cb, response] : completed_callbacks) {
      cb(std::move(response));
    }
    completed_callbacks.clear();
  };

  while (true) {
    std::deque<PendingRequest> batch;
    bool should_stop = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      batch.swap(pending_requests_);
      should_stop = stop_requested_;
    }

    // 1. Register new pending requests onto the CURLM handle.
    for (auto& pending : batch) {
      const std::string request_id = pending.request.request_id();
      if (pending.ctx->is_cancelled.load() || should_stop) {
        {
          std::lock_guard<std::mutex> lock(mutex_);
          active_requests_.erase(request_id);
        }
        const std::string err_code = TurboErrorCodes::kCancel;
        const std::string err_msg = "Request was cancelled.";
        completed_callbacks.emplace_back(
            std::move(pending.on_result),
            TurboHttpResponse(&zero_status, nullptr, nullptr, nullptr, &err_code, &err_msg));
        continue;
      }

      if (multi_handle == nullptr) {
        {
          std::lock_guard<std::mutex> lock(mutex_);
          active_requests_.erase(request_id);
        }
        const std::string err_code = TurboErrorCodes::kUnknown;
        const std::string err_msg = "Failed to initialize CURLM handle.";
        completed_callbacks.emplace_back(
            std::move(pending.on_result),
            TurboHttpResponse(&zero_status, nullptr, nullptr, nullptr, &err_code, &err_msg));
        continue;
      }

      auto in_flight_req = std::make_unique<InFlightRequest>();
      in_flight_req->request_id = request_id;
      in_flight_req->ctx = pending.ctx;
      in_flight_req->on_result = std::move(pending.on_result);

      CURL* easy = CreateEasyHandle(pending.request, *in_flight_req);
      if (easy == nullptr) {
        {
          std::lock_guard<std::mutex> lock(mutex_);
          active_requests_.erase(request_id);
        }
        const std::string err_code = TurboErrorCodes::kUnknown;
        const std::string err_msg = "Failed to initialize CURL handle.";
        completed_callbacks.emplace_back(
            std::move(in_flight_req->on_result),
            TurboHttpResponse(&zero_status, nullptr, nullptr, nullptr, &err_code, &err_msg));
        continue;
      }

      CURLMcode add_res = curl_multi_add_handle(multi_handle, easy);
      if (add_res != CURLM_OK) {
        curl_easy_cleanup(easy);
        {
          std::lock_guard<std::mutex> lock(mutex_);
          active_requests_.erase(request_id);
        }
        const std::string err_code = TurboErrorCodes::kUnknown;
        const std::string err_msg = curl_multi_strerror(add_res);
        completed_callbacks.emplace_back(
            std::move(in_flight_req->on_result),
            TurboHttpResponse(&zero_status, nullptr, nullptr, nullptr, &err_code, &err_msg));
        continue;
      }

      in_flight.emplace(easy, std::move(in_flight_req));
    }

    // 2. Abort any in-flight handles that were cancelled, timed out, or during shutdown.
    auto now = std::chrono::steady_clock::now();
    for (auto it = in_flight.begin(); it != in_flight.end();) {
      CURL* easy = it->first;
      auto& entry = it->second;
      bool timed_out = CheckPhaseTimeout(*entry->ctx, now);
      if (entry->ctx->is_cancelled.load() || should_stop || timed_out) {
        if (should_stop && !timed_out) {
          entry->ctx->is_cancelled.store(true);
        }
        TurboHttpResponse response =
            BuildFinalResponse(easy, CURLE_ABORTED_BY_CALLBACK, *entry->ctx);
        curl_multi_remove_handle(multi_handle, easy);
        curl_easy_cleanup(easy);
        {
          std::lock_guard<std::mutex> lock(mutex_);
          active_requests_.erase(entry->request_id);
        }
        completed_callbacks.emplace_back(std::move(entry->on_result),
                                         std::move(response));
        it = in_flight.erase(it);
      } else {
        ++it;
      }
    }

    flush_callbacks();

    if (should_stop && in_flight.empty()) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (pending_requests_.empty()) {
        break;
      }
      continue;
    }

    // 3. Drive active transfers and harvest completed messages.
    if (multi_handle != nullptr && !in_flight.empty()) {
      int still_running = 0;
      curl_multi_perform(multi_handle, &still_running);

      int msgs_left = 0;
      while (CURLMsg* msg = curl_multi_info_read(multi_handle, &msgs_left)) {
        if (msg->msg != CURLMSG_DONE) {
          continue;
        }
        CURL* easy = msg->easy_handle;
        CURLcode res = msg->data.result;
        auto it = in_flight.find(easy);
        if (it == in_flight.end()) {
          curl_multi_remove_handle(multi_handle, easy);
          curl_easy_cleanup(easy);
          continue;
        }
        auto entry = std::move(it->second);
        in_flight.erase(it);

        TurboHttpResponse response = BuildFinalResponse(easy, res, *entry->ctx);
        curl_multi_remove_handle(multi_handle, easy);
        curl_easy_cleanup(easy);
        {
          std::lock_guard<std::mutex> lock(mutex_);
          active_requests_.erase(entry->request_id);
        }
        completed_callbacks.emplace_back(std::move(entry->on_result),
                                         std::move(response));
      }

      flush_callbacks();
    }

    // 4. Wait efficiently for network I/O or wakeup notifications.
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!pending_requests_.empty() || stop_requested_) {
        continue;
      }
    }

    if (multi_handle != nullptr) {
      int poll_timeout_ms = in_flight.empty() ? 1000 : 50;
      curl_multi_poll(multi_handle, nullptr, 0, poll_timeout_ms, nullptr);
    } else {
      break;
    }
  }
}

void TurboHttpExecutor::Execute(
    const TurboHttpRequest& request,
    std::function<void(TurboHttpResponse)> on_result) {
  const int64_t zero_status = 0;
  // Validate request parameters before entering active registry
  if (!IsValidHttpUrl(request.url())) {
    const std::string err_code = TurboErrorCodes::kConnectionError;
    const std::string err_msg = "Invalid URL: " + request.url();
    on_result(TurboHttpResponse(&zero_status, nullptr, nullptr, nullptr, &err_code, &err_msg));
    return;
  }

  if (HasInvalidHeaderChars(request.headers())) {
    const std::string err_code = TurboErrorCodes::kConnectionError;
    const std::string err_msg = "Headers contain invalid newline characters.";
    on_result(TurboHttpResponse(&zero_status, nullptr, nullptr, nullptr, &err_code, &err_msg));
    return;
  }

  auto ctx = std::make_shared<LinuxRequestContext>();

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_shutdown_.load()) {
      const std::string err_code = TurboErrorCodes::kCancel;
      const std::string err_msg = "Executor is shut down.";
      on_result(TurboHttpResponse(&zero_status, nullptr, nullptr, nullptr, &err_code, &err_msg));
      return;
    }
    active_requests_[request.request_id()] = ctx;
    pending_requests_.push_back(PendingRequest{request, ctx, std::move(on_result)});
    EnsureEventLoopStartedLocked();
  }

  session_manager_.Wakeup();
}

}  // namespace flutter_turbo_fetch
