#include "turbo_http_executor.h"

#include <curl/curl.h>

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
  auto [code1, msg1] =
      TurboHttpExecutor::ClassifyError(CURLE_ABORTED_BY_CALLBACK, true, ExecutionPhase::kReceiving, nullptr);
  ASSERT_EQ(TurboErrorCodes::kCancel, code1);

  // Even if CURLE_OK is passed, if is_cancelled is true, it classifies as cancel
  auto [code2, msg2] =
      TurboHttpExecutor::ClassifyError(CURLE_OK, true, ExecutionPhase::kReceiving, nullptr);
  ASSERT_EQ(TurboErrorCodes::kCancel, code2);
}

TURBO_TEST(TurboHttpExecutor_ClassifyError_ClassifiesTimeoutsCorrectly) {
  // Pre-set timed_out_error_code takes top priority over CURLE_ABORTED_BY_CALLBACK
  auto [code_pre, msg_pre] = TurboHttpExecutor::ClassifyError(
      CURLE_ABORTED_BY_CALLBACK, false, ExecutionPhase::kReceiving,
      TurboErrorCodes::kReceiveTimeout);
  ASSERT_EQ(TurboErrorCodes::kReceiveTimeout, code_pre);

  // Timeout in connecting phase -> connectionTimeout
  auto [code_conn, msg_conn] = TurboHttpExecutor::ClassifyError(
      CURLE_OPERATION_TIMEDOUT, false, ExecutionPhase::kConnecting, nullptr);
  ASSERT_EQ(TurboErrorCodes::kConnectionTimeout, code_conn);

  // Timeout in sending body phase -> sendTimeout
  auto [code_send, msg_send] = TurboHttpExecutor::ClassifyError(
      CURLE_OPERATION_TIMEDOUT, false, ExecutionPhase::kSendingBody, nullptr);
  ASSERT_EQ(TurboErrorCodes::kSendTimeout, code_send);

  // Timeout in receiving response phase -> receiveTimeout
  auto [code_recv, msg_recv] = TurboHttpExecutor::ClassifyError(
      CURLE_OPERATION_TIMEDOUT, false, ExecutionPhase::kReceiving, nullptr);
  ASSERT_EQ(TurboErrorCodes::kReceiveTimeout, code_recv);
}

TURBO_TEST(TurboHttpExecutor_ClassifyError_ClassifiesCertificateAndConnectionErrors) {
  // SSL / Certificate verification errors
  auto [code_cert1, msg_cert1] = TurboHttpExecutor::ClassifyError(
      CURLE_PEER_FAILED_VERIFICATION, false, ExecutionPhase::kConnecting, nullptr);
  ASSERT_EQ(TurboErrorCodes::kBadCertificate, code_cert1);

  auto [code_cert2, msg_cert2] = TurboHttpExecutor::ClassifyError(
      CURLE_SSL_CONNECT_ERROR, false, ExecutionPhase::kConnecting, nullptr);
  ASSERT_EQ(TurboErrorCodes::kBadCertificate, code_cert2);

  // Connection and protocol errors
  auto [code_host, msg_host] = TurboHttpExecutor::ClassifyError(
      CURLE_COULDNT_RESOLVE_HOST, false, ExecutionPhase::kConnecting, nullptr);
  ASSERT_EQ(TurboErrorCodes::kConnectionError, code_host);

  auto [code_conn, msg_conn] = TurboHttpExecutor::ClassifyError(
      CURLE_COULDNT_CONNECT, false, ExecutionPhase::kConnecting, nullptr);
  ASSERT_EQ(TurboErrorCodes::kConnectionError, code_conn);

  auto [code_http2, msg_http2] = TurboHttpExecutor::ClassifyError(
      CURLE_HTTP2, false, ExecutionPhase::kConnecting, nullptr);
  ASSERT_EQ(TurboErrorCodes::kConnectionError, code_http2);

  auto [code_url, msg_url] = TurboHttpExecutor::ClassifyError(
      CURLE_URL_MALFORMAT, false, ExecutionPhase::kConnecting, nullptr);
  ASSERT_EQ(TurboErrorCodes::kConnectionError, code_url);

  // Unknown / unclassified error
  auto [code_unk, msg_unk] = TurboHttpExecutor::ClassifyError(
      CURLE_FAILED_INIT, false, ExecutionPhase::kConnecting, nullptr);
  ASSERT_EQ(TurboErrorCodes::kUnknown, code_unk);
}

TURBO_TEST(TurboHttpExecutor_Execute_InvalidUrl_ReturnsConnectionError) {
  TurboSessionManager session_manager;
  TurboHttpExecutor executor(session_manager);

  auto request = CreateRequest("req-invalid-url", "htp://invalid url scheme", "GET");
  auto response = ExecuteSync(executor, request);

  ASSERT_NE(nullptr, response.status_code());
  ASSERT_EQ(static_cast<int64_t>(0), *response.status_code());
  ASSERT_TRUE(response.error_code() != nullptr);
  ASSERT_EQ(TurboErrorCodes::kConnectionError, *response.error_code());
}

TURBO_TEST(TurboHttpExecutor_Execute_InvalidHeaderCharacters_ReturnsConnectionErrorSafelyWithoutCrash) {
  TurboSessionManager session_manager;
  TurboHttpExecutor executor(session_manager);

  flutter::EncodableMap bad_headers;
  bad_headers[flutter::EncodableValue("Bad\r\nHeader")] =
      flutter::EncodableValue("Value");

  auto request = CreateRequest("req-bad-header", "https://example.com", "GET", bad_headers);
  auto response = ExecuteSync(executor, request);

  ASSERT_NE(nullptr, response.status_code());
  ASSERT_EQ(static_cast<int64_t>(0), *response.status_code());
  ASSERT_TRUE(response.error_code() != nullptr);
  ASSERT_EQ(TurboErrorCodes::kConnectionError, *response.error_code());
}

TURBO_TEST(TurboHttpExecutor_ParseRawHeaders_NormalizesToLowercaseAndAggregatesMultiValues) {
  std::string raw_headers =
      "HTTP/2 200\r\n"
      "Content-Type: application/json; charset=utf-8\r\n"
      "Set-Cookie: session=abc; Path=/\r\n"
      "Set-Cookie: token=xyz; Secure\r\n"
      "x-custom-id: 12345\r\n"
      "\r\n";

  auto [headers_map, content_length] =
      TurboHttpExecutor::ParseRawHeaders(raw_headers);

  // Lowercase keys
  ASSERT_TRUE(headers_map.find(flutter::EncodableValue("content-type")) != headers_map.end());
  ASSERT_TRUE(headers_map.find(flutter::EncodableValue("set-cookie")) != headers_map.end());
  ASSERT_TRUE(headers_map.find(flutter::EncodableValue("x-custom-id")) != headers_map.end());

  // Multi-value aggregation for set-cookie
  const auto& cookies_val = headers_map[flutter::EncodableValue("set-cookie")];
  const auto& cookies_list = std::get<flutter::EncodableList>(cookies_val);
  ASSERT_EQ(static_cast<size_t>(2), cookies_list.size());
  ASSERT_EQ("session=abc; Path=/", std::get<std::string>(cookies_list[0]));
  ASSERT_EQ("token=xyz; Secure", std::get<std::string>(cookies_list[1]));
}

TURBO_TEST(TurboHttpExecutor_ParseRawHeaders_MalformedLinesAndInvalidContentLength_HandlesSafely) {
  std::string raw_headers =
      "Malformed line without colon\r\n"
      ": leading colon\r\n"
      "Content-Length: not-a-number\r\n"
      "Valid-Header: test\r\n";

  auto [headers_map, content_length] =
      TurboHttpExecutor::ParseRawHeaders(raw_headers);

  ASSERT_FALSE(content_length.has_value());
  ASSERT_TRUE(headers_map.find(flutter::EncodableValue("valid-header")) != headers_map.end());
}

TURBO_TEST(TurboHttpExecutor_Execute_MaxResponseBytesNull_ReturnsFullBodyAndHeaders) {
  TurboSessionManager session_manager;
  TurboHttpExecutor executor(session_manager);

  std::string header_block = "HTTP/2 200\r\nContent-Length: 1000\r\n\r\n";
  std::vector<uint8_t> body_data(1000, 0xAB);

  auto res = TurboHttpExecutor::ValidateAndAssembleChunks(
      200, "OK", header_block, body_data, std::nullopt);

  ASSERT_TRUE(res.error_code() == nullptr);
  ASSERT_NE(nullptr, res.status_code());
  ASSERT_EQ(static_cast<int64_t>(200), *res.status_code());
  ASSERT_TRUE(res.body() != nullptr);
  ASSERT_EQ(static_cast<size_t>(1000), res.body()->size());
}

TURBO_TEST(TurboHttpExecutor_ValidateAndAssembleChunks_EnforcesMaxResponseBytes) {
  // 1. Content-Length header exceeds limit
  std::string header_large = "HTTP/2 200\r\nContent-Length: 2048\r\n\r\n";
  std::vector<uint8_t> empty_body;
  auto res1 = TurboHttpExecutor::ValidateAndAssembleChunks(
      200, "OK", header_large, empty_body, 1024);
  ASSERT_TRUE(res1.error_code() != nullptr);
  ASSERT_EQ(TurboErrorCodes::kMaxResponseBytesExceeded, *res1.error_code());

  // 2. Body data exceeds limit
  std::string header_no_len = "HTTP/2 200\r\n\r\n";
  std::vector<uint8_t> large_body(2048, 0xFF);
  auto res2 = TurboHttpExecutor::ValidateAndAssembleChunks(
      200, "OK", header_no_len, large_body, 1024);
  ASSERT_TRUE(res2.error_code() != nullptr);
  ASSERT_EQ(TurboErrorCodes::kMaxResponseBytesExceeded, *res2.error_code());
}

TURBO_TEST(TurboHttpExecutor_Cancel_CancelsInFlightRequest) {
  TurboSessionManager session_manager;
  TurboHttpExecutor executor(session_manager);

  auto request = CreateRequest("req-cancel", "https://example.com/sleep", "GET");

  std::mutex mtx;
  std::condition_variable cv;
  std::optional<TurboHttpResponse> result_opt;

  executor.Execute(request, [&](TurboHttpResponse response) {
    std::lock_guard<std::mutex> lock(mtx);
    result_opt = std::move(response);
    cv.notify_one();
  });

  // Ensure request is registered synchronously before calling cancel
  ASSERT_TRUE(executor.has_active_request("req-cancel"));
  executor.CancelRequest("req-cancel");

  std::unique_lock<std::mutex> lock(mtx);
  bool finished = cv.wait_for(lock, std::chrono::seconds(5),
                              [&] { return result_opt.has_value(); });
  ASSERT_TRUE(finished);
  ASSERT_TRUE(result_opt->error_code() != nullptr);
  ASSERT_EQ(TurboErrorCodes::kCancel, *result_opt->error_code());
}

TURBO_TEST(TurboHttpExecutor_Cancel_ImmediatelyAfterExecute_CancelsSafelyAndInvokesCallbackExactlyOnce) {
  TurboSessionManager session_manager;
  TurboHttpExecutor executor(session_manager);

  std::atomic<int> callback_count{0};
  std::mutex mtx;
  std::condition_variable cv;
  bool done = false;

  auto request = CreateRequest("req-race", "https://example.com/api", "GET");

  executor.Execute(request, [&](TurboHttpResponse response) {
    callback_count.fetch_add(1);
    std::lock_guard<std::mutex> lock(mtx);
    done = true;
    cv.notify_one();
  });

  // Cancel immediately to test race condition
  executor.CancelRequest("req-race");

  std::unique_lock<std::mutex> lock(mtx);
  cv.wait_for(lock, std::chrono::seconds(5), [&] { return done; });

  ASSERT_EQ(1, callback_count.load());
  ASSERT_EQ(static_cast<size_t>(0), executor.active_request_count());
}

TURBO_TEST(TurboHttpExecutor_Shutdown_CancelsInFlightRequestsAndCleansUpActiveRequests) {
  TurboSessionManager session_manager;
  TurboHttpExecutor executor(session_manager);

  std::atomic<int> callback_count{0};
  std::mutex mtx;
  std::condition_variable cv;

  auto req1 = CreateRequest("req-shutdown-1", "https://example.com/1", "GET");
  auto req2 = CreateRequest("req-shutdown-2", "https://example.com/2", "GET");

  executor.Execute(req1, [&](TurboHttpResponse) {
    callback_count.fetch_add(1);
    cv.notify_one();
  });
  executor.Execute(req2, [&](TurboHttpResponse) {
    callback_count.fetch_add(1);
    cv.notify_one();
  });

  ASSERT_EQ(static_cast<size_t>(2), executor.active_request_count());

  executor.Shutdown();

  std::unique_lock<std::mutex> lock(mtx);
  cv.wait_for(lock, std::chrono::seconds(5), [&] { return callback_count.load() == 2; });

  ASSERT_EQ(2, callback_count.load());
  ASSERT_EQ(static_cast<size_t>(0), executor.active_request_count());
}

TURBO_TEST(TurboHttpExecutor_Execute_PostWithRequestBody_TransmitsPayloadCorrectly) {
  TurboSessionManager session_manager;
  TurboHttpExecutor executor(session_manager);

  std::vector<uint8_t> body = {'h', 'e', 'l', 'l', 'o'};
  auto request = CreateRequest("req-post", "https://example.com/post", "POST", {}, &body);

  ASSERT_TRUE(request.body() != nullptr);
  ASSERT_EQ(static_cast<size_t>(5), request.body()->size());
}

TURBO_TEST(TurboHttpExecutor_Execute_PostWithNullBody_HandlesSafelyWithoutCrash) {
  TurboSessionManager session_manager;
  TurboHttpExecutor executor(session_manager);

  auto request = CreateRequest("req-post-null", "https://example.com/post", "POST", {}, nullptr);
  ASSERT_TRUE(request.body() == nullptr);
}

TURBO_TEST(TurboHttpExecutor_Execute_EmptyResponseBody_ReturnsEmptyBodySafely) {
  std::string headers = "HTTP/2 204 No Content\r\n\r\n";
  std::vector<uint8_t> empty_body;

  auto res = TurboHttpExecutor::ValidateAndAssembleChunks(
      204, "No Content", headers, empty_body, std::nullopt);

  ASSERT_NE(nullptr, res.status_code());
  ASSERT_EQ(static_cast<int64_t>(204), *res.status_code());
  ASSERT_TRUE(res.body() != nullptr);
  ASSERT_EQ(static_cast<size_t>(0), res.body()->size());
}

}  // namespace test
}  // namespace flutter_turbo_fetch

