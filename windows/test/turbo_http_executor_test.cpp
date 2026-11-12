#include "turbo_http_executor.h"

#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
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
      &timeout_ms, true, 32, false,
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

}  // namespace

TURBO_TEST(TurboHttpExecutor_ClassifyError_ClassifiesCanceledRequest) {
  RequestContext ctx;
  ctx.is_cancelled = true;
  ASSERT_EQ(std::string("cancel"),
            TurboHttpExecutor::ClassifyError(ctx, ERROR_SUCCESS,
                                             RequestPhase::kReceiving));

  RequestContext uncancelled_ctx;
  ASSERT_EQ(std::string("cancel"),
            TurboHttpExecutor::ClassifyError(
                uncancelled_ctx, ERROR_WINHTTP_OPERATION_CANCELLED,
                RequestPhase::kReceiving));
  ASSERT_EQ(std::string("cancel"),
            TurboHttpExecutor::ClassifyError(uncancelled_ctx,
                                             ERROR_INVALID_HANDLE,
                                             RequestPhase::kReceiving));
}

TURBO_TEST(TurboHttpExecutor_ClassifyError_ClassifiesTimeoutsCorrectly) {
  RequestContext ctx;

  // Connect timeout
  ASSERT_EQ(std::string("connectionTimeout"),
            TurboHttpExecutor::ClassifyError(ctx, ERROR_WINHTTP_TIMEOUT,
                                             RequestPhase::kConnecting));

  // Send timeout
  ASSERT_EQ(std::string("sendTimeout"),
            TurboHttpExecutor::ClassifyError(ctx, ERROR_WINHTTP_TIMEOUT,
                                             RequestPhase::kSendingBody));

  // Receive timeout
  ASSERT_EQ(std::string("receiveTimeout"),
            TurboHttpExecutor::ClassifyError(ctx, ERROR_WINHTTP_TIMEOUT,
                                             RequestPhase::kReceiving));
}

TURBO_TEST(
    TurboHttpExecutor_ClassifyError_ClassifiesCertificateAndConnectionErrors) {
  RequestContext ctx;

  // Certificate / TLS errors
  ASSERT_EQ(std::string("badCertificate"),
            TurboHttpExecutor::ClassifyError(ctx, ERROR_WINHTTP_SECURE_FAILURE,
                                             RequestPhase::kConnecting));
  ASSERT_EQ(std::string("badCertificate"),
            TurboHttpExecutor::ClassifyError(
                ctx, ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED,
                RequestPhase::kConnecting));
  ASSERT_EQ(std::string("badCertificate"),
            TurboHttpExecutor::ClassifyError(
                ctx, static_cast<DWORD>(SEC_E_UNTRUSTED_ROOT),
                RequestPhase::kConnecting));
  ASSERT_EQ(std::string("badCertificate"),
            TurboHttpExecutor::ClassifyError(
                ctx, static_cast<DWORD>(SEC_E_CERT_EXPIRED),
                RequestPhase::kConnecting));

  // Connection errors
  ASSERT_EQ(std::string("connectionError"),
            TurboHttpExecutor::ClassifyError(
                ctx, ERROR_WINHTTP_NAME_NOT_RESOLVED,
                RequestPhase::kConnecting));
  ASSERT_EQ(std::string("connectionError"),
            TurboHttpExecutor::ClassifyError(ctx, ERROR_WINHTTP_CANNOT_CONNECT,
                                             RequestPhase::kConnecting));
  ASSERT_EQ(std::string("connectionError"),
            TurboHttpExecutor::ClassifyError(
                ctx, ERROR_WINHTTP_CONNECTION_ERROR,
                RequestPhase::kReceiving));
  ASSERT_EQ(std::string("connectionError"),
            TurboHttpExecutor::ClassifyError(ctx, ERROR_WINHTTP_INVALID_URL,
                                             RequestPhase::kConnecting));
  ASSERT_EQ(std::string("connectionError"),
            TurboHttpExecutor::ClassifyError(
                ctx, ERROR_WINHTTP_UNRECOGNIZED_SCHEME,
                RequestPhase::kConnecting));
  ASSERT_EQ(std::string("connectionError"),
            TurboHttpExecutor::ClassifyError(
                ctx, ERROR_WINHTTP_REDIRECT_FAILED, RequestPhase::kReceiving));

  // Unknown error
  ASSERT_EQ(std::string("unknown"),
            TurboHttpExecutor::ClassifyError(ctx, ERROR_INTERNAL_ERROR,
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
    TurboHttpExecutor_ParseRawHeaders_NormalizesToLowercaseAndAggregatesMultiValues) {
  const std::wstring raw_headers =
      L"HTTP/2 201 Created\r\n"
      L"Content-Type: application/json\r\n"
      L"X-Custom: v1\r\n"
      L"x-custom: v2\r\n"
      L"Content-Length: 19\r\n\r\n";

  std::optional<int64_t> content_length;
  flutter::EncodableMap parsed =
      TurboHttpExecutor::ParseRawHeaders(raw_headers, &content_length);

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
    TurboHttpExecutor_ParseRawHeaders_MalformedLinesAndInvalidContentLength_HandlesSafely) {
  const std::wstring raw_headers =
      L"HTTP/2 200 OK\r\n"
      L": missing-key\r\n"
      L"MalformedHeaderWithoutColon\r\n"
      L"   \r\n"
      L"Content-Length: not-a-number\r\n"
      L"X-Valid: ok\r\n\r\n";

  std::optional<int64_t> content_length;
  flutter::EncodableMap parsed =
      TurboHttpExecutor::ParseRawHeaders(raw_headers, &content_length);

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

TURBO_TEST(
    TurboHttpExecutor_ValidateAndAssembleChunks_EnforcesMaxResponseBytes) {
  const std::vector<std::vector<uint8_t>> chunks = {
      std::vector<uint8_t>(32, 1),
      std::vector<uint8_t>(32, 2),
  };

  // 1. Exceeded by Content-Length header prior to reading chunks
  {
    std::vector<uint8_t> out_body;
    std::string err_code;
    std::string err_msg;
    bool ok = TurboHttpExecutor::ValidateAndAssembleChunks(
        /*content_length=*/100, /*max_response_bytes=*/50, chunks, &out_body,
        &err_code, &err_msg);
    ASSERT_FALSE(ok);
    ASSERT_EQ(std::string("maxResponseBytesExceeded"), err_code);
    ASSERT_TRUE(out_body.empty());
  }

  // 2. Exceeded during chunk streaming when Content-Length is unknown
  {
    std::vector<uint8_t> out_body;
    std::string err_code;
    std::string err_msg;
    bool ok = TurboHttpExecutor::ValidateAndAssembleChunks(
        /*content_length=*/std::nullopt, /*max_response_bytes=*/48, chunks,
        &out_body, &err_code, &err_msg);
    ASSERT_FALSE(ok);
    ASSERT_EQ(std::string("maxResponseBytesExceeded"), err_code);
  }

  // 3. Exact boundary (64 bytes == maxResponseBytes 64) succeeds
  {
    std::vector<uint8_t> out_body;
    std::string err_code;
    std::string err_msg;
    bool ok = TurboHttpExecutor::ValidateAndAssembleChunks(
        /*content_length=*/64, /*max_response_bytes=*/64, chunks, &out_body,
        &err_code, &err_msg);
    ASSERT_TRUE(ok);
    ASSERT_EQ(static_cast<size_t>(64), out_body.size());
  }

  // 4. Unlimited (max_response_bytes == nullopt) succeeds
  {
    std::vector<uint8_t> out_body;
    std::string err_code;
    std::string err_msg;
    bool ok = TurboHttpExecutor::ValidateAndAssembleChunks(
        /*content_length=*/64, /*max_response_bytes=*/std::nullopt, chunks,
        &out_body, &err_code, &err_msg);
    ASSERT_TRUE(ok);
    ASSERT_EQ(static_cast<size_t>(64), out_body.size());
  }

  // 5. Empty body (chunks empty) succeeds
  {
    std::vector<uint8_t> out_body;
    std::string err_code;
    std::string err_msg;
    const std::vector<std::vector<uint8_t>> empty_chunks;
    bool ok = TurboHttpExecutor::ValidateAndAssembleChunks(
        /*content_length=*/0, /*max_response_bytes=*/32, empty_chunks,
        &out_body, &err_code, &err_msg);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(out_body.empty());
  }

  // 6. INT64_MAX boundary succeeds without overflow
  {
    std::vector<uint8_t> out_body;
    std::string err_code;
    std::string err_msg;
    bool ok = TurboHttpExecutor::ValidateAndAssembleChunks(
        /*content_length=*/64, /*max_response_bytes=*/INT64_MAX, chunks,
        &out_body, &err_code, &err_msg);
    ASSERT_TRUE(ok);
    ASSERT_EQ(static_cast<size_t>(64), out_body.size());
  }
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

}  // namespace test
}  // namespace flutter_turbo_fetch

