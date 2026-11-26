#include "turbo_http_executor.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <climits>
#include <cstdint>
#include <limits>
#include <utility>

namespace flutter_turbo_fetch {

namespace {

constexpr DWORD kReadBufferSizeBytes = 64 * 1024;
constexpr int64_t kMaxPreallocBytes = 64 * 1024 * 1024;
constexpr size_t kStatusTextStackChars = 64;
constexpr size_t kRawHeadersStackChars = 2048;

std::wstring Utf8ToWide(std::string_view str) {
  if (str.empty()) {
    return std::wstring();
  }

  std::wstring result(str.size(), L'\0');
  bool is_ascii = true;
  for (size_t i = 0; i < str.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(str[i]);
    if (c >= 0x80) {
      is_ascii = false;
      break;
    }
    result[i] = static_cast<wchar_t>(c);
  }
  if (is_ascii) {
    return result;
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
  result.resize(static_cast<size_t>(len));
  MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()),
                      result.data(), len);
  return result;
}

std::string WideToUtf8(std::wstring_view wstr) {
  if (wstr.empty()) {
    return std::string();
  }

  std::string result(wstr.size(), '\0');
  bool is_ascii = true;
  for (size_t i = 0; i < wstr.size(); ++i) {
    uint16_t wc = static_cast<uint16_t>(wstr[i]);
    if (wc >= 0x80) {
      is_ascii = false;
      break;
    }
    result[i] = static_cast<char>(wc);
  }
  if (is_ascii) {
    return result;
  }

  int len = WideCharToMultiByte(CP_UTF8, 0, wstr.data(),
                                static_cast<int>(wstr.size()), nullptr, 0,
                                nullptr, nullptr);
  if (len <= 0) {
    return std::string();
  }
  result.resize(static_cast<size_t>(len));
  WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
                      result.data(), len, nullptr, nullptr);
  return result;
}

std::string_view TrimAsciiView(std::string_view s) {
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

void AssignLowerAscii(std::string_view src, std::string* dst) {
  dst->resize(src.size());
  for (size_t i = 0; i < src.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(src[i]);
    (*dst)[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A'))
                                       : static_cast<char>(c);
  }
}

bool ContainsForbiddenHeaderChars(std::string_view s, bool is_name) {
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

LPCWSTR ResolveMethodWide(std::string_view method,
                          std::wstring* custom_storage) {
  if (method == "GET") return L"GET";
  if (method == "POST") return L"POST";
  if (method == "PUT") return L"PUT";
  if (method == "DELETE") return L"DELETE";
  if (method == "PATCH") return L"PATCH";
  if (method == "HEAD") return L"HEAD";
  if (method == "OPTIONS") return L"OPTIONS";
  *custom_storage = Utf8ToWide(method);
  return custom_storage->c_str();
}

std::wstring_view QueryWinHttpHeaderString(HINTERNET h_request,
                                           DWORD info_level,
                                           wchar_t* stack_buf,
                                           DWORD stack_buf_bytes,
                                           std::wstring* heap_fallback) {
  DWORD bytes = stack_buf_bytes;
  if (WinHttpQueryHeaders(h_request, info_level, WINHTTP_HEADER_NAME_BY_INDEX,
                          stack_buf, &bytes, WINHTTP_NO_HEADER_INDEX)) {
    size_t chars = bytes / sizeof(wchar_t);
    while (chars > 0 && stack_buf[chars - 1] == L'\0') {
      --chars;
    }
    return std::wstring_view(stack_buf, chars);
  }

  if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && bytes > 0) {
    heap_fallback->resize(bytes / sizeof(wchar_t), L'\0');
    if (WinHttpQueryHeaders(h_request, info_level, WINHTTP_HEADER_NAME_BY_INDEX,
                            heap_fallback->data(), &bytes,
                            WINHTTP_NO_HEADER_INDEX)) {
      size_t chars = bytes / sizeof(wchar_t);
      if (chars > heap_fallback->size()) {
        chars = heap_fallback->size();
      }
      while (chars > 0 && (*heap_fallback)[chars - 1] == L'\0') {
        --chars;
      }
      return std::wstring_view(heap_fallback->data(), chars);
    }
  }

  return std::wstring_view();
}

TurboHttpResponse MakeErrorResponse(const std::string& code,
                                    const std::string& message) {
  TurboHttpResponse res;
  res.set_error_code(code);
  res.set_error_message(message);
  return res;
}

int ClampTimeoutMs(std::optional<int64_t> timeout_opt, int default_ms) {
  if (!timeout_opt.has_value()) {
    return default_ms;
  }
  int64_t val = *timeout_opt;
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

  // Validate and batch-format headers in UTF-8 before a single wide conversion.
  std::string utf8_headers;
  const auto& req_headers = request.headers();
  if (!req_headers.empty()) {
    utf8_headers.reserve(req_headers.size() * 48);
    for (const auto& kv : req_headers) {
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
      utf8_headers.append(key);
      utf8_headers.append(": ");
      utf8_headers.append(val);
      utf8_headers.append("\r\n");
    }
  }
  std::wstring formatted_headers = Utf8ToWide(utf8_headers);

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

  // Copy only the fields required by the WinHTTP worker path (avoiding
  // redundant deep copies of headers_ and url_).
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

  std::optional<TurboHttpRequest> override_request;
  if (transport_override_) {
    override_request.emplace(request);
  }

  auto* task = new WorkTask{this,
                            std::move(params),
                            std::move(override_request),
                            std::move(ctx),
                            std::move(parsed_url),
                            std::move(formatted_headers),
                            std::move(on_result)};

  if (!TrySubmitThreadpoolCallback(
          &TurboHttpExecutor::WorkCallback, task,
          env_initialized_ ? &callback_env_ : nullptr)) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      active_requests_.erase(task->params.request_id);
    }
    task->on_result(
        MakeErrorResponse("unknown", "Failed to submit threadpool callback."));
    delete task;
    return;
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
      response = MakeErrorResponse("cancel", "Request was cancelled.");
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
    self->active_requests_.erase(task->params.request_id);
  }

  task->on_result(std::move(response));
}

TurboHttpResponse TurboHttpExecutor::PerformWinHttpRequest(
    const ParsedUrl& parsed_url,
    const std::wstring& formatted_headers,
    const RequestParams& params,
    RequestContext& ctx) {
  std::shared_ptr<ScopedWinHttpHandle> h_connect =
      session_manager_.AcquireConnection(parsed_url.host, parsed_url.port);
  if (h_connect == nullptr || h_connect->handle == nullptr) {
    DWORD err = GetLastError();
    return MakeErrorResponse(
        ClassifyError(ctx, err, RequestPhase::kConnecting),
        "WinHttpConnect failed (error " + std::to_string(err) + ").");
  }

  std::wstring custom_method_w;
  const LPCWSTR method_w = ResolveMethodWide(params.method, &custom_method_w);
  const DWORD open_flags = parsed_url.is_https ? WINHTTP_FLAG_SECURE : 0;
  HINTERNET raw_request = WinHttpOpenRequest(
      h_connect->handle, method_w, parsed_url.path_and_query.c_str(), nullptr,
      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, open_flags);
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
  const int connect_ms = ClampTimeoutMs(params.connect_timeout_ms, 60000);
  const int send_ms = ClampTimeoutMs(params.send_timeout_ms, 30000);
  const int receive_ms = ClampTimeoutMs(params.receive_timeout_ms, 30000);
  WinHttpSetTimeouts(raw_request, connect_ms, connect_ms, send_ms, receive_ms);

  // Configure redirect behavior
  if (!params.follow_redirects) {
    DWORD disable_flag = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(raw_request, WINHTTP_OPTION_DISABLE_FEATURE, &disable_flag,
                     sizeof(disable_flag));
  }

  // Configure self-signed certificate allowance for local verification
  if (params.allow_bad_certificates && parsed_url.is_https) {
    DWORD sec_flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                      SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(raw_request, WINHTTP_OPTION_SECURITY_FLAGS, &sec_flags,
                     sizeof(sec_flags));
  }

  const size_t body_size = params.body.has_value() ? params.body->size() : 0;
  if (body_size > static_cast<size_t>(MAXDWORD)) {
    return MakeErrorResponse("connectionError",
                             "Request body exceeds WinHTTP DWORD size limit.");
  }
  const DWORD total_body_len = static_cast<DWORD>(body_size);

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

  // Phase 2: Send request body if present (handling partial writes safely)
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
      return MakeErrorResponse(
          ClassifyError(ctx, write_err, phase),
          "WinHttpWriteData failed (error " + std::to_string(write_err) + ").");
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

  // Query status message using a stack buffer fast path
  wchar_t status_text_stack[kStatusTextStackChars];
  std::wstring status_text_heap;
  std::wstring_view status_text_view = QueryWinHttpHeaderString(
      raw_request, WINHTTP_QUERY_STATUS_TEXT, status_text_stack,
      static_cast<DWORD>(sizeof(status_text_stack)), &status_text_heap);
  std::string status_message = WideToUtf8(status_text_view);

  // Query raw response headers using a stack buffer fast path
  wchar_t raw_headers_stack[kRawHeadersStackChars];
  std::wstring raw_headers_heap;
  std::wstring_view raw_headers_view = QueryWinHttpHeaderString(
      raw_request, WINHTTP_QUERY_RAW_HEADERS_CRLF, raw_headers_stack,
      static_cast<DWORD>(sizeof(raw_headers_stack)), &raw_headers_heap);

  std::optional<int64_t> content_length;
  flutter::EncodableMap parsed_headers =
      ParseRawHeaders(raw_headers_view, &content_length);

  // Read response body directly into body_buffer without intermediate chunk copy
  std::vector<uint8_t> body_buffer;
  std::string read_err_code;
  std::string read_err_msg;
  DWORD read_win32_err = ERROR_SUCCESS;

  if (!ReadResponseBodyDirect(
          content_length, params.max_response_bytes, ctx,
          [raw_request, &read_win32_err](uint8_t* dst, DWORD max_len,
                                         DWORD* bytes_read) -> bool {
            if (!WinHttpReadData(raw_request, dst, max_len, bytes_read)) {
              read_win32_err = GetLastError();
              return false;
            }
            return true;
          },
          &body_buffer, &read_err_code, &read_err_msg, &read_win32_err)) {
    if (read_err_code == "maxResponseBytesExceeded") {
      ctx.CloseRequestHandle();
      return MakeErrorResponse(read_err_code, read_err_msg);
    }
    if (!read_err_code.empty()) {
      return MakeErrorResponse(read_err_code, read_err_msg);
    }
    return MakeErrorResponse(
        ClassifyError(ctx, read_win32_err, phase),
        "WinHttpReadData failed (error " + std::to_string(read_win32_err) +
            ").");
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
    std::wstring_view raw_headers,
    std::optional<int64_t>* out_content_length) {
  if (out_content_length != nullptr) {
    *out_content_length = std::nullopt;
  }

  flutter::EncodableMap result;
  const std::string utf8_raw = WideToUtf8(raw_headers);
  const std::string_view raw_view(utf8_raw);

  std::string key_buf;
  key_buf.reserve(32);

  size_t pos = 0;
  bool is_first_line = true;
  while (pos < raw_view.size()) {
    size_t end = raw_view.find("\r\n", pos);
    if (end == std::string_view::npos) {
      end = raw_view.size();
    }
    std::string_view line = raw_view.substr(pos, end - pos);
    pos = (end < raw_view.size()) ? end + 2 : raw_view.size();

    if (line.empty()) {
      continue;
    }
    if (is_first_line && line.size() >= 5 && line.substr(0, 5) == "HTTP/") {
      is_first_line = false;
      continue;
    }
    is_first_line = false;

    size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) {
      continue;
    }

    std::string_view key_view = TrimAsciiView(line.substr(0, colon));
    if (key_view.empty()) {
      continue;
    }
    std::string_view value_view = TrimAsciiView(line.substr(colon + 1));

    AssignLowerAscii(key_view, &key_buf);

    auto [it, inserted] = result.try_emplace(
        flutter::EncodableValue(key_buf),
        flutter::EncodableValue(flutter::EncodableList{}));
    std::get<flutter::EncodableList>(it->second)
        .emplace_back(std::string(value_view));

    if (out_content_length != nullptr && key_buf == "content-length" &&
        !value_view.empty()) {
      int64_t parsed_len = 0;
      const char* begin = value_view.data();
      const char* finish = begin + value_view.size();
      auto [ptr, ec] = std::from_chars(begin, finish, parsed_len);
      if (ec == std::errc() && ptr == finish && parsed_len >= 0) {
        *out_content_length = parsed_len;
      }
    }
  }

  return result;
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
bool TurboHttpExecutor::ReadResponseBodyDirect(
    std::optional<int64_t> content_length,
    std::optional<int64_t> max_response_bytes,
    const RequestContext& ctx,
    const ReadDataFn& read_fn,
    std::vector<uint8_t>* out_body,
    std::string* out_error_code,
    std::string* out_error_message,
    DWORD* out_win32_error) {
  out_body->clear();
  if (out_error_code != nullptr) {
    out_error_code->clear();
  }
  if (out_error_message != nullptr) {
    out_error_message->clear();
  }

  const std::optional<int64_t> safe_max =
      max_response_bytes.has_value()
          ? std::optional<int64_t>(std::max<int64_t>(0, *max_response_bytes))
          : std::nullopt;

  // Step 1: Verify Content-Length before reading body
  if (safe_max.has_value() && content_length.has_value() &&
      *content_length > *safe_max) {
    if (out_error_code != nullptr) {
      *out_error_code = "maxResponseBytesExceeded";
    }
    if (out_error_message != nullptr) {
      *out_error_message =
          "Response Content-Length (" + std::to_string(*content_length) +
          " bytes) exceeded maxResponseBytes limit (" +
          std::to_string(*safe_max) + " bytes)";
    }
    return false;
  }

  // Compute initial buffer size (at least 1 byte so boundary +1 detection works
  // even when maxResponseBytes == 0).
  int64_t initial_size = kReadBufferSizeBytes;
  if (content_length.has_value() && *content_length > 0) {
    initial_size = std::min<int64_t>(*content_length, kMaxPreallocBytes);
  }
  if (safe_max.has_value()) {
    const int64_t capped_max =
        (*safe_max < (std::numeric_limits<int64_t>::max)())
            ? (*safe_max + 1)
            : *safe_max;
    initial_size = std::min<int64_t>(initial_size, capped_max);
  }
  if (initial_size < 1) {
    initial_size = 1;
  }
  out_body->resize(static_cast<size_t>(initial_size));

  size_t total_read = 0;
  for (;;) {
    if (ctx.is_cancelled.load()) {
      out_body->clear();
      if (out_error_code != nullptr) {
        *out_error_code = "cancel";
      }
      if (out_error_message != nullptr) {
        *out_error_message = "Request was cancelled.";
      }
      if (out_win32_error != nullptr) {
        *out_win32_error = ERROR_WINHTTP_OPERATION_CANCELLED;
      }
      return false;
    }

    if (total_read == out_body->size()) {
      size_t grown =
          std::max<size_t>(out_body->size() * 2,
                           out_body->size() + static_cast<size_t>(kReadBufferSizeBytes));
      if (safe_max.has_value() &&
          *safe_max < (std::numeric_limits<int64_t>::max)()) {
        const size_t max_alloc = static_cast<size_t>(*safe_max + 1);
        if (grown > max_alloc) {
          grown = max_alloc;
        }
      }
      out_body->resize(grown);
    }

    size_t available = out_body->size() - total_read;
    DWORD to_read = static_cast<DWORD>(
        std::min<size_t>(available, static_cast<size_t>(kReadBufferSizeBytes)));
    if (safe_max.has_value()) {
      const int64_t remaining_allowed =
          *safe_max - static_cast<int64_t>(total_read);
      if (remaining_allowed < static_cast<int64_t>(to_read)) {
        to_read = static_cast<DWORD>(remaining_allowed + 1);
      }
    }

    DWORD bytes_read = 0;
    if (!read_fn(out_body->data() + total_read, to_read, &bytes_read)) {
      out_body->clear();
      return false;
    }

    if (bytes_read == 0) {
      break;
    }

    total_read += static_cast<size_t>(bytes_read);
    if (safe_max.has_value() && static_cast<int64_t>(total_read) > *safe_max) {
      out_body->clear();
      if (out_error_code != nullptr) {
        *out_error_code = "maxResponseBytesExceeded";
      }
      if (out_error_message != nullptr) {
        *out_error_message = "Response body exceeded maxResponseBytes limit (" +
                             std::to_string(*safe_max) + " bytes)";
      }
      return false;
    }
  }

  if (ctx.is_cancelled.load()) {
    out_body->clear();
    if (out_error_code != nullptr) {
      *out_error_code = "cancel";
    }
    if (out_error_message != nullptr) {
      *out_error_message = "Request was cancelled.";
    }
    if (out_win32_error != nullptr) {
      *out_win32_error = ERROR_WINHTTP_OPERATION_CANCELLED;
    }
    return false;
  }

  out_body->resize(total_read);
  return true;
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
