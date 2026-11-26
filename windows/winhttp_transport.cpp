#include "winhttp_transport.h"

#include <algorithm>
#include <climits>
#include <limits>
#include <string_view>
#include <utility>

namespace flutter_turbo_fetch {

namespace {

constexpr DWORD kInitialUnknownBodyBytes = 8 * 1024;
constexpr DWORD kReadBufferSizeBytes = 64 * 1024;
constexpr int64_t kMaxPreallocBytes = 64 * 1024 * 1024;
constexpr size_t kStatusTextStackChars = 64;
constexpr size_t kRawHeadersStackChars = 2048;

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

std::wstring_view TrimTrailingNulls(const wchar_t* data,
                                    size_t max_chars,
                                    DWORD bytes) {
  size_t chars = bytes / sizeof(wchar_t);
  if (chars > max_chars) {
    chars = max_chars;
  }
  while (chars > 0 && data[chars - 1] == L'\0') {
    --chars;
  }
  return std::wstring_view(data, chars);
}

std::wstring_view QueryWinHttpHeaderString(HINTERNET h_request,
                                           DWORD info_level,
                                           wchar_t* stack_buf,
                                           DWORD stack_buf_bytes,
                                           std::wstring* heap_fallback,
                                           DWORD* out_win32_error = nullptr) {
  if (out_win32_error != nullptr) {
    *out_win32_error = ERROR_SUCCESS;
  }

  DWORD bytes = stack_buf_bytes;
  if (WinHttpQueryHeaders(h_request, info_level, WINHTTP_HEADER_NAME_BY_INDEX,
                          stack_buf, &bytes, WINHTTP_NO_HEADER_INDEX)) {
    return TrimTrailingNulls(stack_buf, stack_buf_bytes / sizeof(wchar_t),
                             bytes);
  }

  DWORD err = GetLastError();
  if (err == ERROR_INSUFFICIENT_BUFFER && bytes > 0) {
    heap_fallback->resize((bytes + sizeof(wchar_t) - 1) / sizeof(wchar_t),
                          L'\0');
    if (WinHttpQueryHeaders(h_request, info_level, WINHTTP_HEADER_NAME_BY_INDEX,
                            heap_fallback->data(), &bytes,
                            WINHTTP_NO_HEADER_INDEX)) {
      return TrimTrailingNulls(heap_fallback->data(), heap_fallback->size(),
                               bytes);
    }
    err = GetLastError();
  }

  if (out_win32_error != nullptr) {
    *out_win32_error = err;
  }
  return std::wstring_view();
}

TurboHttpResponse MakeWinHttpError(const RequestContext& ctx,
                                   RequestPhase phase,
                                   const char* api_name,
                                   DWORD err) {
  return MakeErrorResponse(
      ClassifyWinHttpError(ctx.is_cancelled.load(), err, phase),
      std::string(api_name) + " failed (error " + std::to_string(err) + ").");
}

ReadBodyResult MakeReadError(std::string_view code, std::string_view message) {
  ReadBodyResult result;
  result.error_code = std::string(code);
  result.error_message = std::string(message);
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

// static
bool WinHttpTransport::ConfigureRequestOptions(
    HINTERNET raw_request,
    const ValidatedWinHttpRequest& request,
    const char** out_failed_api,
    DWORD* out_win32_error) {
  const int connect_ms = ClampTimeoutMs(request.connect_timeout_ms, 60000);
  const int send_ms = ClampTimeoutMs(request.send_timeout_ms, 30000);
  const int receive_ms = ClampTimeoutMs(request.receive_timeout_ms, 30000);
  if (!WinHttpSetTimeouts(raw_request, connect_ms, connect_ms, send_ms,
                          receive_ms)) {
    *out_failed_api = "WinHttpSetTimeouts";
    *out_win32_error = GetLastError();
    return false;
  }

  if (!request.follow_redirects) {
    DWORD disable_flag = WINHTTP_DISABLE_REDIRECTS;
    if (!WinHttpSetOption(raw_request, WINHTTP_OPTION_DISABLE_FEATURE,
                          &disable_flag, sizeof(disable_flag))) {
      *out_failed_api = "WinHttpSetOption (disable redirects)";
      *out_win32_error = GetLastError();
      return false;
    }
  }

  if (request.allow_bad_certificates && request.parsed_url.is_https) {
    DWORD sec_flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                      SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    if (!WinHttpSetOption(raw_request, WINHTTP_OPTION_SECURITY_FLAGS,
                          &sec_flags, sizeof(sec_flags))) {
      *out_failed_api = "WinHttpSetOption (security flags)";
      *out_win32_error = GetLastError();
      return false;
    }
  }
  return true;
}

void WinHttpTransport::Shutdown() { session_manager_.Shutdown(); }

TurboHttpResponse WinHttpTransport::Execute(
    const ValidatedWinHttpRequest& request,
    RequestContext& ctx) {
  auto h_connect = session_manager_.AcquireConnection(request.parsed_url.host,
                                                      request.parsed_url.port);
  if (h_connect == nullptr || h_connect->handle == nullptr) {
    return MakeWinHttpError(ctx, RequestPhase::kConnecting, "WinHttpConnect",
                            GetLastError());
  }

  std::wstring custom_method_w;
  const LPCWSTR method_w = ResolveMethodWide(request.method, &custom_method_w);
  const DWORD open_flags =
      request.parsed_url.is_https ? WINHTTP_FLAG_SECURE : 0;
  HINTERNET raw_request = WinHttpOpenRequest(
      h_connect->handle, method_w, request.parsed_url.path_and_query.c_str(),
      nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, open_flags);
  if (raw_request == nullptr) {
    return MakeWinHttpError(ctx, RequestPhase::kConnecting,
                            "WinHttpOpenRequest", GetLastError());
  }

  if (!ctx.SetRequestHandle(raw_request)) {
    return MakeCancelResponse();
  }

  const char* failed_opt_api = nullptr;
  DWORD opt_err = ERROR_SUCCESS;
  if (!ConfigureRequestOptions(raw_request, request, &failed_opt_api,
                               &opt_err)) {
    return MakeWinHttpError(ctx, RequestPhase::kConnecting, failed_opt_api,
                            opt_err);
  }

  const DWORD total_body_len =
      request.body.has_value() ? static_cast<DWORD>(request.body->size()) : 0;

  // Phase 1: Connect & send request headers
  if (ctx.is_cancelled.load()) {
    return MakeCancelResponse();
  }
  RequestPhase phase = RequestPhase::kConnecting;
  LPCWSTR headers_ptr = request.formatted_headers.empty()
                            ? WINHTTP_NO_ADDITIONAL_HEADERS
                            : request.formatted_headers.c_str();
  if (!WinHttpSendRequest(raw_request, headers_ptr,
                          static_cast<DWORD>(request.formatted_headers.size()),
                          WINHTTP_NO_REQUEST_DATA, 0, total_body_len, 0)) {
    return MakeWinHttpError(ctx, phase, "WinHttpSendRequest", GetLastError());
  }

  // Phase 2: Send request body (handling partial writes safely)
  if (total_body_len > 0) {
    phase = RequestPhase::kSendingBody;
    DWORD write_err = ERROR_SUCCESS;
    if (!WriteAllRequestBody(
            *request.body, ctx,
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
  DWORD headers_err = ERROR_SUCCESS;
  const std::wstring_view raw_headers_view = QueryWinHttpHeaderString(
      raw_request, WINHTTP_QUERY_RAW_HEADERS_CRLF, raw_headers_stack,
      static_cast<DWORD>(sizeof(raw_headers_stack)), &raw_headers_heap,
      &headers_err);
  if (headers_err != ERROR_SUCCESS) {
    return MakeWinHttpError(ctx, phase, "WinHttpQueryHeaders (raw headers)",
                            headers_err);
  }
  std::optional<int64_t> content_length;
  flutter::EncodableMap parsed_headers =
      ParseRawHttpHeaders(raw_headers_view, &content_length);

  // Phase 4: Read response body directly without intermediate chunk copies
  DWORD read_win32_err = ERROR_SUCCESS;
  ReadBodyResult read_result = ReadResponseBodyDirect(
      content_length, request.max_response_bytes, ctx,
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

// static
bool WinHttpTransport::WriteAllRequestBody(const std::vector<uint8_t>& body,
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
    if (bytes_written == 0 || bytes_written > remaining) {
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
ReadBodyResult WinHttpTransport::ReadResponseBodyDirect(
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
    return MakeReadError(
        "maxResponseBytesExceeded",
        "Response Content-Length (" + std::to_string(*content_length) +
            " bytes) exceeded maxResponseBytes limit (" +
            std::to_string(*safe_max) + " bytes)");
  }

  // Reserve initial capacity without zero-filling unread pages (at least 1 byte
  // so boundary +1 detection works even when maxResponseBytes == 0).
  int64_t reserve_size = kInitialUnknownBodyBytes;
  if (content_length.has_value() && *content_length > 0) {
    reserve_size = std::min<int64_t>(*content_length, kMaxPreallocBytes);
  }
  if (safe_max.has_value()) {
    const int64_t capped_max =
        (*safe_max < (std::numeric_limits<int64_t>::max)()) ? (*safe_max + 1)
                                                            : *safe_max;
    reserve_size = std::min<int64_t>(reserve_size, capped_max);
  }
  result.body.reserve(static_cast<size_t>(std::max<int64_t>(1, reserve_size)));

  size_t total_read = 0;
  for (;;) {
    if (ctx.is_cancelled.load()) {
      return MakeReadError("cancel", "Request was cancelled.");
    }

    if (total_read == result.body.capacity()) {
      const size_t grown =
          ComputeNextBufferSize(result.body.capacity(), safe_max);
      if (grown <= result.body.capacity()) {
        return MakeReadError("unknown", "Response buffer size limit exceeded.");
      }
      result.body.reserve(grown);
    }

    DWORD to_read = static_cast<DWORD>(
        std::min<size_t>(result.body.capacity() - total_read,
                         static_cast<size_t>(kReadBufferSizeBytes)));
    if (safe_max.has_value()) {
      const int64_t remaining_allowed =
          *safe_max - static_cast<int64_t>(total_read);
      if (remaining_allowed < static_cast<int64_t>(to_read)) {
        to_read = static_cast<DWORD>(remaining_allowed + 1);
      }
    }

    result.body.resize(total_read + static_cast<size_t>(to_read));

    DWORD bytes_read = 0;
    if (!read_fn(result.body.data() + total_read, to_read, &bytes_read)) {
      return ReadBodyResult{};
    }
    if (bytes_read > to_read) {
      return MakeReadError("unknown",
                           "Read callback reported more bytes than requested.");
    }
    if (bytes_read == 0) {
      break;
    }

    total_read += static_cast<size_t>(bytes_read);
    if (safe_max.has_value() && static_cast<int64_t>(total_read) > *safe_max) {
      return MakeReadError("maxResponseBytesExceeded",
                           "Response body exceeded maxResponseBytes limit (" +
                               std::to_string(*safe_max) + " bytes)");
    }
  }

  if (ctx.is_cancelled.load()) {
    return MakeReadError("cancel", "Request was cancelled.");
  }

  result.body.resize(total_read);
  result.ok = true;
  return result;
}

}  // namespace flutter_turbo_fetch
