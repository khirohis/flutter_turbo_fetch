#include "turbo_http_executor.h"
#include "winhttp_helpers.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "test/test_assertions.h"

namespace flutter_turbo_fetch {
namespace test {

namespace {

TurboHttpRequest CreateRequest(
    const std::string& request_id = "test-req",
    const std::string& url = "https://example.com/api",
    const std::string& method = "GET",
    const flutter::EncodableMap& headers = {},
    const std::vector<uint8_t>* body = nullptr,
    std::optional<int64_t> max_response_bytes = std::nullopt) {
  const int64_t timeout_ms = 5000;
  return TurboHttpRequest(
      request_id, url, method, headers, body, &timeout_ms, &timeout_ms,
      &timeout_ms, true, false,
      max_response_bytes.has_value() ? &(*max_response_bytes) : nullptr);
}

TurboHttpResponse ExecuteSync(TurboHttpExecutor& executor,
                              const TurboHttpRequest& request) {
  std::mutex mtx;
  std::condition_variable cv;
  std::optional<TurboHttpResponse> result_opt;
  std::atomic<int> call_count{0};

  executor.Execute(request, [&](TurboHttpResponse response) {
    call_count.fetch_add(1);
    {
      std::lock_guard<std::mutex> lock(mtx);
      result_opt = std::move(response);
    }
    cv.notify_one();
  });

  std::unique_lock<std::mutex> lock(mtx);
  bool finished = cv.wait_for(lock, std::chrono::seconds(5),
                              [&] { return result_opt.has_value(); });
  ASSERT_TRUE(finished);
  ASSERT_EQ(1, call_count.load());
  ASSERT_EQ(static_cast<size_t>(0), executor.active_request_count());
  ASSERT_FALSE(executor.has_active_request(request.request_id()));
  return std::move(*result_opt);
}

TurboHttpExecutor::ReadDataFn MakeSliceReader(
    const std::vector<uint8_t>& source,
    size_t max_chunk = (std::numeric_limits<size_t>::max)()) {
  size_t offset = 0;
  return [&source, max_chunk, offset](uint8_t* dst, DWORD max_len,
                                      DWORD* bytes_read) mutable -> bool {
    if (offset >= source.size()) {
      *bytes_read = 0;
      return true;
    }
    const size_t n = std::min<size_t>(
        {static_cast<size_t>(max_len), max_chunk, source.size() - offset});
    std::copy(source.begin() + offset, source.begin() + offset + n, dst);
    offset += n;
    *bytes_read = static_cast<DWORD>(n);
    return true;
  };
}

}  // namespace

TURBO_TEST(WinHttpHelpers_ClassifyWinHttpError_ClassifiesCanceledRequest) {
  ASSERT_EQ(std::string("cancel"),
            ClassifyWinHttpError(/*is_cancelled=*/true, ERROR_SUCCESS,
                                 RequestPhase::kReceiving));
  ASSERT_EQ(std::string("cancel"),
            ClassifyWinHttpError(/*is_cancelled=*/false,
                                 ERROR_WINHTTP_OPERATION_CANCELLED,
                                 RequestPhase::kReceiving));
  ASSERT_EQ(std::string("cancel"),
            ClassifyWinHttpError(/*is_cancelled=*/false, ERROR_INVALID_HANDLE,
                                 RequestPhase::kReceiving));
}

TURBO_TEST(WinHttpHelpers_ClassifyWinHttpError_ClassifiesTimeoutsCorrectly) {
  // Connect timeout
  ASSERT_EQ(std::string("connectionTimeout"),
            ClassifyWinHttpError(/*is_cancelled=*/false, ERROR_WINHTTP_TIMEOUT,
                                 RequestPhase::kConnecting));

  // Send timeout
  ASSERT_EQ(std::string("sendTimeout"),
            ClassifyWinHttpError(/*is_cancelled=*/false, ERROR_WINHTTP_TIMEOUT,
                                 RequestPhase::kSendingBody));

  // Receive timeout
  ASSERT_EQ(std::string("receiveTimeout"),
            ClassifyWinHttpError(/*is_cancelled=*/false, ERROR_WINHTTP_TIMEOUT,
                                 RequestPhase::kReceiving));
}

TURBO_TEST(
    WinHttpHelpers_ClassifyWinHttpError_ClassifiesCertificateAndConnectionErrors) {
  // Certificate / TLS errors
  ASSERT_EQ(std::string("badCertificate"),
            ClassifyWinHttpError(/*is_cancelled=*/false,
                                 ERROR_WINHTTP_SECURE_FAILURE,
                                 RequestPhase::kConnecting));
  ASSERT_EQ(std::string("badCertificate"),
            ClassifyWinHttpError(/*is_cancelled=*/false,
                                 ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED,
                                 RequestPhase::kConnecting));
  ASSERT_EQ(std::string("badCertificate"),
            ClassifyWinHttpError(/*is_cancelled=*/false,
                                 static_cast<DWORD>(SEC_E_UNTRUSTED_ROOT),
                                 RequestPhase::kConnecting));
  ASSERT_EQ(std::string("badCertificate"),
            ClassifyWinHttpError(/*is_cancelled=*/false,
                                 static_cast<DWORD>(SEC_E_CERT_EXPIRED),
                                 RequestPhase::kConnecting));

  // Connection errors
  ASSERT_EQ(std::string("connectionError"),
            ClassifyWinHttpError(/*is_cancelled=*/false,
                                 ERROR_WINHTTP_NAME_NOT_RESOLVED,
                                 RequestPhase::kConnecting));
  ASSERT_EQ(std::string("connectionError"),
            ClassifyWinHttpError(/*is_cancelled=*/false,
                                 ERROR_WINHTTP_CANNOT_CONNECT,
                                 RequestPhase::kConnecting));
  ASSERT_EQ(std::string("connectionError"),
            ClassifyWinHttpError(/*is_cancelled=*/false,
                                 ERROR_WINHTTP_CONNECTION_ERROR,
                                 RequestPhase::kReceiving));
  ASSERT_EQ(std::string("connectionError"),
            ClassifyWinHttpError(/*is_cancelled=*/false,
                                 ERROR_WINHTTP_INVALID_URL,
                                 RequestPhase::kConnecting));
  ASSERT_EQ(std::string("connectionError"),
            ClassifyWinHttpError(/*is_cancelled=*/false,
                                 ERROR_WINHTTP_UNRECOGNIZED_SCHEME,
                                 RequestPhase::kConnecting));
  ASSERT_EQ(std::string("connectionError"),
            ClassifyWinHttpError(/*is_cancelled=*/false,
                                 ERROR_WINHTTP_REDIRECT_FAILED,
                                 RequestPhase::kReceiving));

  // Unknown error
  ASSERT_EQ(std::string("unknown"),
            ClassifyWinHttpError(/*is_cancelled=*/false, ERROR_INTERNAL_ERROR,
                                 RequestPhase::kReceiving));
}

TURBO_TEST(TurboHttpExecutor_Execute_InvalidUrl_ReturnsConnectionError) {
  TurboHttpExecutor executor;
  TurboHttpResponse response =
      ExecuteSync(executor, CreateRequest("req-invalid-url", "http://[::1"));

  ASSERT_NE(nullptr, response.error_code());
  ASSERT_EQ(std::string("connectionError"), *response.error_code());
  ASSERT_NE(nullptr, response.error_message());
}

TURBO_TEST(
    TurboHttpExecutor_Execute_InvalidHeaderCharacters_ReturnsConnectionErrorSafelyWithoutCrash) {
  TurboHttpExecutor executor;
  flutter::EncodableMap invalid_headers = {
      {flutter::EncodableValue("X-Invalid\r\nHeader"),
       flutter::EncodableValue("value\r\ninjection")},
  };

  TurboHttpResponse response = ExecuteSync(
      executor,
      CreateRequest("req-invalid-hdr", "https://example.com/api", "GET",
                    invalid_headers));

  ASSERT_NE(nullptr, response.error_code());
  ASSERT_EQ(std::string("connectionError"), *response.error_code());
}

TURBO_TEST(
    WinHttpHelpers_ParseRawHttpHeaders_NormalizesToLowercaseAndAggregatesMultiValues) {
  const std::wstring raw_headers =
      L"HTTP/2 201 Created\r\n"
      L"Content-Type: application/json\r\n"
      L"X-Custom: v1\r\n"
      L"x-custom: v2\r\n"
      L"Content-Length: 19\r\n\r\n";

  std::optional<int64_t> content_length;
  flutter::EncodableMap parsed =
      ParseRawHttpHeaders(raw_headers, &content_length);

  ASSERT_TRUE(content_length.has_value());
  ASSERT_EQ(19, *content_length);

  auto ct_it = parsed.find(flutter::EncodableValue("content-type"));
  ASSERT_TRUE(ct_it != parsed.end());
  const auto& ct_list = std::get<flutter::EncodableList>(ct_it->second);
  ASSERT_EQ(static_cast<size_t>(1), ct_list.size());
  ASSERT_EQ(std::string("application/json"), std::get<std::string>(ct_list[0]));

  auto xc_it = parsed.find(flutter::EncodableValue("x-custom"));
  ASSERT_TRUE(xc_it != parsed.end());
  const auto& xc_list = std::get<flutter::EncodableList>(xc_it->second);
  ASSERT_EQ(static_cast<size_t>(2), xc_list.size());
  ASSERT_EQ(std::string("v1"), std::get<std::string>(xc_list[0]));
  ASSERT_EQ(std::string("v2"), std::get<std::string>(xc_list[1]));
}

TURBO_TEST(
    WinHttpHelpers_ParseRawHttpHeaders_MalformedLinesAndInvalidContentLength_HandlesSafely) {
  const std::wstring raw_headers =
      L"HTTP/2 200 OK\r\n"
      L": missing-key\r\n"
      L"MalformedHeaderWithoutColon\r\n"
      L"   \r\n"
      L"Content-Length: not-a-number\r\n"
      L"X-Valid: ok\r\n\r\n";

  std::optional<int64_t> content_length;
  flutter::EncodableMap parsed =
      ParseRawHttpHeaders(raw_headers, &content_length);

  ASSERT_FALSE(content_length.has_value());
  auto valid_it = parsed.find(flutter::EncodableValue("x-valid"));
  ASSERT_TRUE(valid_it != parsed.end());
  const auto& val_list = std::get<flutter::EncodableList>(valid_it->second);
  ASSERT_EQ(static_cast<size_t>(1), val_list.size());
  ASSERT_EQ(std::string("ok"), std::get<std::string>(val_list[0]));
}

TURBO_TEST(
    TurboHttpExecutor_Execute_MaxResponseBytesNull_ReturnsFullBodyAndHeaders) {
  const std::vector<uint8_t> expected_bytes = {'t', 'u', 'r', 'b', 'o'};
  TurboHttpExecutor executor(
      [expected_bytes](const TurboHttpRequest& /*req*/,
                       RequestContext& /*ctx*/) -> TurboHttpResponse {
        TurboHttpResponse res;
        res.set_status_code(201);
        res.set_status_message("Created");
        flutter::EncodableMap headers = {
            {flutter::EncodableValue("x-custom"),
             flutter::EncodableValue(flutter::EncodableList{
                 flutter::EncodableValue("v1"),
                 flutter::EncodableValue("v2"),
             })},
        };
        res.set_headers(headers);
        res.set_body(expected_bytes);
        return res;
      });

  TurboHttpResponse response =
      ExecuteSync(executor, CreateRequest("req-ok", "https://example.com/api"));
  ASSERT_EQ(nullptr, response.error_code());
  ASSERT_NE(nullptr, response.status_code());
  ASSERT_EQ(201, *response.status_code());
  ASSERT_NE(nullptr, response.body());
  ASSERT_EQ(expected_bytes, *response.body());
}

TURBO_TEST(TurboHttpExecutor_Cancel_CancelsInFlightRequest) {
  std::mutex sync_mtx;
  std::condition_variable started_cv;
  std::condition_variable finish_cv;
  bool request_started = false;
  bool allow_finish = false;

  TurboHttpExecutor executor(
      [&](const TurboHttpRequest& /*req*/,
          RequestContext& ctx) -> TurboHttpResponse {
        {
          std::lock_guard<std::mutex> lock(sync_mtx);
          request_started = true;
        }
        started_cv.notify_one();

        {
          std::unique_lock<std::mutex> lock(sync_mtx);
          finish_cv.wait_for(lock, std::chrono::seconds(5),
                             [&] { return allow_finish || ctx.is_cancelled.load(); });
        }

        if (ctx.is_cancelled.load()) {
          TurboHttpResponse res;
          res.set_error_code("cancel");
          res.set_error_message("Request cancelled");
          return res;
        }
        TurboHttpResponse res;
        res.set_status_code(200);
        return res;
      });

  std::mutex res_mtx;
  std::condition_variable res_cv;
  std::optional<TurboHttpResponse> final_res;
  std::atomic<int> call_count{0};

  executor.Execute(CreateRequest("cancel-target"), [&](TurboHttpResponse res) {
    call_count.fetch_add(1);
    {
      std::lock_guard<std::mutex> lock(res_mtx);
      final_res = std::move(res);
    }
    res_cv.notify_one();
  });

  {
    std::unique_lock<std::mutex> lock(sync_mtx);
    ASSERT_TRUE(started_cv.wait_for(lock, std::chrono::seconds(5),
                                    [&] { return request_started; }));
  }

  executor.Cancel("cancel-target");
  {
    std::lock_guard<std::mutex> lock(sync_mtx);
    allow_finish = true;
  }
  finish_cv.notify_all();

  std::unique_lock<std::mutex> res_lock(res_mtx);
  ASSERT_TRUE(res_cv.wait_for(res_lock, std::chrono::seconds(5),
                              [&] { return final_res.has_value(); }));
  ASSERT_NE(nullptr, final_res->error_code());
  ASSERT_EQ(std::string("cancel"), *final_res->error_code());
  ASSERT_EQ(1, call_count.load());
  ASSERT_EQ(static_cast<size_t>(0), executor.active_request_count());
  ASSERT_FALSE(executor.has_active_request("cancel-target"));
}

TURBO_TEST(
    TurboHttpExecutor_Cancel_ImmediatelyAfterExecute_CancelsSafelyAndInvokesCallbackExactlyOnce) {
  std::mutex sync_mtx;
  std::condition_variable finish_cv;
  bool allow_finish = false;

  TurboHttpExecutor executor(
      [&](const TurboHttpRequest& /*req*/,
          RequestContext& ctx) -> TurboHttpResponse {
        {
          std::unique_lock<std::mutex> lock(sync_mtx);
          finish_cv.wait_for(lock, std::chrono::seconds(5),
                             [&] { return allow_finish || ctx.is_cancelled.load(); });
        }

        if (ctx.is_cancelled.load()) {
          TurboHttpResponse res;
          res.set_error_code("cancel");
          res.set_error_message("Request cancelled immediately");
          return res;
        }
        TurboHttpResponse res;
        res.set_status_code(200);
        return res;
      });

  std::mutex res_mtx;
  std::condition_variable res_cv;
  std::optional<TurboHttpResponse> final_res;
  std::atomic<int> call_count{0};

  executor.Execute(CreateRequest("race-cancel"), [&](TurboHttpResponse res) {
    call_count.fetch_add(1);
    {
      std::lock_guard<std::mutex> lock(res_mtx);
      final_res = std::move(res);
    }
    res_cv.notify_one();
  });

  // Cancel immediately right after Execute (testing race condition)
  executor.Cancel("race-cancel");

  {
    std::lock_guard<std::mutex> lock(sync_mtx);
    allow_finish = true;
  }
  finish_cv.notify_all();

  std::unique_lock<std::mutex> res_lock(res_mtx);
  ASSERT_TRUE(res_cv.wait_for(res_lock, std::chrono::seconds(5),
                              [&] { return final_res.has_value(); }));
  ASSERT_NE(nullptr, final_res->error_code());
  ASSERT_EQ(std::string("cancel"), *final_res->error_code());
  ASSERT_EQ(1, call_count.load());
  ASSERT_EQ(static_cast<size_t>(0), executor.active_request_count());
}

TURBO_TEST(
    TurboHttpExecutor_Shutdown_CancelsInFlightRequestsAndCleansUpActiveRequests) {
  std::mutex sync_mtx;
  std::condition_variable started_cv;
  std::condition_variable finish_cv;
  bool request_started = false;
  bool allow_finish = false;

  TurboHttpExecutor executor(
      [&](const TurboHttpRequest& /*req*/,
          RequestContext& ctx) -> TurboHttpResponse {
        {
          std::lock_guard<std::mutex> lock(sync_mtx);
          request_started = true;
        }
        started_cv.notify_one();

        {
          std::unique_lock<std::mutex> lock(sync_mtx);
          finish_cv.wait_for(lock, std::chrono::seconds(5),
                             [&] { return allow_finish || ctx.is_cancelled.load(); });
        }

        if (ctx.is_cancelled.load()) {
          TurboHttpResponse res;
          res.set_error_code("cancel");
          res.set_error_message("Request cancelled by shutdown");
          return res;
        }
        TurboHttpResponse res;
        res.set_status_code(200);
        return res;
      });

  std::mutex res_mtx;
  std::condition_variable res_cv;
  std::optional<TurboHttpResponse> final_res;
  std::atomic<int> call_count{0};

  executor.Execute(CreateRequest("shutdown-target"), [&](TurboHttpResponse res) {
    call_count.fetch_add(1);
    {
      std::lock_guard<std::mutex> lock(res_mtx);
      final_res = std::move(res);
    }
    res_cv.notify_one();
  });

  {
    std::unique_lock<std::mutex> lock(sync_mtx);
    ASSERT_TRUE(started_cv.wait_for(lock, std::chrono::seconds(5),
                                    [&] { return request_started; }));
  }

  // Shutdown cancels all in-flight requests and cleans up threadpool
  executor.Shutdown();

  {
    std::lock_guard<std::mutex> lock(sync_mtx);
    allow_finish = true;
  }
  finish_cv.notify_all();

  std::unique_lock<std::mutex> res_lock(res_mtx);
  ASSERT_TRUE(res_cv.wait_for(res_lock, std::chrono::seconds(5),
                              [&] { return final_res.has_value(); }));
  ASSERT_NE(nullptr, final_res->error_code());
  ASSERT_EQ(std::string("cancel"), *final_res->error_code());
  ASSERT_EQ(1, call_count.load());
  ASSERT_EQ(static_cast<size_t>(0), executor.active_request_count());
}

TURBO_TEST(RequestContext_SetRequestHandle_AfterCancel_ClosesHandleImmediately) {
  RequestContext ctx;
  ctx.is_cancelled = true;

  HINTERNET test_handle = WinHttpOpen(
      L"flutter_turbo_fetch_test/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  ASSERT_NE(nullptr, test_handle);

  // When context was already cancelled, SetRequestHandle must close the handle and return false
  bool accepted = ctx.SetRequestHandle(test_handle);
  ASSERT_FALSE(accepted);
  ASSERT_EQ(nullptr, ctx.h_request);
}

TURBO_TEST(TurboHttpExecutor_Execute_PostWithRequestBody_TransmitsPayloadCorrectly) {
  const std::vector<uint8_t> payload = {'p', 'o', 's', 't', '_', 'o', 'k'};
  std::vector<uint8_t> captured_body;

  TurboHttpExecutor executor(
      [&](const TurboHttpRequest& req, RequestContext& /*ctx*/) -> TurboHttpResponse {
        ASSERT_EQ(std::string("POST"), req.method());
        if (req.body() != nullptr) {
          captured_body = *req.body();
        }
        TurboHttpResponse res;
        res.set_status_code(200);
        res.set_body(payload);
        return res;
      });

  TurboHttpResponse response = ExecuteSync(
      executor, CreateRequest("req-post", "https://example.com/api", "POST", {}, &payload));

  ASSERT_EQ(nullptr, response.error_code());
  ASSERT_NE(nullptr, response.status_code());
  ASSERT_EQ(200, *response.status_code());
  ASSERT_EQ(payload, captured_body);
  ASSERT_NE(nullptr, response.body());
  ASSERT_EQ(payload, *response.body());
}

TURBO_TEST(TurboHttpExecutor_Execute_PostWithNullBody_HandlesSafelyWithoutCrash) {
  bool executed = false;

  TurboHttpExecutor executor(
      [&](const TurboHttpRequest& req, RequestContext& /*ctx*/) -> TurboHttpResponse {
        ASSERT_EQ(std::string("POST"), req.method());
        ASSERT_EQ(nullptr, req.body());
        executed = true;

        TurboHttpResponse res;
        res.set_status_code(204);
        res.set_status_message("No Content");
        res.set_body(std::vector<uint8_t>{});
        return res;
      });

  TurboHttpResponse response = ExecuteSync(
      executor, CreateRequest("req-post-null", "https://example.com/api", "POST", {}, nullptr));

  ASSERT_TRUE(executed);
  ASSERT_EQ(nullptr, response.error_code());
  ASSERT_NE(nullptr, response.status_code());
  ASSERT_EQ(204, *response.status_code());
  ASSERT_NE(nullptr, response.body());
  ASSERT_TRUE(response.body()->empty());
}

TURBO_TEST(TurboHttpExecutor_Execute_EmptyResponseBody_ReturnsEmptyBodySafely) {
  TurboHttpExecutor executor(
      [](const TurboHttpRequest& /*req*/, RequestContext& /*ctx*/) -> TurboHttpResponse {
        TurboHttpResponse res;
        res.set_status_code(200);
        res.set_body(std::vector<uint8_t>{});
        return res;
      });

  TurboHttpResponse response =
      ExecuteSync(executor, CreateRequest("req-empty", "https://example.com/api"));

  ASSERT_EQ(nullptr, response.error_code());
  ASSERT_NE(nullptr, response.status_code());
  ASSERT_EQ(200, *response.status_code());
  ASSERT_NE(nullptr, response.body());
  ASSERT_TRUE(response.body()->empty());
}

TURBO_TEST(
    TurboHttpExecutor_WriteAllRequestBody_HandlesPartialWritesAndCompletes) {
  std::vector<uint8_t> body(100);
  for (size_t i = 0; i < body.size(); ++i) {
    body[i] = static_cast<uint8_t>(i & 0xff);
  }

  RequestContext ctx;
  std::vector<uint8_t> received;
  const std::vector<DWORD> chunk_limits = {30, 40, 30};
  size_t call_index = 0;
  DWORD win32_err = ERROR_SUCCESS;

  bool ok = TurboHttpExecutor::WriteAllRequestBody(
      body, ctx,
      [&](const uint8_t* data, DWORD len, DWORD* bytes_written) -> bool {
        ASSERT_TRUE(call_index < chunk_limits.size());
        DWORD to_take =
            (len < chunk_limits[call_index]) ? len : chunk_limits[call_index];
        received.insert(received.end(), data, data + to_take);
        *bytes_written = to_take;
        ++call_index;
        return true;
      },
      &win32_err);

  ASSERT_TRUE(ok);
  ASSERT_EQ(static_cast<size_t>(3), call_index);
  ASSERT_EQ(body, received);
}

TURBO_TEST(
    TurboHttpExecutor_WriteAllRequestBody_StopsOnCancelBetweenChunks) {
  const std::vector<uint8_t> body(64, 0xAB);
  RequestContext ctx;
  int call_count = 0;
  DWORD win32_err = ERROR_SUCCESS;

  bool ok = TurboHttpExecutor::WriteAllRequestBody(
      body, ctx,
      [&](const uint8_t* /*data*/, DWORD /*len*/,
          DWORD* bytes_written) -> bool {
        ++call_count;
        *bytes_written = 16;
        ctx.is_cancelled = true;
        return true;
      },
      &win32_err);

  ASSERT_FALSE(ok);
  ASSERT_EQ(1, call_count);
  ASSERT_EQ(static_cast<DWORD>(ERROR_WINHTTP_OPERATION_CANCELLED), win32_err);
}

TURBO_TEST(
    TurboHttpExecutor_WriteAllRequestBody_StopsOnZeroByteWriteProgress) {
  const std::vector<uint8_t> body(32, 0xCD);
  RequestContext ctx;
  int call_count = 0;
  DWORD win32_err = ERROR_SUCCESS;

  bool ok = TurboHttpExecutor::WriteAllRequestBody(
      body, ctx,
      [&](const uint8_t* /*data*/, DWORD /*len*/,
          DWORD* bytes_written) -> bool {
        ++call_count;
        *bytes_written = 0;
        return true;
      },
      &win32_err);

  ASSERT_FALSE(ok);
  ASSERT_EQ(1, call_count);
  ASSERT_EQ(static_cast<DWORD>(ERROR_WINHTTP_CONNECTION_ERROR), win32_err);
}

TURBO_TEST(
    TurboHttpExecutor_ReadResponseBodyDirect_ReadsKnownUnknownAndExpandedLengths) {
  RequestContext ctx;

  // 1. Known Content-Length read in multiple chunks
  {
    const std::vector<uint8_t> source(256, 0x5A);
    ReadBodyResult result = TurboHttpExecutor::ReadResponseBodyDirect(
        /*content_length=*/256, /*max_response_bytes=*/std::nullopt, ctx,
        MakeSliceReader(source, /*max_chunk=*/64));

    ASSERT_TRUE(result.ok);
    ASSERT_EQ(source, result.body);
  }

  // 2. Unknown Content-Length (chunked / HTTP/2 streaming) triggering growth
  {
    const std::vector<uint8_t> source(150000, 0x3C);
    ReadBodyResult result = TurboHttpExecutor::ReadResponseBodyDirect(
        /*content_length=*/std::nullopt, /*max_response_bytes=*/std::nullopt,
        ctx, MakeSliceReader(source, /*max_chunk=*/40000));

    ASSERT_TRUE(result.ok);
    ASSERT_EQ(source, result.body);
  }

  // 3. Body larger than Content-Length (e.g., transparent gzip decompression)
  {
    const std::vector<uint8_t> source(1024, 0x7E);
    ReadBodyResult result = TurboHttpExecutor::ReadResponseBodyDirect(
        /*content_length=*/128, /*max_response_bytes=*/2048, ctx,
        MakeSliceReader(source, /*max_chunk=*/256));

    ASSERT_TRUE(result.ok);
    ASSERT_EQ(source, result.body);
  }
}

TURBO_TEST(
    TurboHttpExecutor_ReadResponseBodyDirect_EnforcesMaxResponseBytesAndBoundary) {
  RequestContext ctx;

  // 1. Content-Length exceeds maxResponseBytes: read_fn must never be called
  {
    int read_calls = 0;
    ReadBodyResult result = TurboHttpExecutor::ReadResponseBodyDirect(
        /*content_length=*/100, /*max_response_bytes=*/64, ctx,
        [&](uint8_t* /*dst*/, DWORD /*max_len*/,
            DWORD* bytes_read) -> bool {
          ++read_calls;
          *bytes_read = 0;
          return true;
        });

    ASSERT_FALSE(result.ok);
    ASSERT_EQ(0, read_calls);
    ASSERT_EQ(std::string("maxResponseBytesExceeded"), result.error_code);
    ASSERT_TRUE(result.body.empty());
  }

  // 2. Exact boundary succeeds
  {
    const std::vector<uint8_t> source(64, 0x11);
    ReadBodyResult result = TurboHttpExecutor::ReadResponseBodyDirect(
        /*content_length=*/std::nullopt, /*max_response_bytes=*/64, ctx,
        MakeSliceReader(source));

    ASSERT_TRUE(result.ok);
    ASSERT_EQ(source, result.body);
  }

  // 3. Exceeding by 1 byte aborts immediately and clears body
  {
    const std::vector<uint8_t> source(65, 0x22);
    ReadBodyResult result = TurboHttpExecutor::ReadResponseBodyDirect(
        /*content_length=*/std::nullopt, /*max_response_bytes=*/64, ctx,
        MakeSliceReader(source));

    ASSERT_FALSE(result.ok);
    ASSERT_EQ(std::string("maxResponseBytesExceeded"), result.error_code);
    ASSERT_TRUE(result.body.empty());
  }

  // 4. Empty body with maxResponseBytes == 0 succeeds
  {
    ReadBodyResult result = TurboHttpExecutor::ReadResponseBodyDirect(
        /*content_length=*/0, /*max_response_bytes=*/0, ctx,
        [](uint8_t* /*dst*/, DWORD /*max_len*/, DWORD* bytes_read) -> bool {
          *bytes_read = 0;
          return true;
        });

    ASSERT_TRUE(result.ok);
    ASSERT_TRUE(result.body.empty());
  }

  // 5. 1-byte body with maxResponseBytes == 0 fails
  {
    ReadBodyResult result = TurboHttpExecutor::ReadResponseBodyDirect(
        /*content_length=*/std::nullopt, /*max_response_bytes=*/0, ctx,
        [](uint8_t* dst, DWORD max_len, DWORD* bytes_read) -> bool {
          ASSERT_TRUE(max_len >= 1);
          dst[0] = 0xFF;
          *bytes_read = 1;
          return true;
        });

    ASSERT_FALSE(result.ok);
    ASSERT_EQ(std::string("maxResponseBytesExceeded"), result.error_code);
    ASSERT_TRUE(result.body.empty());
  }

  // 6. INT64_MAX boundary succeeds without overflow
  {
    const std::vector<uint8_t> source(64, 0x33);
    ReadBodyResult result = TurboHttpExecutor::ReadResponseBodyDirect(
        /*content_length=*/64, /*max_response_bytes=*/INT64_MAX, ctx,
        MakeSliceReader(source));

    ASSERT_TRUE(result.ok);
    ASSERT_EQ(source, result.body);
  }
}

TURBO_TEST(TurboHttpExecutor_ReadResponseBodyDirect_CancelsMidStream) {
  RequestContext ctx;
  int read_calls = 0;

  ReadBodyResult result = TurboHttpExecutor::ReadResponseBodyDirect(
      /*content_length=*/std::nullopt, /*max_response_bytes=*/std::nullopt, ctx,
      [&](uint8_t* dst, DWORD max_len, DWORD* bytes_read) -> bool {
        ++read_calls;
        DWORD n = (max_len < 32) ? max_len : 32;
        std::fill(dst, dst + n, static_cast<uint8_t>(0xEE));
        *bytes_read = n;
        ctx.is_cancelled = true;
        return true;
      });

  ASSERT_FALSE(result.ok);
  ASSERT_EQ(1, read_calls);
  ASSERT_EQ(std::string("cancel"), result.error_code);
}

TURBO_TEST(
    WinHttpHelpers_ParseRawHttpHeaders_HandlesWhitespaceColonsAndContentLengthEdgeCases) {
  const std::wstring raw_headers =
      L"HTTP/2 200 OK\r\n"
      L"  Date :  Thu, 26 Nov 2026 11:00:00 GMT  \r\n"
      L"X-Empty:   \r\n"
      L"Set-Cookie: a=1\r\n"
      L"set-cookie: b=2\r\n"
      L"Content-Length: 123abc\r\n\r\n";

  std::optional<int64_t> content_length;
  flutter::EncodableMap parsed =
      ParseRawHttpHeaders(raw_headers, &content_length);

  // Trailing garbage in Content-Length must be rejected
  ASSERT_FALSE(content_length.has_value());

  auto date_it = parsed.find(flutter::EncodableValue("date"));
  ASSERT_TRUE(date_it != parsed.end());
  const auto& date_list = std::get<flutter::EncodableList>(date_it->second);
  ASSERT_EQ(static_cast<size_t>(1), date_list.size());
  ASSERT_EQ(std::string("Thu, 26 Nov 2026 11:00:00 GMT"),
            std::get<std::string>(date_list[0]));

  auto empty_it = parsed.find(flutter::EncodableValue("x-empty"));
  ASSERT_TRUE(empty_it != parsed.end());
  const auto& empty_list = std::get<flutter::EncodableList>(empty_it->second);
  ASSERT_EQ(static_cast<size_t>(1), empty_list.size());
  ASSERT_EQ(std::string(""), std::get<std::string>(empty_list[0]));

  auto cookie_it = parsed.find(flutter::EncodableValue("set-cookie"));
  ASSERT_TRUE(cookie_it != parsed.end());
  const auto& cookie_list = std::get<flutter::EncodableList>(cookie_it->second);
  ASSERT_EQ(static_cast<size_t>(2), cookie_list.size());
  ASSERT_EQ(std::string("a=1"), std::get<std::string>(cookie_list[0]));
  ASSERT_EQ(std::string("b=2"), std::get<std::string>(cookie_list[1]));

  // Negative and overflowing Content-Length must be ignored
  std::optional<int64_t> neg_len;
  ParseRawHttpHeaders(L"HTTP/2 200 OK\r\nContent-Length: -5\r\n\r\n", &neg_len);
  ASSERT_FALSE(neg_len.has_value());

  std::optional<int64_t> overflow_len;
  ParseRawHttpHeaders(
      L"HTTP/2 200 OK\r\nContent-Length: 999999999999999999999999\r\n\r\n",
      &overflow_len);
  ASSERT_FALSE(overflow_len.has_value());
}

TURBO_TEST(
    WinHttpHelpers_ParseRawHttpHeaders_HandlesAsciiAndNonAsciiHeaderValues) {
  // Pure ASCII headers (exercises ASCII fast path)
  {
    const std::wstring ascii_raw =
        L"HTTP/2 200 OK\r\nX-Ascii: hello-world-123\r\n\r\n";
    std::optional<int64_t> content_length;
    flutter::EncodableMap parsed =
        ParseRawHttpHeaders(ascii_raw, &content_length);
    auto it = parsed.find(flutter::EncodableValue("x-ascii"));
    ASSERT_TRUE(it != parsed.end());
    const auto& list = std::get<flutter::EncodableList>(it->second);
    ASSERT_EQ(static_cast<size_t>(1), list.size());
    ASSERT_EQ(std::string("hello-world-123"), std::get<std::string>(list[0]));
  }

  // Non-ASCII UTF-16 header value (exercises WideCharToMultiByte fallback)
  {
    const std::wstring unicode_raw =
        L"HTTP/2 200 OK\r\nX-Unicode: caf\u00E9-\u30C6\u30B9\u30C8\r\n\r\n";
    std::optional<int64_t> content_length;
    flutter::EncodableMap parsed =
        ParseRawHttpHeaders(unicode_raw, &content_length);
    auto it = parsed.find(flutter::EncodableValue("x-unicode"));
    ASSERT_TRUE(it != parsed.end());
    const auto& list = std::get<flutter::EncodableList>(it->second);
    ASSERT_EQ(static_cast<size_t>(1), list.size());
    ASSERT_EQ(std::string("caf\xC3\xA9-\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88"),
              std::get<std::string>(list[0]));
  }
}

TURBO_TEST(
    TurboHttpExecutor_Execute_HandlesNonAsciiUrlAndHeaderValueSafely) {
  TurboHttpExecutor executor(
      [](const TurboHttpRequest& /*req*/, RequestContext& /*ctx*/) -> TurboHttpResponse {
        TurboHttpResponse res;
        res.set_status_code(200);
        res.set_body(std::vector<uint8_t>{});
        return res;
      });

  flutter::EncodableMap headers = {
      {flutter::EncodableValue("X-Label"),
       flutter::EncodableValue("caf\xC3\xA9-\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88")},
  };

  TurboHttpResponse response = ExecuteSync(
      executor,
      CreateRequest(
          "req-unicode",
          "https://example.com/search?q=\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88",
          "GET", headers));
  ASSERT_EQ(nullptr, response.error_code());
  ASSERT_NE(nullptr, response.status_code());
  ASSERT_EQ(200, *response.status_code());
}

TURBO_TEST(
    TurboHttpExecutor_Execute_ConcurrentRequests_CompleteSafelyWithZeroActiveLeft) {
  constexpr int kConcurrency = 16;
  TurboHttpExecutor executor(
      [](const TurboHttpRequest& req, RequestContext& /*ctx*/) -> TurboHttpResponse {
        TurboHttpResponse res;
        res.set_status_code(200);
        res.set_body(std::vector<uint8_t>(req.request_id().begin(),
                                          req.request_id().end()));
        return res;
      });

  std::mutex mtx;
  std::condition_variable cv;
  int completed = 0;

  for (int i = 0; i < kConcurrency; ++i) {
    const std::string req_id = "concurrent-" + std::to_string(i);
    executor.Execute(
        CreateRequest(req_id, "https://example.com/item/" + std::to_string(i)),
        [&](TurboHttpResponse res) {
          ASSERT_EQ(nullptr, res.error_code());
          ASSERT_NE(nullptr, res.status_code());
          ASSERT_EQ(200, *res.status_code());
          {
            std::lock_guard<std::mutex> lock(mtx);
            ++completed;
          }
          cv.notify_one();
        });
  }

  std::unique_lock<std::mutex> lock(mtx);
  ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5),
                          [&] { return completed == kConcurrency; }));
  ASSERT_EQ(static_cast<size_t>(0), executor.active_request_count());
}

TURBO_TEST(
    TurboHttpExecutor_Execute_AfterShutdown_ReturnsCancelImmediately) {
  TurboHttpExecutor executor;
  executor.Shutdown();

  TurboHttpResponse response =
      ExecuteSync(executor, CreateRequest("req-after-shutdown"));
  ASSERT_NE(nullptr, response.error_code());
  ASSERT_EQ(std::string("cancel"), *response.error_code());
  ASSERT_EQ(static_cast<size_t>(0), executor.active_request_count());
}

TURBO_TEST(
    TurboHttpExecutor_ExecuteAndShutdown_ConcurrentStress_CompletesSafely) {
  constexpr int kRequests = 32;
  TurboHttpExecutor executor(
      [](const TurboHttpRequest& /*req*/,
         RequestContext& ctx) -> TurboHttpResponse {
        if (ctx.is_cancelled.load()) {
          TurboHttpResponse res;
          res.set_error_code("cancel");
          res.set_error_message("Cancelled");
          return res;
        }
        TurboHttpResponse res;
        res.set_status_code(200);
        return res;
      });

  std::mutex mtx;
  std::condition_variable cv;
  int completed = 0;

  std::thread shutdown_thread([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    executor.Shutdown();
  });

  for (int i = 0; i < kRequests; ++i) {
    executor.Execute(
        CreateRequest("stress-" + std::to_string(i)),
        [&](TurboHttpResponse /*res*/) {
          {
            std::lock_guard<std::mutex> lock(mtx);
            ++completed;
          }
          cv.notify_one();
        });
  }

  shutdown_thread.join();

  std::unique_lock<std::mutex> lock(mtx);
  ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5),
                          [&] { return completed == kRequests; }));
  ASSERT_EQ(static_cast<size_t>(0), executor.active_request_count());
}

TURBO_TEST(
    TurboHttpExecutor_DuplicateRequestId_OldCompletionDoesNotEraseNewEntry) {
  std::mutex sync_mtx;
  std::condition_variable cv;
  int started_count = 0;
  bool release_first = false;
  bool release_second = false;

  TurboHttpExecutor executor(
      [&](const TurboHttpRequest& /*req*/,
          RequestContext& /*ctx*/) -> TurboHttpResponse {
        int my_index = 0;
        {
          std::lock_guard<std::mutex> lock(sync_mtx);
          my_index = ++started_count;
        }
        cv.notify_all();

        std::unique_lock<std::mutex> lock(sync_mtx);
        if (my_index == 1) {
          cv.wait_for(lock, std::chrono::seconds(5),
                      [&] { return release_first; });
        } else {
          cv.wait_for(lock, std::chrono::seconds(5),
                      [&] { return release_second; });
        }

        TurboHttpResponse res;
        res.set_status_code(200);
        return res;
      });

  std::mutex done_mtx;
  std::condition_variable done_cv;
  int done_count = 0;

  // Dispatch first request with duplicate ID
  executor.Execute(CreateRequest("dup-id"), [&](TurboHttpResponse /*res*/) {
    {
      std::lock_guard<std::mutex> lock(done_mtx);
      ++done_count;
    }
    done_cv.notify_all();
  });

  {
    std::unique_lock<std::mutex> lock(sync_mtx);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5),
                            [&] { return started_count == 1; }));
  }

  // Dispatch second request with the same ID while first is still in flight
  executor.Execute(CreateRequest("dup-id"), [&](TurboHttpResponse /*res*/) {
    {
      std::lock_guard<std::mutex> lock(done_mtx);
      ++done_count;
    }
    done_cv.notify_all();
  });

  {
    std::unique_lock<std::mutex> lock(sync_mtx);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5),
                            [&] { return started_count == 2; }));
    release_first = true;
  }
  cv.notify_all();

  // Wait for first request to complete; second request's entry must remain active
  {
    std::unique_lock<std::mutex> lock(done_mtx);
    ASSERT_TRUE(done_cv.wait_for(lock, std::chrono::seconds(5),
                                 [&] { return done_count == 1; }));
  }
  ASSERT_TRUE(executor.has_active_request("dup-id"));

  // Release second request and verify clean completion
  {
    std::lock_guard<std::mutex> lock(sync_mtx);
    release_second = true;
  }
  cv.notify_all();

  {
    std::unique_lock<std::mutex> lock(done_mtx);
    ASSERT_TRUE(done_cv.wait_for(lock, std::chrono::seconds(5),
                                 [&] { return done_count == 2; }));
  }
  ASSERT_EQ(static_cast<size_t>(0), executor.active_request_count());
}

TURBO_TEST(
    TurboHttpExecutor_ReadResponseBodyDirect_AbortsOnOverflowingBytesRead) {
  RequestContext ctx;
  ReadBodyResult result = TurboHttpExecutor::ReadResponseBodyDirect(
      /*content_length=*/std::nullopt, /*max_response_bytes=*/std::nullopt, ctx,
      [](uint8_t* /*dst*/, DWORD max_len, DWORD* bytes_read) -> bool {
        *bytes_read = max_len + 1;
        return true;
      });

  ASSERT_FALSE(result.ok);
  ASSERT_TRUE(result.body.empty());
  ASSERT_EQ(std::string("unknown"), result.error_code);
}

}  // namespace test
}  // namespace flutter_turbo_fetch

