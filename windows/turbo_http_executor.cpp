#include "turbo_http_executor.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cwctype>
#include <sstream>
#include <utility>

namespace flutter_turbo_fetch {

namespace {

constexpr DWORD kReadBufferSizeBytes = 64 * 1024;
constexpr int64_t kMaxPreallocBytes = 64 * 1024 * 1024;

struct ScopedWinHttpHandle {
  explicit ScopedWinHttpHandle(HINTERNET h = nullptr) : handle(h) {}
  ~ScopedWinHttpHandle() {
    if (handle != nullptr) {
      WinHttpCloseHandle(handle);
    }
  }
  ScopedWinHttpHandle(const ScopedWinHttpHandle&) = delete;
  ScopedWinHttpHandle& operator=(const ScopedWinHttpHandle&) = delete;

  HINTERNET handle = nullptr;
};

std::wstring Utf8ToWide(const std::string& str) {
  if (str.empty()) {
    return std::wstring();
  }
  int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, str.data(),
                                static_cast<int>(str.size()), nullptr, 0);
  if (len <= 0) {
    len = MultiByteToWideChar(CP_UTF8, 0, str.data(),
                              static_cast<int>(str.size()), nullptr, 0);
  }
  if (len <= 0) {
    return std::wstring();
  }
  std::wstring result(static_cast<size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()),
                      result.data(), len);
  return result;
}

std::string WideToUtf8(const std::wstring& wstr) {
  if (wstr.empty()) {
    return std::string();
  }
  int len = WideCharToMultiByte(CP_UTF8, 0, wstr.data(),
                                static_cast<int>(wstr.size()), nullptr, 0,
                                nullptr, nullptr);
  if (len <= 0) {
    return std::string();
  }
  std::string result(static_cast<size_t>(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
                      result.data(), len, nullptr, nullptr);
  return result;
}

std::string ToLowerAscii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string TrimAscii(const std::string& s) {
  size_t first = 0;
  while (first < s.size() &&
         std::isspace(static_cast<unsigned char>(s[first]))) {
    ++first;
  }
  size_t last = s.size();
  while (last > first &&
         std::isspace(static_cast<unsigned char>(s[last - 1]))) {
    --last;
  }
  return s.substr(first, last - first);
}

bool ContainsForbiddenHeaderChars(const std::string& s, bool is_name) {
  if (is_name && s.empty()) {
    return true;
  }
  for (unsigned char c : s) {
    if (c == '\r' || c == '\n' || c == '\0') {
      return true;
    }
    if (is_name && (c <= 0x20 || c == ':' || c >= 0x7f)) {
      return true;
    }
  }
  return false;
}

TurboHttpResponse MakeErrorResponse(const std::string& code,
                                    const std::string& message) {
  TurboHttpResponse res;
  res.set_error_code(code);
  res.set_error_message(message);
  return res;
}

int ClampTimeoutMs(const int64_t* timeout_ptr, int default_ms) {
  if (timeout_ptr == nullptr) {
    return default_ms;
  }
  int64_t val = *timeout_ptr;
  if (val <= 0) {
    return 0;
  }
  if (val > static_cast<int64_t>(INT_MAX)) {
    return INT_MAX;
  }
  return static_cast<int>(val);
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

void TurboHttpExecutor::Execute(
    const TurboHttpRequest& request,
    std::function<void(TurboHttpResponse)> on_result) {
  // 1. Synchronous pre-dispatch validation (URL and headers)
  const std::wstring url_w = Utf8ToWide(request.url());
  if (url_w.empty()) {
    on_result(MakeErrorResponse("connectionError", "Invalid or empty URL."));
    return;
  }

  URL_COMPONENTS url_comp{};
  url_comp.dwStructSize = sizeof(url_comp);
  url_comp.dwSchemeLength = static_cast<DWORD>(-1);
  url_comp.dwHostNameLength = static_cast<DWORD>(-1);
  url_comp.dwUrlPathLength = static_cast<DWORD>(-1);
  url_comp.dwExtraInfoLength = static_cast<DWORD>(-1);

  if (!WinHttpCrackUrl(url_w.c_str(), static_cast<DWORD>(url_w.size()), 0,
                       &url_comp) ||
      url_comp.dwHostNameLength == 0 ||
      (url_comp.nScheme != INTERNET_SCHEME_HTTP &&
       url_comp.nScheme != INTERNET_SCHEME_HTTPS)) {
    on_result(MakeErrorResponse("connectionError",
                                "Failed to parse URL or unsupported scheme: " +
                                    request.url()));
    return;
  }

  ParsedUrl parsed_url;
  parsed_url.is_https = (url_comp.nScheme == INTERNET_SCHEME_HTTPS);
  parsed_url.port = url_comp.nPort;
  parsed_url.host.assign(url_comp.lpszHostName, url_comp.dwHostNameLength);
  if (url_comp.dwUrlPathLength > 0) {
    parsed_url.path_and_query.assign(url_comp.lpszUrlPath,
                                     url_comp.dwUrlPathLength);
  } else {
    parsed_url.path_and_query = L"/";
  }
  if (url_comp.dwExtraInfoLength > 0) {
    parsed_url.path_and_query.append(url_comp.lpszExtraInfo,
                                     url_comp.dwExtraInfoLength);
  }

  std::wstring formatted_headers;
  for (const auto& kv : request.headers()) {
    if (!std::holds_alternative<std::string>(kv.first) ||
        !std::holds_alternative<std::string>(kv.second)) {
      continue;
    }
    const auto& key = std::get<std::string>(kv.first);
    const auto& val = std::get<std::string>(kv.second);
    if (ContainsForbiddenHeaderChars(key, /*is_name=*/true) ||
        ContainsForbiddenHeaderChars(val, /*is_name=*/false)) {
      on_result(MakeErrorResponse("connectionError",
                                  "Invalid characters in request header."));
      return;
    }
    formatted_headers.append(Utf8ToWide(key));
    formatted_headers.append(L": ");
    formatted_headers.append(Utf8ToWide(val));
    formatted_headers.append(L"\r\n");
  }

  // 2. Register active RequestContext before submitting work to thread pool
  auto ctx = std::make_shared<RequestContext>();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_shutdown_.load()) {
      on_result(MakeErrorResponse("cancel", "Executor is shut down."));
      return;
    }
    active_requests_[request.request_id()] = ctx;
  }

  auto* task = new WorkTask{this,
                            request,
                            ctx,
                            std::move(parsed_url),
                            std::move(formatted_headers),
                            std::move(on_result)};

  PTP_WORK work = CreateThreadpoolWork(&TurboHttpExecutor::WorkCallback, task,
                                       env_initialized_ ? &callback_env_ : nullptr);
  if (work == nullptr) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      active_requests_.erase(request.request_id());
    }
    task->on_result(
        MakeErrorResponse("unknown", "Failed to create threadpool work."));
    delete task;
    return;
  }

  SubmitThreadpoolWork(work);
}

// static
void CALLBACK TurboHttpExecutor::WorkCallback(PTP_CALLBACK_INSTANCE /*instance*/,
                                              PVOID parameter,
                                              PTP_WORK work) {
  std::unique_ptr<WorkTask> task(static_cast<WorkTask*>(parameter));
  CloseThreadpoolWork(work);

  TurboHttpExecutor* self = task->executor;
  TurboHttpResponse response;

  try {
    if (task->ctx->is_cancelled.load()) {
      response = MakeErrorResponse("cancel", "Request was cancelled.");
    } else if (self->transport_override_) {
      response = self->transport_override_(task->request, *task->ctx);
    } else {
      response = self->PerformWinHttpRequest(
          task->parsed_url, task->formatted_headers, task->request, *task->ctx);
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
    self->active_requests_.erase(task->request.request_id());
  }

  task->on_result(std::move(response));
}

TurboHttpResponse TurboHttpExecutor::PerformWinHttpRequest(
    const ParsedUrl& parsed_url,
    const std::wstring& formatted_headers,
    const TurboHttpRequest& request,
    RequestContext& ctx) {
  HINTERNET session =
      session_manager_.AcquireSession(request.max_requests_per_host());
  if (session == nullptr) {
    DWORD err = GetLastError();
    return MakeErrorResponse(
        ClassifyError(ctx, err, RequestPhase::kConnecting),
        "Failed to initialize WinHTTP session (error " + std::to_string(err) +
            ").");
  }

  ScopedWinHttpHandle h_connect(WinHttpConnect(
      session, parsed_url.host.c_str(), parsed_url.port, 0));
  if (h_connect.handle == nullptr) {
    DWORD err = GetLastError();
    return MakeErrorResponse(
        ClassifyError(ctx, err, RequestPhase::kConnecting),
        "WinHttpConnect failed (error " + std::to_string(err) + ").");
  }

  const std::wstring method_w = Utf8ToWide(request.method());
  const DWORD open_flags = parsed_url.is_https ? WINHTTP_FLAG_SECURE : 0;
  HINTERNET raw_request = WinHttpOpenRequest(
      h_connect.handle, method_w.c_str(), parsed_url.path_and_query.c_str(),
      nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, open_flags);
  if (raw_request == nullptr) {
    DWORD err = GetLastError();
    return MakeErrorResponse(
        ClassifyError(ctx, err, RequestPhase::kConnecting),
        "WinHttpOpenRequest failed (error " + std::to_string(err) + ").");
  }

  if (!ctx.SetRequestHandle(raw_request)) {
    return MakeErrorResponse("cancel", "Request was cancelled.");
  }

  // Configure per-request timeouts (default: 60s connect/resolve, 30s send, 30s receive)
  const int connect_ms = ClampTimeoutMs(request.connect_timeout_ms(), 60000);
  const int send_ms = ClampTimeoutMs(request.send_timeout_ms(), 30000);
  const int receive_ms = ClampTimeoutMs(request.receive_timeout_ms(), 30000);
  WinHttpSetTimeouts(raw_request, connect_ms, connect_ms, send_ms, receive_ms);

  // Configure redirect behavior
  if (!request.follow_redirects()) {
    DWORD disable_flag = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(raw_request, WINHTTP_OPTION_DISABLE_FEATURE, &disable_flag,
                     sizeof(disable_flag));
  }

  // Configure self-signed certificate allowance for local verification
  if (request.allow_bad_certificates() && parsed_url.is_https) {
    DWORD sec_flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                      SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(raw_request, WINHTTP_OPTION_SECURITY_FLAGS, &sec_flags,
                     sizeof(sec_flags));
  }

  const std::vector<uint8_t>* req_body = request.body();
  const DWORD total_body_len =
      (req_body != nullptr) ? static_cast<DWORD>(req_body->size()) : 0;

  // Phase 1: Connect & send request headers
  RequestPhase phase = RequestPhase::kConnecting;
  LPCWSTR headers_ptr = formatted_headers.empty()
                            ? WINHTTP_NO_ADDITIONAL_HEADERS
                            : formatted_headers.c_str();
  DWORD headers_len = static_cast<DWORD>(formatted_headers.size());

  if (!WinHttpSendRequest(raw_request, headers_ptr, headers_len,
                          WINHTTP_NO_REQUEST_DATA, 0, total_body_len, 0)) {
    DWORD err = GetLastError();
    return MakeErrorResponse(
        ClassifyError(ctx, err, phase),
        "WinHttpSendRequest failed (error " + std::to_string(err) + ").");
  }

  // Phase 2: Send request body if present
  if (total_body_len > 0) {
    phase = RequestPhase::kSendingBody;
    DWORD bytes_written = 0;
    if (!WinHttpWriteData(raw_request, req_body->data(), total_body_len,
                          &bytes_written)) {
      DWORD err = GetLastError();
      return MakeErrorResponse(
          ClassifyError(ctx, err, phase),
          "WinHttpWriteData failed (error " + std::to_string(err) + ").");
    }
  }

  // Phase 3: Receive response headers
  phase = RequestPhase::kReceiving;
  if (!WinHttpReceiveResponse(raw_request, nullptr)) {
    DWORD err = GetLastError();
    return MakeErrorResponse(
        ClassifyError(ctx, err, phase),
        "WinHttpReceiveResponse failed (error " + std::to_string(err) + ").");
  }

  // Query status code
  DWORD status_code = 0;
  DWORD status_code_size = sizeof(status_code);
  if (!WinHttpQueryHeaders(
          raw_request,
          WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_code_size,
          WINHTTP_NO_HEADER_INDEX)) {
    DWORD err = GetLastError();
    return MakeErrorResponse(
        ClassifyError(ctx, err, phase),
        "WinHttpQueryHeaders (status code) failed (error " +
            std::to_string(err) + ").");
  }

  // Query status message
  std::string status_message;
  DWORD status_text_bytes = 0;
  WinHttpQueryHeaders(raw_request, WINHTTP_QUERY_STATUS_TEXT,
                      WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &status_text_bytes,
                      WINHTTP_NO_HEADER_INDEX);
  if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && status_text_bytes > 0) {
    std::wstring status_text_w(status_text_bytes / sizeof(wchar_t), L'\0');
    if (WinHttpQueryHeaders(raw_request, WINHTTP_QUERY_STATUS_TEXT,
                            WINHTTP_HEADER_NAME_BY_INDEX, status_text_w.data(),
                            &status_text_bytes, WINHTTP_NO_HEADER_INDEX)) {
      if (!status_text_w.empty() && status_text_w.back() == L'\0') {
        status_text_w.pop_back();
      }
      status_message = WideToUtf8(status_text_w);
    }
  }

  // Query raw response headers
  std::wstring raw_headers_w;
  DWORD raw_headers_bytes = 0;
  WinHttpQueryHeaders(raw_request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                      WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &raw_headers_bytes,
                      WINHTTP_NO_HEADER_INDEX);
  if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && raw_headers_bytes > 0) {
    raw_headers_w.resize(raw_headers_bytes / sizeof(wchar_t), L'\0');
    if (WinHttpQueryHeaders(raw_request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                            WINHTTP_HEADER_NAME_BY_INDEX, raw_headers_w.data(),
                            &raw_headers_bytes, WINHTTP_NO_HEADER_INDEX)) {
      if (!raw_headers_w.empty() && raw_headers_w.back() == L'\0') {
        raw_headers_w.pop_back();
      }
    }
  }

  std::optional<int64_t> content_length;
  flutter::EncodableMap parsed_headers =
      ParseRawHeaders(raw_headers_w, &content_length);

  const int64_t* max_bytes_ptr = request.max_response_bytes();
  const std::optional<int64_t> max_bytes_opt =
      (max_bytes_ptr != nullptr)
          ? std::optional<int64_t>(std::max<int64_t>(0, *max_bytes_ptr))
          : std::nullopt;

  // Step 1 of maxResponseBytes check: verify Content-Length before reading body
  if (max_bytes_opt.has_value() && content_length.has_value() &&
      *content_length > *max_bytes_opt) {
    ctx.CloseRequestHandle();
    return MakeErrorResponse(
        "maxResponseBytesExceeded",
        "Response Content-Length (" + std::to_string(*content_length) +
            " bytes) exceeded maxResponseBytes limit (" +
            std::to_string(*max_bytes_opt) + " bytes)");
  }

  // Step 2: Read and concatenate response body in native memory
  std::vector<uint8_t> body_buffer;
  if (content_length.has_value() && *content_length > 0 &&
      *content_length <= kMaxPreallocBytes) {
    body_buffer.reserve(static_cast<size_t>(*content_length));
  }

  std::vector<uint8_t> read_chunk(kReadBufferSizeBytes);
  for (;;) {
    if (ctx.is_cancelled.load()) {
      return MakeErrorResponse("cancel", "Request was cancelled.");
    }

    DWORD bytes_read = 0;
    if (!WinHttpReadData(raw_request, read_chunk.data(), kReadBufferSizeBytes,
                         &bytes_read)) {
      DWORD err = GetLastError();
      return MakeErrorResponse(
          ClassifyError(ctx, err, phase),
          "WinHttpReadData failed (error " + std::to_string(err) + ").");
    }

    if (bytes_read == 0) {
      break;
    }

    const int64_t new_size =
        static_cast<int64_t>(body_buffer.size()) + static_cast<int64_t>(bytes_read);
    if (max_bytes_opt.has_value() && new_size > *max_bytes_opt) {
      ctx.CloseRequestHandle();
      return MakeErrorResponse(
          "maxResponseBytesExceeded",
          "Response body exceeded maxResponseBytes limit (" +
              std::to_string(*max_bytes_opt) + " bytes)");
    }

    body_buffer.insert(body_buffer.end(), read_chunk.begin(),
                       read_chunk.begin() + bytes_read);
  }

  if (ctx.is_cancelled.load()) {
    return MakeErrorResponse("cancel", "Request was cancelled.");
  }

  TurboHttpResponse response;
  response.set_status_code(static_cast<int64_t>(status_code));
  response.set_status_message(status_message);
  response.set_headers(parsed_headers);
  response.set_body(body_buffer);
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
  bool expected = false;
  if (!is_shutdown_.compare_exchange_strong(expected, true)) {
    return;
  }

  std::vector<std::shared_ptr<RequestContext>> to_cancel;
  {
    std::lock_guard<std::mutex> lock(mutex_);
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

// static
std::string TurboHttpExecutor::ClassifyError(const RequestContext& ctx,
                                             DWORD win32_error,
                                             RequestPhase phase) {
  if (ctx.is_cancelled.load() ||
      win32_error == ERROR_WINHTTP_OPERATION_CANCELLED ||
      win32_error == ERROR_INVALID_HANDLE) {
    return "cancel";
  }

  if (win32_error == ERROR_WINHTTP_TIMEOUT) {
    switch (phase) {
      case RequestPhase::kConnecting:
        return "connectionTimeout";
      case RequestPhase::kSendingBody:
        return "sendTimeout";
      case RequestPhase::kReceiving:
        return "receiveTimeout";
    }
  }

  switch (win32_error) {
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
    case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
    case ERROR_WINHTTP_SECURE_INVALID_CERT:
    case ERROR_WINHTTP_SECURE_CERT_REVOKED:
    case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
    case static_cast<DWORD>(SEC_E_UNTRUSTED_ROOT):
    case static_cast<DWORD>(SEC_E_CERT_EXPIRED):
    case static_cast<DWORD>(SEC_E_WRONG_PRINCIPAL):
    case static_cast<DWORD>(SEC_E_ILLEGAL_MESSAGE):
    case static_cast<DWORD>(SEC_E_INVALID_TOKEN):
      return "badCertificate";

    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR:
    case ERROR_WINHTTP_INVALID_URL:
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
    case ERROR_WINHTTP_REDIRECT_FAILED:
    case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:
    case ERROR_WINHTTP_HEADER_NOT_FOUND:
    case ERROR_INVALID_PARAMETER:
      return "connectionError";

    default:
      return "unknown";
  }
}

// static
flutter::EncodableMap TurboHttpExecutor::ParseRawHeaders(
    const std::wstring& raw_headers,
    std::optional<int64_t>* out_content_length) {
  if (out_content_length != nullptr) {
    *out_content_length = std::nullopt;
  }

  std::unordered_map<std::string, flutter::EncodableList> grouped;
  const std::string utf8_raw = WideToUtf8(raw_headers);

  size_t pos = 0;
  bool is_first_line = true;
  while (pos < utf8_raw.size()) {
    size_t end = utf8_raw.find("\r\n", pos);
    if (end == std::string::npos) {
      end = utf8_raw.size();
    }
    std::string line = utf8_raw.substr(pos, end - pos);
    pos = end + 2;

    if (line.empty()) {
      continue;
    }
    if (is_first_line && line.rfind("HTTP/", 0) == 0) {
      is_first_line = false;
      continue;
    }
    is_first_line = false;

    size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0) {
      continue;
    }

    std::string key = ToLowerAscii(TrimAscii(line.substr(0, colon)));
    std::string value = TrimAscii(line.substr(colon + 1));
    if (key.empty()) {
      continue;
    }

    grouped[key].push_back(flutter::EncodableValue(value));

    if (out_content_length != nullptr && key == "content-length") {
      try {
        int64_t parsed_len = std::stoll(value);
        if (parsed_len >= 0) {
          *out_content_length = parsed_len;
        }
      } catch (...) {
      }
    }
  }

  flutter::EncodableMap result;
  for (auto& kv : grouped) {
    result.emplace(flutter::EncodableValue(kv.first),
                   flutter::EncodableValue( std::move(kv.second) ));
  }
  return result;
}

// static
bool TurboHttpExecutor::ValidateAndAssembleChunks(
    std::optional<int64_t> content_length,
    std::optional<int64_t> max_response_bytes,
    const std::vector<std::vector<uint8_t>>& chunks,
    std::vector<uint8_t>* out_body,
    std::string* out_error_code,
    std::string* out_error_message) {
  out_body->clear();
  const std::optional<int64_t> safe_max =
      max_response_bytes.has_value()
          ? std::optional<int64_t>(std::max<int64_t>(0, *max_response_bytes))
          : std::nullopt;

  if (safe_max.has_value() && content_length.has_value() &&
      *content_length > *safe_max) {
    *out_error_code = "maxResponseBytesExceeded";
    *out_error_message =
        "Response Content-Length (" + std::to_string(*content_length) +
        " bytes) exceeded maxResponseBytes limit (" +
        std::to_string(*safe_max) + " bytes)";
    return false;
  }

  for (const auto& chunk : chunks) {
    const int64_t next_size =
        static_cast<int64_t>(out_body->size()) + static_cast<int64_t>(chunk.size());
    if (safe_max.has_value() && next_size > *safe_max) {
      out_body->clear();
      *out_error_code = "maxResponseBytesExceeded";
      *out_error_message = "Response body exceeded maxResponseBytes limit (" +
                           std::to_string(*safe_max) + " bytes)";
      return false;
    }
    out_body->insert(out_body->end(), chunk.begin(), chunk.end());
  }

  return true;
}

}  // namespace flutter_turbo_fetch

