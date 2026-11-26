#include "turbo_http_executor.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace flutter_turbo_fetch {

namespace {

constexpr DWORD kReadBufferSizeBytes = 64 * 1024;
constexpr int64_t kMaxPreallocBytes = 64 * 1024 * 1024;
constexpr size_t kStatusTextStackChars = 64;
constexpr size_t kRawHeadersStackChars = 2048;

TurboHttpResponse MakeCancelResponse() {
  return MakeErrorResponse("cancel", "Request was cancelled.");
}

TurboHttpResponse MakeWinHttpError(const RequestContext& ctx,
                                   RequestPhase phase,
                                   const char* api_name,
                                   DWORD err) {
  return MakeErrorResponse(
      ClassifyWinHttpError(ctx.is_cancelled.load(), err, phase),
      std::string(api_name) + " failed (error " + std::to_string(err) + ").");
}

ReadBodyResult MakeCancelReadResult() {
  ReadBodyResult result;
  result.error_code = "cancel";
  result.error_message = "Request was cancelled.";
  return result;
}

size_t ComputeNextBufferSize(size_t current,
                             std::optional<int64_t> safe_max) {
  const size_t max_size_t = (std::numeric_limits<size_t>::max)();
  const size_t doubled =
      (current <= max_size_t / 2) ? (current * 2) : max_size_t;
  const size_t plus_chunk =
      (current <= max_size_t - static_cast<size_t>(kReadBufferSizeBytes))
          ? (current + static_cast<size_t>(kReadBufferSizeBytes))
          : max_size_t;
  size_t grown = std::max<size_t>(doubled, plus_chunk);
  if (safe_max.has_value() &&
      *safe_max < (std::numeric_limits<int64_t>::max)()) {
    grown = std::min<size_t>(grown, static_cast<size_t>(*safe_max + 1));
  }
  return grown;
}

}  // namespace

TurboHttpExecutor::TurboHttpExecutor() { InitThreadPool(); }

TurboHttpExecutor::TurboHttpExecutor(TransportOverride transport_override)
    : transport_override_(std::move(transport_override)) {
  InitThreadPool();
}

TurboHttpExecutor::~TurboHttpExecutor() { Shutdown(); }

void TurboHttpExecutor::InitThreadPool() {
  pool_ = CreateThreadpool(nullptr);
  if (pool_ != nullptr) {
    SetThreadpoolThreadMaximum(pool_, 64);
    SetThreadpoolThreadMinimum(pool_, 1);
  }
  cleanup_group_ = CreateThreadpoolCleanupGroup();
  InitializeThreadpoolEnvironment(&callback_env_);
  env_initialized_ = true;
  if (pool_ != nullptr) {
    SetThreadpoolCallbackPool(&callback_env_, pool_);
  }
  if (cleanup_group_ != nullptr) {
    SetThreadpoolCallbackCleanupGroup(&callback_env_, cleanup_group_, nullptr);
  }
}

// static
TurboHttpExecutor::RequestParams TurboHttpExecutor::BuildRequestParams(
    const TurboHttpRequest& request) {
  RequestParams params;
  params.request_id = request.request_id();
  params.method = request.method();
  if (request.body() != nullptr) {
    params.body = *request.body();
  }
  if (request.connect_timeout_ms() != nullptr) {
    params.connect_timeout_ms = *request.connect_timeout_ms();
  }
  if (request.send_timeout_ms() != nullptr) {
    params.send_timeout_ms = *request.send_timeout_ms();
  }
  if (request.receive_timeout_ms() != nullptr) {
    params.receive_timeout_ms = *request.receive_timeout_ms();
  }
  if (request.max_response_bytes() != nullptr) {
    params.max_response_bytes = *request.max_response_bytes();
  }
  params.follow_redirects = request.follow_redirects();
  params.allow_bad_certificates = request.allow_bad_certificates();
  return params;
}

// static
void TurboHttpExecutor::ConfigureRequestOptions(HINTERNET raw_request,
                                                const RequestParams& params,
                                                bool is_https) {
  const int connect_ms = ClampTimeoutMs(params.connect_timeout_ms, 60000);
  const int send_ms = ClampTimeoutMs(params.send_timeout_ms, 30000);
  const int receive_ms = ClampTimeoutMs(params.receive_timeout_ms, 30000);
  WinHttpSetTimeouts(raw_request, connect_ms, connect_ms, send_ms, receive_ms);

  if (!params.follow_redirects) {
    DWORD disable_flag = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(raw_request, WINHTTP_OPTION_DISABLE_FEATURE, &disable_flag,
                     sizeof(disable_flag));
  }

  if (params.allow_bad_certificates && is_https) {
    DWORD sec_flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                      SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(raw_request, WINHTTP_OPTION_SECURITY_FLAGS, &sec_flags,
                     sizeof(sec_flags));
  }
}

void TurboHttpExecutor::Execute(
    const TurboHttpRequest& request,
    std::function<void(TurboHttpResponse)> on_result) {
  // 1. Synchronous pre-dispatch validation (URL and headers)
  ParsedUrl parsed_url;
  std::string url_error;
  if (!ParseRequestUrl(request.url(), &parsed_url, &url_error)) {
    on_result(MakeErrorResponse("connectionError", url_error));
    return;
  }

  std::wstring formatted_headers;
  if (!FormatAndValidateHeaders(request.headers(), &formatted_headers)) {
    on_result(MakeErrorResponse("connectionError",
                                "Invalid characters in request header."));
    return;
  }

  // 2. Build task before locking, then atomically register and submit under
  // mutex_ so Shutdown() cannot destroy the threadpool mid-submission.
  auto ctx = std::make_shared<RequestContext>();
  std::optional<TurboHttpRequest> override_request;
  if (transport_override_) {
    override_request.emplace(request);
  }

  auto task = std::make_unique<WorkTask>(WorkTask{
      this,
      BuildRequestParams(request),
      std::move(override_request),
      std::move(ctx),
      std::move(parsed_url),
      std::move(formatted_headers),
      std::move(on_result)});

  bool shut_down = false;
  bool submitted = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_shutdown_) {
      shut_down = true;
    } else {
      active_requests_[task->params.request_id] = task->ctx;
      submitted = TrySubmitThreadpoolCallback(
          &TurboHttpExecutor::WorkCallback, task.get(),
          env_initialized_ ? &callback_env_ : nullptr);
      if (submitted) {
        task.release();
      } else {
        auto it = active_requests_.find(task->params.request_id);
        if (it != active_requests_.end() && it->second == task->ctx) {
          active_requests_.erase(it);
        }
      }
    }
  }

  if (shut_down) {
    task->on_result(MakeErrorResponse("cancel", "Executor is shut down."));
  } else if (!submitted) {
    task->on_result(
        MakeErrorResponse("unknown", "Failed to submit threadpool callback."));
  }
}

// static
void CALLBACK TurboHttpExecutor::WorkCallback(
    PTP_CALLBACK_INSTANCE /*instance*/,
    PVOID parameter) {
  std::unique_ptr<WorkTask> task(static_cast<WorkTask*>(parameter));
  TurboHttpExecutor* self = task->executor;
  TurboHttpResponse response;

  try {
    if (task->ctx->is_cancelled.load()) {
      response = MakeCancelResponse();
    } else if (self->transport_override_ && task->override_request.has_value()) {
      response = self->transport_override_(*task->override_request, *task->ctx);
    } else {
      response = self->PerformWinHttpRequest(
          task->parsed_url, task->formatted_headers, task->params, *task->ctx);
    }
  } catch (const std::exception& e) {
    response = MakeErrorResponse("unknown", e.what());
  } catch (...) {
    response = MakeErrorResponse("unknown",
                                 "Unexpected native runtime exception.");
  }

  task->ctx->CloseRequestHandle();
  {
    std::lock_guard<std::mutex> lock(self->mutex_);
    auto it = self->active_requests_.find(task->params.request_id);
    if (it != self->active_requests_.end() && it->second == task->ctx) {
      self->active_requests_.erase(it);
    }
  }

  task->on_result(std::move(response));
}

TurboHttpResponse TurboHttpExecutor::PerformWinHttpRequest(
    const ParsedUrl& parsed_url,
    const std::wstring& formatted_headers,
    const RequestParams& params,
    RequestContext& ctx) {
  auto h_connect =
      session_manager_.AcquireConnection(parsed_url.host, parsed_url.port);
  if (h_connect == nullptr || h_connect->handle == nullptr) {
    return MakeWinHttpError(ctx, RequestPhase::kConnecting, "WinHttpConnect",
                            GetLastError());
  }

  std::wstring custom_method_w;
  const LPCWSTR method_w = ResolveMethodWide(params.method, &custom_method_w);
  const DWORD open_flags = parsed_url.is_https ? WINHTTP_FLAG_SECURE : 0;
  HINTERNET raw_request = WinHttpOpenRequest(
      h_connect->handle, method_w, parsed_url.path_and_query.c_str(), nullptr,
      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, open_flags);
  if (raw_request == nullptr) {
    return MakeWinHttpError(ctx, RequestPhase::kConnecting,
                            "WinHttpOpenRequest", GetLastError());
  }

  if (!ctx.SetRequestHandle(raw_request)) {
    return MakeCancelResponse();
  }

  ConfigureRequestOptions(raw_request, params, parsed_url.is_https);

  const size_t body_size = params.body.has_value() ? params.body->size() : 0;
  if (body_size > static_cast<size_t>(MAXDWORD)) {
    return MakeErrorResponse("connectionError",
                             "Request body exceeds WinHTTP DWORD size limit.");
  }
  if (formatted_headers.size() > static_cast<size_t>(MAXDWORD)) {
    return MakeErrorResponse(
        "connectionError",
        "Request headers exceed WinHTTP DWORD size limit.");
  }
  const DWORD total_body_len = static_cast<DWORD>(body_size);

  // Phase 1: Connect & send request headers
  if (ctx.is_cancelled.load()) {
    return MakeCancelResponse();
  }
  RequestPhase phase = RequestPhase::kConnecting;
  LPCWSTR headers_ptr = formatted_headers.empty()
                            ? WINHTTP_NO_ADDITIONAL_HEADERS
                            : formatted_headers.c_str();
  if (!WinHttpSendRequest(raw_request, headers_ptr,
                          static_cast<DWORD>(formatted_headers.size()),
                          WINHTTP_NO_REQUEST_DATA, 0, total_body_len, 0)) {
    return MakeWinHttpError(ctx, phase, "WinHttpSendRequest", GetLastError());
  }

  // Phase 2: Send request body (handling partial writes safely)
  if (total_body_len > 0) {
    phase = RequestPhase::kSendingBody;
    DWORD write_err = ERROR_SUCCESS;
    if (!WriteAllRequestBody(
            *params.body, ctx,
            [raw_request, &write_err](const uint8_t* data, DWORD len,
                                      DWORD* bytes_written) -> bool {
              if (!WinHttpWriteData(raw_request, data, len, bytes_written)) {
                write_err = GetLastError();
                return false;
              }
              return true;
            },
            &write_err)) {
      return MakeWinHttpError(ctx, phase, "WinHttpWriteData", write_err);
    }
  }

  // Phase 3: Receive response headers
  if (ctx.is_cancelled.load()) {
    return MakeCancelResponse();
  }
  phase = RequestPhase::kReceiving;
  if (!WinHttpReceiveResponse(raw_request, nullptr)) {
    return MakeWinHttpError(ctx, phase, "WinHttpReceiveResponse",
                            GetLastError());
  }

  DWORD status_code = 0;
  DWORD status_code_size = sizeof(status_code);
  if (!WinHttpQueryHeaders(
          raw_request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_code_size,
          WINHTTP_NO_HEADER_INDEX)) {
    return MakeWinHttpError(ctx, phase, "WinHttpQueryHeaders (status code)",
                            GetLastError());
  }

  wchar_t status_text_stack[kStatusTextStackChars];
  std::wstring status_text_heap;
  const std::string status_message = WideToUtf8(QueryWinHttpHeaderString(
      raw_request, WINHTTP_QUERY_STATUS_TEXT, status_text_stack,
      static_cast<DWORD>(sizeof(status_text_stack)), &status_text_heap));

  wchar_t raw_headers_stack[kRawHeadersStackChars];
  std::wstring raw_headers_heap;
  std::optional<int64_t> content_length;
  flutter::EncodableMap parsed_headers = ParseRawHttpHeaders(
      QueryWinHttpHeaderString(
          raw_request, WINHTTP_QUERY_RAW_HEADERS_CRLF, raw_headers_stack,
          static_cast<DWORD>(sizeof(raw_headers_stack)), &raw_headers_heap),
      &content_length);

  // Phase 4: Read response body directly without intermediate chunk copies
  DWORD read_win32_err = ERROR_SUCCESS;
  ReadBodyResult read_result = ReadResponseBodyDirect(
      content_length, params.max_response_bytes, ctx,
      [raw_request, &read_win32_err](uint8_t* dst, DWORD max_len,
                                     DWORD* bytes_read) -> bool {
        if (!WinHttpReadData(raw_request, dst, max_len, bytes_read)) {
          read_win32_err = GetLastError();
          return false;
        }
        return true;
      });

  if (!read_result.ok) {
    if (!read_result.error_code.empty()) {
      return MakeErrorResponse(read_result.error_code,
                               read_result.error_message);
    }
    return MakeWinHttpError(ctx, phase, "WinHttpReadData", read_win32_err);
  }

  TurboHttpResponse response;
  response.set_status_code(static_cast<int64_t>(status_code));
  response.set_status_message(status_message);
  response.set_headers(parsed_headers);
  response.set_body(read_result.body);
  return response;
}

void TurboHttpExecutor::Cancel(const std::string& request_id) {
  std::shared_ptr<RequestContext> ctx;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = active_requests_.find(request_id);
    if (it != active_requests_.end()) {
      ctx = it->second;
      active_requests_.erase(it);
    }
  }
  if (ctx != nullptr) {
    ctx->is_cancelled = true;
    ctx->CloseRequestHandle();
  }
}

void TurboHttpExecutor::Shutdown() {
  std::vector<std::shared_ptr<RequestContext>> to_cancel;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_shutdown_) {
      return;
    }
    is_shutdown_ = true;
    to_cancel.reserve(active_requests_.size());
    for (auto& kv : active_requests_) {
      to_cancel.push_back(kv.second);
    }
    active_requests_.clear();
  }

  for (auto& ctx : to_cancel) {
    ctx->is_cancelled = true;
    ctx->CloseRequestHandle();
  }

  if (cleanup_group_ != nullptr) {
    CloseThreadpoolCleanupGroupMembers(cleanup_group_, TRUE, nullptr);
    CloseThreadpoolCleanupGroup(cleanup_group_);
    cleanup_group_ = nullptr;
  }
  if (env_initialized_) {
    DestroyThreadpoolEnvironment(&callback_env_);
    env_initialized_ = false;
  }
  if (pool_ != nullptr) {
    CloseThreadpool(pool_);
    pool_ = nullptr;
  }

  session_manager_.Shutdown();
}

size_t TurboHttpExecutor::active_request_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_requests_.size();
}

bool TurboHttpExecutor::has_active_request(
    const std::string& request_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_requests_.find(request_id) != active_requests_.end();
}

// static
bool TurboHttpExecutor::WriteAllRequestBody(const std::vector<uint8_t>& body,
                                            const RequestContext& ctx,
                                            const WriteDataFn& write_fn,
                                            DWORD* out_win32_error) {
  if (body.size() > static_cast<size_t>(MAXDWORD)) {
    if (out_win32_error != nullptr) {
      *out_win32_error = ERROR_INVALID_PARAMETER;
    }
    return false;
  }

  size_t offset = 0;
  while (offset < body.size()) {
    if (ctx.is_cancelled.load()) {
      if (out_win32_error != nullptr) {
        *out_win32_error = ERROR_WINHTTP_OPERATION_CANCELLED;
      }
      return false;
    }

    const DWORD remaining = static_cast<DWORD>(body.size() - offset);
    DWORD bytes_written = 0;
    if (!write_fn(body.data() + offset, remaining, &bytes_written)) {
      return false;
    }
    if (bytes_written == 0) {
      if (out_win32_error != nullptr) {
        *out_win32_error = ERROR_WINHTTP_CONNECTION_ERROR;
      }
      return false;
    }
    offset += bytes_written;
  }

  return true;
}

// static
ReadBodyResult TurboHttpExecutor::ReadResponseBodyDirect(
    std::optional<int64_t> content_length,
    std::optional<int64_t> max_response_bytes,
    const RequestContext& ctx,
    const ReadDataFn& read_fn) {
  ReadBodyResult result;

  const std::optional<int64_t> safe_max =
      max_response_bytes.has_value()
          ? std::optional<int64_t>(std::max<int64_t>(0, *max_response_bytes))
          : std::nullopt;

  // Step 1: Verify Content-Length before reading body
  if (safe_max.has_value() && content_length.has_value() &&
      *content_length > *safe_max) {
    result.error_code = "maxResponseBytesExceeded";
    result.error_message =
        "Response Content-Length (" + std::to_string(*content_length) +
        " bytes) exceeded maxResponseBytes limit (" +
        std::to_string(*safe_max) + " bytes)";
    return result;
  }

  // Compute initial buffer size (at least 1 byte so boundary +1 detection works
  // even when maxResponseBytes == 0).
  int64_t initial_size = kReadBufferSizeBytes;
  if (content_length.has_value() && *content_length > 0) {
    initial_size = std::min<int64_t>(*content_length, kMaxPreallocBytes);
  }
  if (safe_max.has_value()) {
    const int64_t capped_max =
        (*safe_max < (std::numeric_limits<int64_t>::max)()) ? (*safe_max + 1)
                                                            : *safe_max;
    initial_size = std::min<int64_t>(initial_size, capped_max);
  }
  result.body.resize(static_cast<size_t>(std::max<int64_t>(1, initial_size)));

  size_t total_read = 0;
  for (;;) {
    if (ctx.is_cancelled.load()) {
      return MakeCancelReadResult();
    }

    if (total_read == result.body.size()) {
      const size_t grown = ComputeNextBufferSize(result.body.size(), safe_max);
      if (grown <= result.body.size()) {
        result.body.clear();
        result.error_code = "unknown";
        result.error_message = "Response buffer size limit exceeded.";
        return result;
      }
      result.body.resize(grown);
    }

    DWORD to_read = static_cast<DWORD>(std::min<size_t>(
        result.body.size() - total_read, static_cast<size_t>(kReadBufferSizeBytes)));
    if (safe_max.has_value()) {
      const int64_t remaining_allowed =
          *safe_max - static_cast<int64_t>(total_read);
      if (remaining_allowed < static_cast<int64_t>(to_read)) {
        to_read = static_cast<DWORD>(remaining_allowed + 1);
      }
    }

    DWORD bytes_read = 0;
    if (!read_fn(result.body.data() + total_read, to_read, &bytes_read)) {
      result.body.clear();
      return result;
    }
    if (bytes_read > to_read) {
      result.body.clear();
      result.error_code = "unknown";
      result.error_message = "Read callback reported more bytes than requested.";
      return result;
    }
    if (bytes_read == 0) {
      break;
    }

    total_read += static_cast<size_t>(bytes_read);
    if (safe_max.has_value() && static_cast<int64_t>(total_read) > *safe_max) {
      result.body.clear();
      result.error_code = "maxResponseBytesExceeded";
      result.error_message = "Response body exceeded maxResponseBytes limit (" +
                             std::to_string(*safe_max) + " bytes)";
      return result;
    }
  }

  if (ctx.is_cancelled.load()) {
    return MakeCancelReadResult();
  }

  result.body.resize(total_read);
  result.ok = true;
  return result;
}

}  // namespace flutter_turbo_fetch
