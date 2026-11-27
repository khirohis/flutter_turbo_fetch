#include "curl_helpers.h"
#include "curl_transport.h"
#include "turbo_http_executor.h"

#include <curl/curl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "test/test_assertions.h"

namespace flutter_turbo_fetch {
namespace test {

namespace {

TurboHttpRequest CreateRequest(
    const std::string& request_id = "test-req",
    const std::string& url = "https://example.com/api",
    const std::string& method = "GET",
    const std::unordered_map<std::string, std::string>& headers = {},
    const std::vector<uint8_t>* body = nullptr,
    std::optional<int64_t> max_response_bytes = std::nullopt,
    bool follow_redirects = true,
    bool allow_bad_certificates = false) {
  const int64_t timeout_ms = 5000;
  return TurboHttpRequest(
      request_id, url, method, headers, body, &timeout_ms, &timeout_ms,
      &timeout_ms, follow_redirects, allow_bad_certificates,
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

TurboHttpResponse MakeOkResponse(int64_t status = 200,
                                 std::string message = "OK",
                                 std::vector<uint8_t> body = {}) {
  return TurboHttpResponse(status, std::move(message), {}, std::move(body));
}

}  // namespace

// =============================================================================
// 1. CurlHelpers: Error Classification
// =============================================================================

TURBO_TEST(CurlHelpers_ClassifyCurlError_ClassifiesCanceledRequest) {
  auto [code1, msg1] = ClassifyCurlError(
      CURLE_ABORTED_BY_CALLBACK, true, ExecutionPhase::kReceiving, nullptr);
  ASSERT_EQ(std::string(TurboErrorCodes::kCancel), code1);

  // Even with CURLE_OK, is_cancelled == true must classify as cancel.
  auto [code2, msg2] = ClassifyCurlError(
      CURLE_OK, true, ExecutionPhase::kReceiving, nullptr);
  ASSERT_EQ(std::string(TurboErrorCodes::kCancel), code2);

  // CURLE_ABORTED_BY_CALLBACK without timeout also classifies as cancel.
  auto [code3, msg3] = ClassifyCurlError(
      CURLE_ABORTED_BY_CALLBACK, false, ExecutionPhase::kConnecting, nullptr);
  ASSERT_EQ(std::string(TurboErrorCodes::kCancel), code3);
}

TURBO_TEST(CurlHelpers_ClassifyCurlError_ClassifiesTimeoutsCorrectly) {
  // Pre-set timed_out_error_code takes priority over cancel/CURLE_ABORTED_BY_CALLBACK.
  auto [code_pre, msg_pre] = ClassifyCurlError(
      CURLE_ABORTED_BY_CALLBACK, true, ExecutionPhase::kReceiving,
      TurboErrorCodes::kReceiveTimeout);
  ASSERT_EQ(std::string(TurboErrorCodes::kReceiveTimeout), code_pre);

  auto [code_conn, msg_conn] = ClassifyCurlError(
      CURLE_OPERATION_TIMEDOUT, false, ExecutionPhase::kConnecting, nullptr);
  ASSERT_EQ(std::string(TurboErrorCodes::kConnectionTimeout), code_conn);

  auto [code_send, msg_send] = ClassifyCurlError(
      CURLE_OPERATION_TIMEDOUT, false, ExecutionPhase::kSendingBody, nullptr);
  ASSERT_EQ(std::string(TurboErrorCodes::kSendTimeout), code_send);

  auto [code_recv, msg_recv] = ClassifyCurlError(
      CURLE_OPERATION_TIMEDOUT, false, ExecutionPhase::kReceiving, nullptr);
  ASSERT_EQ(std::string(TurboErrorCodes::kReceiveTimeout), code_recv);
}

TURBO_TEST(CurlHelpers_ClassifyCurlError_ClassifiesCertificateAndConnectionErrors) {
  // SSL / certificate errors
  const CURLcode cert_codes[] = {
      CURLE_PEER_FAILED_VERIFICATION,
      CURLE_SSL_CONNECT_ERROR,
      CURLE_SSL_CERTPROBLEM,
      CURLE_SSL_CIPHER,
      CURLE_SSL_CACERT_BADFILE,
      CURLE_SSL_SHUTDOWN_FAILED,
      CURLE_SSL_CRL_BADFILE,
      CURLE_SSL_ISSUER_ERROR,
  };
  for (CURLcode c : cert_codes) {
    auto [code, msg] =
        ClassifyCurlError(c, false, ExecutionPhase::kConnecting, nullptr);
    ASSERT_EQ(std::string(TurboErrorCodes::kBadCertificate), code);
  }

  // Connection / protocol / URL / redirect errors
  const CURLcode conn_codes[] = {
      CURLE_COULDNT_RESOLVE_HOST,
      CURLE_COULDNT_CONNECT,
      CURLE_HTTP2,
      CURLE_HTTP2_STREAM,
      CURLE_UNSUPPORTED_PROTOCOL,
      CURLE_TOO_MANY_REDIRECTS,
      CURLE_URL_MALFORMAT,
  };
  for (CURLcode c : conn_codes) {
    auto [code, msg] =
        ClassifyCurlError(c, false, ExecutionPhase::kConnecting, nullptr);
    ASSERT_EQ(std::string(TurboErrorCodes::kConnectionError), code);
  }

  // Unclassified error -> unknown
  auto [code_unk, msg_unk] = ClassifyCurlError(
      CURLE_FAILED_INIT, false, ExecutionPhase::kConnecting, nullptr);
  ASSERT_EQ(std::string(TurboErrorCodes::kUnknown), code_unk);
}

// =============================================================================
// 2. CurlHelpers: Request Validation & Normalization
// =============================================================================

TURBO_TEST(CurlHelpers_ValidateCurlRequest_NormalizesValidRequest) {
  const std::vector<uint8_t> payload = {'h', 'i'};
  const std::unordered_map<std::string, std::string> headers = {
      {"X-Token", "abc123"},
      {"Accept-Encoding", "identity"},
      {"Expect", "100-continue"},
  };
  const int64_t connect_ms = 1000;
  const int64_t send_ms = 2000;
  const int64_t recv_ms = 3000;
  const int64_t max_bytes = 4096;

  TurboHttpRequest request("req-valid", "HTTPS://example.com:8443/v1?k=v",
                           "POST", headers, &payload, &connect_ms, &recv_ms,
                           &send_ms, false, true, &max_bytes);

  ValidatedCurlRequest validated;
  std::string error_message;
  ASSERT_TRUE(ValidateCurlRequest(request, &validated, &error_message));
  ASSERT_EQ(std::string("req-valid"), validated.request_id);
  ASSERT_EQ(std::string("HTTPS://example.com:8443/v1?k=v"), validated.url);
  ASSERT_EQ(std::string("POST"), validated.method);
  ASSERT_EQ(static_cast<size_t>(3), validated.headers.size());
  ASSERT_TRUE(validated.has_accept_encoding_header);
  ASSERT_TRUE(validated.has_expect_header);
  ASSERT_TRUE(validated.body.has_value());
  ASSERT_EQ(payload, *validated.body);
  ASSERT_TRUE(validated.connect_timeout_ms.has_value());
  ASSERT_EQ(static_cast<int64_t>(1000), *validated.connect_timeout_ms);
  ASSERT_TRUE(validated.send_timeout_ms.has_value());
  ASSERT_EQ(static_cast<int64_t>(2000), *validated.send_timeout_ms);
  ASSERT_TRUE(validated.receive_timeout_ms.has_value());
  ASSERT_EQ(static_cast<int64_t>(3000), *validated.receive_timeout_ms);
  ASSERT_TRUE(validated.max_response_bytes.has_value());
  ASSERT_EQ(static_cast<int64_t>(4096), *validated.max_response_bytes);
  ASSERT_FALSE(validated.follow_redirects);
  ASSERT_TRUE(validated.allow_bad_certificates);
}

TURBO_TEST(CurlHelpers_ValidateCurlRequest_NormalizesMaxResponseBytesAndTimeouts) {
  // Negative maxResponseBytes is clamped to 0, and 0 is preserved (not treated as unlimited).
  {
    ValidatedCurlRequest validated;
    std::string error_message;
    ASSERT_TRUE(ValidateCurlRequest(
        CreateRequest("req-neg-max", "https://example.com/", "GET", {}, nullptr, -10),
        &validated, &error_message));
    ASSERT_TRUE(validated.max_response_bytes.has_value());
    ASSERT_EQ(static_cast<int64_t>(0), *validated.max_response_bytes);
  }
  {
    ValidatedCurlRequest validated;
    std::string error_message;
    ASSERT_TRUE(ValidateCurlRequest(
        CreateRequest("req-zero-max", "https://example.com/", "GET", {}, nullptr, 0),
        &validated, &error_message));
    ASSERT_TRUE(validated.max_response_bytes.has_value());
    ASSERT_EQ(static_cast<int64_t>(0), *validated.max_response_bytes);
  }

  // ClampCurlTimeoutMs boundary values
  ASSERT_EQ(0L, ClampCurlTimeoutMs(-100));
  ASSERT_EQ(0L, ClampCurlTimeoutMs(0));
  ASSERT_EQ(1500L, ClampCurlTimeoutMs(1500));
  ASSERT_EQ(LONG_MAX, ClampCurlTimeoutMs(std::numeric_limits<int64_t>::max()));
}

TURBO_TEST(CurlHelpers_ValidateCurlRequest_RejectsInvalidMethodUrlAndHeaders) {
  ValidatedCurlRequest validated;
  std::string error_message;

  // Invalid HTTP methods
  ASSERT_FALSE(ValidateCurlRequest(
      CreateRequest("m1", "https://example.com/", ""), &validated, &error_message));
  ASSERT_FALSE(ValidateCurlRequest(
      CreateRequest("m2", "https://example.com/", "GET /"), &validated, &error_message));
  ASSERT_FALSE(ValidateCurlRequest(
      CreateRequest("m3", "https://example.com/", "GET\r\nX"), &validated, &error_message));
  ASSERT_FALSE(ValidateCurlRequest(
      CreateRequest("m4", "https://example.com/", "G\xC3\xA9T"), &validated, &error_message));

  // Valid custom RFC token method
  ASSERT_TRUE(ValidateCurlRequest(
      CreateRequest("m5", "https://example.com/", "M-SEARCH"), &validated, &error_message));
  ASSERT_EQ(std::string("M-SEARCH"), validated.method);

  // Invalid URLs (empty, unsupported scheme, missing host, control chars / NUL)
  ASSERT_FALSE(ValidateCurlRequest(
      CreateRequest("u1", ""), &validated, &error_message));
  ASSERT_FALSE(ValidateCurlRequest(
      CreateRequest("u2", "ftp://example.com/file"), &validated, &error_message));
  ASSERT_FALSE(ValidateCurlRequest(
      CreateRequest("u3", "https://"), &validated, &error_message));
  ASSERT_FALSE(ValidateCurlRequest(
      CreateRequest("u4", "http:///only-path"), &validated, &error_message));
  ASSERT_FALSE(ValidateCurlRequest(
      CreateRequest("u5", "https://example.com/a\r\nb"), &validated, &error_message));
  const std::string nul_url("https://example.com/a\0b", 23);
  ASSERT_FALSE(ValidateCurlRequest(
      CreateRequest("u6", nul_url), &validated, &error_message));

  // Invalid header names (empty, whitespace, colon, CRLF, non-ASCII)
  ASSERT_FALSE(ValidateCurlRequest(
      CreateRequest("h1", "https://example.com/", "GET", {{"", "v"}}),
      &validated, &error_message));
  ASSERT_FALSE(ValidateCurlRequest(
      CreateRequest("h2", "https://example.com/", "GET", {{"Bad Header", "v"}}),
      &validated, &error_message));
  ASSERT_FALSE(ValidateCurlRequest(
      CreateRequest("h3", "https://example.com/", "GET", {{"Bad:Header", "v"}}),
      &validated, &error_message));
  ASSERT_FALSE(ValidateCurlRequest(
      CreateRequest("h4", "https://example.com/", "GET", {{"Bad\r\nHeader", "v"}}),
      &validated, &error_message));
  ASSERT_FALSE(ValidateCurlRequest(
      CreateRequest("h5", "https://example.com/", "GET", {{"X-\xC3\xA9", "v"}}),
      &validated, &error_message));

  // Invalid header values (CR, LF, NUL, CRLF after non-ASCII)
  ASSERT_FALSE(ValidateCurlRequest(
      CreateRequest("hv1", "https://example.com/", "GET", {{"X-Val", "a\r\nb"}}),
      &validated, &error_message));
  const std::string nul_val("a\0b", 3);
  ASSERT_FALSE(ValidateCurlRequest(
      CreateRequest("hv2", "https://example.com/", "GET", {{"X-Val", nul_val}}),
      &validated, &error_message));
  ASSERT_FALSE(ValidateCurlRequest(
      CreateRequest("hv3", "https://example.com/", "GET",
                    {{"X-Label", "caf\xC3\xA9\r\nInjected: 1"}}),
      &validated, &error_message));

  // Valid UTF-8 header value & non-ASCII URL path
  ASSERT_TRUE(ValidateCurlRequest(
      CreateRequest("hv-ok",
                    "https://example.com/search?q=\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88",
                    "GET",
                    {{"X-Label", "caf\xC3\xA9-\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88"}}),
      &validated, &error_message));
  ASSERT_FALSE(validated.has_accept_encoding_header);
  ASSERT_FALSE(validated.has_expect_header);
}

// =============================================================================
// 3. CurlHelpers: Status Line, Content-Length & Raw Header Parsing
// =============================================================================

TURBO_TEST(CurlHelpers_ParseStatusLine_ExtractsCodeAndMessage) {
  ParsedStatusLine status;

  ASSERT_TRUE(ParseStatusLine("HTTP/1.1 200 OK\r\n", &status));
  ASSERT_EQ(200, status.status_code);
  ASSERT_EQ(std::string("OK"), status.status_message);

  ASSERT_TRUE(ParseStatusLine("HTTP/1.1 404 Not Found\r\n", &status));
  ASSERT_EQ(404, status.status_code);
  ASSERT_EQ(std::string("Not Found"), status.status_message);

  ASSERT_TRUE(ParseStatusLine("HTTP/2 204\r\n", &status));
  ASSERT_EQ(204, status.status_code);
  ASSERT_EQ(std::string(""), status.status_message);

  ASSERT_FALSE(ParseStatusLine("Content-Type: text/plain\r\n", &status));
  ASSERT_FALSE(ParseStatusLine("HTTP/1.1 not-a-code\r\n", &status));
}

TURBO_TEST(CurlHelpers_ParseContentLength_EnforcesStrictNonNegativeInteger) {
  auto zero = ParseContentLength("  0  ");
  ASSERT_TRUE(zero.has_value());
  ASSERT_EQ(static_cast<int64_t>(0), *zero);

  auto valid = ParseContentLength("1048576");
  ASSERT_TRUE(valid.has_value());
  ASSERT_EQ(static_cast<int64_t>(1048576), *valid);

  // Trailing garbage, negative, empty, or overflow must be rejected.
  ASSERT_FALSE(ParseContentLength("123abc").has_value());
  ASSERT_FALSE(ParseContentLength("-5").has_value());
  ASSERT_FALSE(ParseContentLength("").has_value());
  ASSERT_FALSE(ParseContentLength("   ").has_value());
  ASSERT_FALSE(ParseContentLength("999999999999999999999999").has_value());
}

TURBO_TEST(CurlRequestContext_OnHeaderLine_NormalizesAggregatesAndDropsRedirectHeaders) {
  const auto t0 = std::chrono::steady_clock::now();
  ValidatedCurlRequest validated;
  std::string err;
  ASSERT_TRUE(ValidateCurlRequest(
      CreateRequest("hdr-norm", "https://example.com/", "GET", {}, nullptr,
                    /*max_response_bytes=*/std::nullopt,
                    /*follow_redirects=*/true),
      &validated, &err));

  CurlRequestContext ctx;
  ctx.InitializeFromRequest(validated, t0);

  const std::string_view lines[] = {
      "HTTP/1.1 302 Found\r\n",
      "Location: https://example.com/final\r\n",
      "Set-Cookie: redirect_only=1\r\n",
      "Content-Length: 99999\r\n",
      "\r\n",
      "HTTP/2 200\r\n",
      "  Content-Type : application/json; charset=utf-8  \r\n",
      "Set-Cookie: session=abc; Path=/\r\n",
      "set-cookie: token=xyz; Secure\r\n",
      "X-Empty:   \r\n",
      "Malformed line without colon\r\n",
      ": leading colon\r\n",
      "Content-Length: 42\r\n",
      "X-Final: tail-without-crlf",
  };
  for (std::string_view line : lines) {
    ASSERT_TRUE(ctx.OnHeaderLine(line, t0));
  }

  // Final status message comes from HTTP/2 200 (empty), not 302 Found.
  ASSERT_EQ(std::string(""), ctx.status_message);

  // Redirect headers must be discarded.
  ASSERT_TRUE(ctx.response_headers.find("location") ==
              ctx.response_headers.end());

  // Final Content-Length overrides redirect Content-Length.
  ASSERT_TRUE(ctx.content_length.has_value());
  ASSERT_EQ(static_cast<int64_t>(42), *ctx.content_length);

  // Lowercase keys & multi-value aggregation
  ASSERT_EQ(std::string("application/json; charset=utf-8"),
            ctx.response_headers["content-type"][0]);
  ASSERT_EQ(static_cast<size_t>(2), ctx.response_headers["set-cookie"].size());
  ASSERT_EQ(std::string("session=abc; Path=/"),
            ctx.response_headers["set-cookie"][0]);
  ASSERT_EQ(std::string("token=xyz; Secure"),
            ctx.response_headers["set-cookie"][1]);
  ASSERT_EQ(std::string(""), ctx.response_headers["x-empty"][0]);
  ASSERT_EQ(std::string("tail-without-crlf"),
            ctx.response_headers["x-final"][0]);
}

// =============================================================================
// 4. CurlRequestContext: Redirect Reset, MaxBytes, Phase Timeout & Final Response
// =============================================================================

TURBO_TEST(CurlRequestContext_RedirectResetsHeadersAndSkipsIntermediateContentLengthLimit) {
  ValidatedCurlRequest validated;
  std::string err;
  ASSERT_TRUE(ValidateCurlRequest(
      CreateRequest("req-redir", "https://example.com/start", "GET", {}, nullptr,
                    /*max_response_bytes=*/64, /*follow_redirects=*/true),
      &validated, &err));

  const auto t0 = std::chrono::steady_clock::now();
  CurlRequestContext ctx;
  ctx.InitializeFromRequest(validated, t0);

  // Intermediate 302 with Content-Length larger than max_response_bytes must NOT abort.
  ASSERT_TRUE(ctx.OnHeaderLine("HTTP/1.1 302 Found\r\n", t0));
  ASSERT_TRUE(ctx.OnHeaderLine("Location: https://example.com/final\r\n", t0));
  ASSERT_TRUE(ctx.OnHeaderLine("Content-Length: 4096\r\n", t0));
  ASSERT_TRUE(ctx.OnHeaderLine("\r\n", t0));
  ASSERT_FALSE(ctx.is_exceeded_limit.load());

  // Final 200 response resets headers and status_message.
  ASSERT_TRUE(ctx.OnHeaderLine("HTTP/1.1 200 OK\r\n", t0));
  ASSERT_TRUE(ctx.OnHeaderLine("X-Final: yes\r\n", t0));
  ASSERT_TRUE(ctx.OnHeaderLine("Content-Length: 5\r\n", t0));
  ASSERT_TRUE(ctx.OnHeaderLine("\r\n", t0));

  const std::vector<uint8_t> body = {'h', 'e', 'l', 'l', 'o'};
  ASSERT_TRUE(ctx.OnBodyChunk(body.data(), body.size(), t0));

  TurboHttpResponse res =
      CurlMultiTransport::BuildFinalResponse(200, CURLE_OK, ctx);
  ASSERT_TRUE(res.error_code() == nullptr);
  ASSERT_EQ(static_cast<int64_t>(200), *res.status_code());
  ASSERT_EQ(std::string("OK"), *res.status_message());
  ASSERT_TRUE(res.headers().find("location") == res.headers().end());
  ASSERT_EQ(std::string("yes"), res.headers().at("x-final")[0]);
  ASSERT_EQ(body, *res.body());
}

TURBO_TEST(CurlRequestContext_HeadRequest_DoesNotAbortOnContentLengthExceedingLimit) {
  ValidatedCurlRequest validated;
  std::string err;
  ASSERT_TRUE(ValidateCurlRequest(
      CreateRequest("req-head", "https://example.com/file", "HEAD", {}, nullptr,
                    /*max_response_bytes=*/64),
      &validated, &err));

  const auto t0 = std::chrono::steady_clock::now();
  CurlRequestContext ctx;
  ctx.InitializeFromRequest(validated, t0);

  // HEAD response has no body; Content-Length advertises resource size only.
  ASSERT_TRUE(ctx.OnHeaderLine("HTTP/1.1 200 OK\r\n", t0));
  ASSERT_TRUE(ctx.OnHeaderLine("Content-Length: 100000\r\n", t0));
  ASSERT_TRUE(ctx.OnHeaderLine("\r\n", t0));
  ASSERT_FALSE(ctx.is_exceeded_limit.load());

  TurboHttpResponse res =
      CurlMultiTransport::BuildFinalResponse(200, CURLE_OK, ctx);
  ASSERT_TRUE(res.error_code() == nullptr);
  ASSERT_EQ(static_cast<int64_t>(200), *res.status_code());
  ASSERT_TRUE(res.body()->empty());
}

TURBO_TEST(CurlRequestContext_MaxResponseBytes_TwoStageAndZeroBoundary) {
  const auto t0 = std::chrono::steady_clock::now();

  // 1. Content-Length header exceeds limit -> immediate abort in OnHeaderLine
  {
    ValidatedCurlRequest validated;
    std::string err;
    ASSERT_TRUE(ValidateCurlRequest(
        CreateRequest("m1", "https://example.com/", "GET", {}, nullptr, 64),
        &validated, &err));
    CurlRequestContext ctx;
    ctx.InitializeFromRequest(validated, t0);

    ASSERT_TRUE(ctx.OnHeaderLine("HTTP/1.1 200 OK\r\n", t0));
    ASSERT_FALSE(ctx.OnHeaderLine("Content-Length: 65\r\n", t0));
    ASSERT_TRUE(ctx.is_exceeded_limit.load());

    TurboHttpResponse res = CurlMultiTransport::BuildFinalResponse(
        200, CURLE_WRITE_ERROR, ctx);
    ASSERT_NE(nullptr, res.error_code());
    ASSERT_EQ(std::string(TurboErrorCodes::kMaxResponseBytesExceeded),
              *res.error_code());
    ASSERT_NE(nullptr, res.error_message());
    ASSERT_TRUE(res.error_message()->find("64") != std::string::npos);
  }

  // 2. Exact boundary succeeds
  {
    ValidatedCurlRequest validated;
    std::string err;
    ASSERT_TRUE(ValidateCurlRequest(
        CreateRequest("m2", "https://example.com/", "GET", {}, nullptr, 64),
        &validated, &err));
    CurlRequestContext ctx;
    ctx.InitializeFromRequest(validated, t0);

    ASSERT_TRUE(ctx.OnHeaderLine("HTTP/1.1 200 OK\r\n", t0));
    ASSERT_TRUE(ctx.OnHeaderLine("Content-Length: 64\r\n", t0));
    ASSERT_TRUE(ctx.response_body.capacity() >= 64);

    const std::vector<uint8_t> data(64, 0xAA);
    ASSERT_TRUE(ctx.OnBodyChunk(data.data(), 32, t0));
    ASSERT_TRUE(ctx.OnBodyChunk(data.data() + 32, 32, t0));

    TurboHttpResponse res =
        CurlMultiTransport::BuildFinalResponse(200, CURLE_OK, ctx);
    ASSERT_TRUE(res.error_code() == nullptr);
    ASSERT_EQ(data, *res.body());
  }

  // 3. Exceeding by 1 byte in chunked stream aborts and clears partial body
  {
    ValidatedCurlRequest validated;
    std::string err;
    ASSERT_TRUE(ValidateCurlRequest(
        CreateRequest("m3", "https://example.com/", "GET", {}, nullptr, 64),
        &validated, &err));
    CurlRequestContext ctx;
    ctx.InitializeFromRequest(validated, t0);

    ASSERT_TRUE(ctx.OnHeaderLine("HTTP/1.1 200 OK\r\n", t0));
    const std::vector<uint8_t> chunk1(64, 0xBB);
    const uint8_t extra = 0xCC;
    ASSERT_TRUE(ctx.OnBodyChunk(chunk1.data(), chunk1.size(), t0));
    ASSERT_FALSE(ctx.OnBodyChunk(&extra, 1, t0));
    ASSERT_TRUE(ctx.is_exceeded_limit.load());
    ASSERT_TRUE(ctx.response_body.empty());

    TurboHttpResponse res = CurlMultiTransport::BuildFinalResponse(
        200, CURLE_WRITE_ERROR, ctx);
    ASSERT_EQ(std::string(TurboErrorCodes::kMaxResponseBytesExceeded),
              *res.error_code());
    ASSERT_TRUE(res.error_message()->find("64") != std::string::npos);
  }

  // 4. maxResponseBytes == 0: empty body succeeds, 1-byte body fails
  {
    ValidatedCurlRequest validated;
    std::string err;
    ASSERT_TRUE(ValidateCurlRequest(
        CreateRequest("m4", "https://example.com/", "GET", {}, nullptr, 0),
        &validated, &err));

    CurlRequestContext ctx_empty;
    ctx_empty.InitializeFromRequest(validated, t0);
    ASSERT_TRUE(ctx_empty.OnHeaderLine("HTTP/2 204\r\n", t0));
    ASSERT_TRUE(ctx_empty.OnHeaderLine("Content-Length: 0\r\n", t0));
    TurboHttpResponse res_empty =
        CurlMultiTransport::BuildFinalResponse(204, CURLE_OK, ctx_empty);
    ASSERT_TRUE(res_empty.error_code() == nullptr);
    ASSERT_EQ(static_cast<int64_t>(204), *res_empty.status_code());
    ASSERT_TRUE(res_empty.body()->empty());

    CurlRequestContext ctx_one;
    ctx_one.InitializeFromRequest(validated, t0);
    ASSERT_TRUE(ctx_one.OnHeaderLine("HTTP/2 200\r\n", t0));
    const uint8_t b = 0x01;
    ASSERT_FALSE(ctx_one.OnBodyChunk(&b, 1, t0));
    ASSERT_TRUE(ctx_one.is_exceeded_limit.load());
  }

  // 5. INT64_MAX boundary must not overflow
  {
    ValidatedCurlRequest validated;
    std::string err;
    ASSERT_TRUE(ValidateCurlRequest(
        CreateRequest("m5", "https://example.com/", "GET", {}, nullptr,
                      std::numeric_limits<int64_t>::max()),
        &validated, &err));
    CurlRequestContext ctx;
    ctx.InitializeFromRequest(validated, t0);
    ASSERT_TRUE(ctx.OnHeaderLine("HTTP/1.1 200 OK\r\n", t0));
    const std::vector<uint8_t> data(32, 0x55);
    ASSERT_TRUE(ctx.OnBodyChunk(data.data(), data.size(), t0));
    ASSERT_FALSE(ctx.is_exceeded_limit.load());
  }
}

TURBO_TEST(CurlRequestContext_PhaseTransitionsAndIdleTimeoutWatchdog) {
  const auto t0 = std::chrono::steady_clock::now();

  // 1. GET request: once TCP/TLS connect completes, transitions to kReceiving
  //    and enforces receiveTimeout (idle), not connectTimeout.
  {
    const int64_t conn_ms = 500;
    const int64_t recv_ms = 200;
    TurboHttpRequest req("t-get", "https://example.com/", "GET", {}, nullptr,
                         &conn_ms, &recv_ms, nullptr, true, false, nullptr);
    ValidatedCurlRequest validated;
    std::string err;
    ASSERT_TRUE(ValidateCurlRequest(req, &validated, &err));

    CurlRequestContext ctx;
    ctx.InitializeFromRequest(validated, t0);
    ASSERT_EQ(ExecutionPhase::kConnecting, ctx.phase);

    // At +100ms, connection finishes -> transitions to kReceiving
    const auto t1 = t0 + std::chrono::milliseconds(100);
    ASSERT_TRUE(ctx.OnProgress(/*dlnow=*/0, /*ulnow=*/0, /*ultotal=*/0,
                               /*connect_completed=*/true, t1));
    ASSERT_EQ(ExecutionPhase::kReceiving, ctx.phase);

    // At +250ms (150ms since connect), receive chunk resets idle timer
    const auto t2 = t0 + std::chrono::milliseconds(250);
    const uint8_t chunk = 0x41;
    ASSERT_TRUE(ctx.OnBodyChunk(&chunk, 1, t2));
    ASSERT_FALSE(ctx.CheckPhaseTimeout(t2 + std::chrono::milliseconds(150)));

    // Remaining timeout reflects idle deadline accurately
    auto remaining = ctx.RemainingTimeoutMs(t2 + std::chrono::milliseconds(150));
    ASSERT_TRUE(remaining.has_value());
    ASSERT_EQ(50, *remaining);

    // At t2 + 200ms (total 450ms > recv_ms=200ms idle), receiveTimeout fires
    ASSERT_TRUE(ctx.CheckPhaseTimeout(t2 + std::chrono::milliseconds(200)));
    ASSERT_NE(nullptr, ctx.timed_out_error_code);
    ASSERT_EQ(std::string(TurboErrorCodes::kReceiveTimeout),
              std::string(ctx.timed_out_error_code));
  }

  // 2. POST with body: kConnecting -> kSendingBody (idle reset on upload progress)
  //    -> kReceiving on upload completion.
  {
    const int64_t conn_ms = 500;
    const int64_t send_ms = 150;
    const int64_t recv_ms = 300;
    const std::vector<uint8_t> body(1024, 0x7A);
    TurboHttpRequest req("t-post", "https://example.com/", "POST", {}, &body,
                         &conn_ms, &recv_ms, &send_ms, true, false, nullptr);
    ValidatedCurlRequest validated;
    std::string err;
    ASSERT_TRUE(ValidateCurlRequest(req, &validated, &err));

    CurlRequestContext ctx;
    ctx.InitializeFromRequest(validated, t0);

    // Connected, upload starts partially
    const auto t1 = t0 + std::chrono::milliseconds(50);
    ASSERT_TRUE(ctx.OnProgress(/*dlnow=*/0, /*ulnow=*/256, /*ultotal=*/1024,
                               /*connect_completed=*/true, t1));
    ASSERT_EQ(ExecutionPhase::kSendingBody, ctx.phase);

    // Upload makes progress at +150ms -> resets send idle timer
    const auto t2 = t1 + std::chrono::milliseconds(100);
    ASSERT_TRUE(ctx.OnProgress(/*dlnow=*/0, /*ulnow=*/512, /*ultotal=*/1024,
                               /*connect_completed=*/true, t2));
    ASSERT_EQ(ExecutionPhase::kSendingBody, ctx.phase);

    // Stall for 150ms without upload progress -> sendTimeout fires
    const auto t3 = t2 + std::chrono::milliseconds(150);
    ASSERT_FALSE(ctx.OnProgress(/*dlnow=*/0, /*ulnow=*/512, /*ultotal=*/1024,
                                /*connect_completed=*/true, t3));
    ASSERT_NE(nullptr, ctx.timed_out_error_code);
    ASSERT_EQ(std::string(TurboErrorCodes::kSendTimeout),
              std::string(ctx.timed_out_error_code));
  }

  // 3. No timeouts configured -> RemainingTimeoutMs returns nullopt
  {
    TurboHttpRequest req("t-none", "https://example.com/", "GET");
    ValidatedCurlRequest validated;
    std::string err;
    ASSERT_TRUE(ValidateCurlRequest(req, &validated, &err));

    CurlRequestContext ctx;
    ctx.InitializeFromRequest(validated, t0);
    ASSERT_FALSE(ctx.RemainingTimeoutMs(t0).has_value());
  }
}

TURBO_TEST(CurlMultiTransport_BuildCurlHeaderList_HandlesExpectAndUserHeaders) {
  // Without user Expect header -> suppresses libcurl's automatic "Expect: 100-continue"
  {
    ValidatedCurlRequest validated;
    std::string err;
    ASSERT_TRUE(ValidateCurlRequest(
        CreateRequest("hdr1", "https://example.com/", "POST",
                      {{"Content-Type", "application/json"}}),
        &validated, &err));

    struct curl_slist* list = CurlMultiTransport::BuildCurlHeaderList(validated);
    ASSERT_NE(nullptr, list);

    bool found_content_type = false;
    bool found_empty_expect = false;
    for (struct curl_slist* node = list; node != nullptr; node = node->next) {
      std::string_view item(node->data);
      if (item == "Content-Type: application/json") found_content_type = true;
      if (item == "Expect:") found_empty_expect = true;
    }
    curl_slist_free_all(list);

    ASSERT_TRUE(found_content_type);
    ASSERT_TRUE(found_empty_expect);
  }

  // With user-supplied Expect header -> preserves user header without adding "Expect:"
  {
    ValidatedCurlRequest validated;
    std::string err;
    ASSERT_TRUE(ValidateCurlRequest(
        CreateRequest("hdr2", "https://example.com/", "POST",
                      {{"Expect", "100-continue"}}),
        &validated, &err));

    struct curl_slist* list = CurlMultiTransport::BuildCurlHeaderList(validated);
    ASSERT_NE(nullptr, list);

    bool found_user_expect = false;
    bool found_empty_expect = false;
    for (struct curl_slist* node = list; node != nullptr; node = node->next) {
      std::string_view item(node->data);
      if (item == "Expect: 100-continue") found_user_expect = true;
      if (item == "Expect:") found_empty_expect = true;
    }
    curl_slist_free_all(list);

    ASSERT_TRUE(found_user_expect);
    ASSERT_FALSE(found_empty_expect);
  }
}

// =============================================================================
// 5. TurboHttpExecutor: Deterministic Execution, Cancellation & Concurrency
// =============================================================================

TURBO_TEST(TurboHttpExecutor_Execute_InvalidRequest_FailsBeforeTransportAndLeavesZeroActive) {
  TurboSessionManager session_manager;
  std::atomic<int> transport_calls{0};
  TurboHttpExecutor executor(
      session_manager,
      [&](ValidatedCurlRequest /*req*/,
          std::shared_ptr<CurlRequestContext> /*ctx*/,
          std::function<void(TurboHttpResponse)> on_complete) {
        transport_calls.fetch_add(1);
        on_complete(MakeOkResponse());
      });

  // Invalid URL
  {
    auto res = ExecuteSync(
        executor, CreateRequest("bad-url", "htp://invalid url scheme", "GET"));
    ASSERT_NE(nullptr, res.error_code());
    ASSERT_EQ(std::string(TurboErrorCodes::kConnectionError), *res.error_code());
  }

  // Invalid method
  {
    auto res = ExecuteSync(
        executor, CreateRequest("bad-method", "https://example.com/", "BAD METHOD"));
    ASSERT_NE(nullptr, res.error_code());
    ASSERT_EQ(std::string(TurboErrorCodes::kConnectionError), *res.error_code());
  }

  // Invalid header characters
  {
    auto res = ExecuteSync(
        executor, CreateRequest("bad-hdr", "https://example.com/", "GET",
                                {{"Bad\r\nHeader", "Value"}}));
    ASSERT_NE(nullptr, res.error_code());
    ASSERT_EQ(std::string(TurboErrorCodes::kConnectionError), *res.error_code());
  }

  ASSERT_EQ(0, transport_calls.load());
}

TURBO_TEST(TurboHttpExecutor_Execute_PostBodiesAndEmptyResponse_HandledCorrectly) {
  TurboSessionManager session_manager;
  TurboHttpExecutor executor(
      session_manager,
      [](ValidatedCurlRequest req,
         std::shared_ptr<CurlRequestContext> /*ctx*/,
         std::function<void(TurboHttpResponse)> on_complete) {
        if (req.body.has_value()) {
          on_complete(MakeOkResponse(200, "OK", *req.body));
        } else {
          on_complete(MakeOkResponse(204, "No Content", {}));
        }
      });

  // POST with payload
  {
    const std::vector<uint8_t> payload = {'h', 'e', 'l', 'l', 'o'};
    auto res = ExecuteSync(
        executor,
        CreateRequest("req-post", "https://example.com/post", "POST", {}, &payload));
    ASSERT_TRUE(res.error_code() == nullptr);
    ASSERT_EQ(static_cast<int64_t>(200), *res.status_code());
    ASSERT_EQ(payload, *res.body());
  }

  // POST with null body -> empty 204 response safely
  {
    auto res = ExecuteSync(
        executor,
        CreateRequest("req-post-null", "https://example.com/post", "POST", {}, nullptr));
    ASSERT_TRUE(res.error_code() == nullptr);
    ASSERT_EQ(static_cast<int64_t>(204), *res.status_code());
    ASSERT_EQ(std::string("No Content"), *res.status_message());
    ASSERT_TRUE(res.body()->empty());
  }
}

TURBO_TEST(TurboHttpExecutor_Cancel_CancelsInFlightRequestExactlyOnce) {
  TurboSessionManager session_manager;
  std::mutex gate_mtx;
  std::condition_variable gate_cv;
  bool started = false;
  std::shared_ptr<CurlRequestContext> captured_ctx;
  std::function<void(TurboHttpResponse)> captured_complete;

  TurboHttpExecutor executor(
      session_manager,
      [&](ValidatedCurlRequest /*req*/,
          std::shared_ptr<CurlRequestContext> ctx,
          std::function<void(TurboHttpResponse)> on_complete) {
        {
          std::lock_guard<std::mutex> lock(gate_mtx);
          captured_ctx = std::move(ctx);
          captured_complete = std::move(on_complete);
          started = true;
        }
        gate_cv.notify_all();
      });

  std::atomic<int> callback_count{0};
  std::optional<TurboHttpResponse> result_opt;

  executor.Execute(
      CreateRequest("req-cancel", "https://example.com/sleep"),
      [&](TurboHttpResponse res) {
        callback_count.fetch_add(1);
        result_opt = std::move(res);
      });

  ASSERT_TRUE(started);
  ASSERT_TRUE(executor.has_active_request("req-cancel"));
  ASSERT_EQ(static_cast<size_t>(1), executor.active_request_count());

  executor.CancelRequest("req-cancel");
  ASSERT_TRUE(captured_ctx->is_cancelled.load());

  // Complete from transport side with cancel response (even if called twice,
  // executor must invoke user callback once and leave zero active).
  captured_complete(MakeCancelResponse());
  captured_complete(MakeCancelResponse());

  ASSERT_EQ(1, callback_count.load());
  ASSERT_TRUE(result_opt.has_value());
  ASSERT_EQ(std::string(TurboErrorCodes::kCancel), *result_opt->error_code());
  ASSERT_EQ(static_cast<size_t>(0), executor.active_request_count());
  ASSERT_FALSE(executor.has_active_request("req-cancel"));
}

TURBO_TEST(TurboHttpExecutor_DuplicateRequestId_OldCompletionDoesNotEraseNewEntry) {
  TurboSessionManager session_manager;
  std::vector<std::function<void(TurboHttpResponse)>> completions;

  TurboHttpExecutor executor(
      session_manager,
      [&](ValidatedCurlRequest /*req*/,
          std::shared_ptr<CurlRequestContext> /*ctx*/,
          std::function<void(TurboHttpResponse)> on_complete) {
        completions.push_back(std::move(on_complete));
      });

  int done_count = 0;
  executor.Execute(CreateRequest("dup-id"), [&](TurboHttpResponse /*res*/) {
    ++done_count;
  });
  executor.Execute(CreateRequest("dup-id"), [&](TurboHttpResponse /*res*/) {
    ++done_count;
  });

  ASSERT_EQ(static_cast<size_t>(2), completions.size());
  ASSERT_TRUE(executor.has_active_request("dup-id"));
  ASSERT_EQ(static_cast<size_t>(2), executor.active_request_count());

  // Completing the first request must NOT erase the second request's active entry.
  completions[0](MakeOkResponse());
  ASSERT_EQ(1, done_count);
  ASSERT_TRUE(executor.has_active_request("dup-id"));
  ASSERT_EQ(static_cast<size_t>(1), executor.active_request_count());

  // Completing the second request cleans up completely.
  completions[1](MakeOkResponse());
  ASSERT_EQ(2, done_count);
  ASSERT_EQ(static_cast<size_t>(0), executor.active_request_count());
  ASSERT_FALSE(executor.has_active_request("dup-id"));
}

TURBO_TEST(TurboHttpExecutor_Shutdown_CancelsAllDuplicateAndHighConcurrencyRequests) {
  TurboSessionManager session_manager;
  std::mutex mtx;
  std::vector<std::pair<std::shared_ptr<CurlRequestContext>,
                        std::function<void(TurboHttpResponse)>>>
      in_flight;

  TurboHttpExecutor executor(
      session_manager,
      [&](ValidatedCurlRequest /*req*/,
          std::shared_ptr<CurlRequestContext> ctx,
          std::function<void(TurboHttpResponse)> on_complete) {
        std::lock_guard<std::mutex> lock(mtx);
        in_flight.emplace_back(std::move(ctx), std::move(on_complete));
      });

  constexpr int kUniqueCount = 16;
  std::atomic<int> callback_count{0};

  for (int i = 0; i < kUniqueCount; ++i) {
    executor.Execute(
        CreateRequest("req-batch-" + std::to_string(i)),
        [&](TurboHttpResponse /*res*/) { callback_count.fetch_add(1); });
  }
  // Also submit duplicate IDs.
  for (int i = 0; i < 2; ++i) {
    executor.Execute(
        CreateRequest("dup-shutdown"),
        [&](TurboHttpResponse /*res*/) { callback_count.fetch_add(1); });
  }

  ASSERT_EQ(static_cast<size_t>(kUniqueCount + 2), in_flight.size());

  executor.Shutdown();

  // Every context—including both duplicate ID contexts—must be marked cancelled.
  for (auto& [ctx, complete] : in_flight) {
    ASSERT_TRUE(ctx->is_cancelled.load());
    complete(MakeCancelResponse());
  }

  ASSERT_EQ(kUniqueCount + 2, callback_count.load());
  ASSERT_EQ(static_cast<size_t>(0), executor.active_request_count());

  // Repeated Shutdown must be idempotent.
  executor.Shutdown();
}

TURBO_TEST(TurboHttpExecutor_Execute_AfterShutdown_ReturnsCancelImmediately) {
  TurboSessionManager session_manager;
  std::atomic<int> transport_calls{0};
  TurboHttpExecutor executor(
      session_manager,
      [&](ValidatedCurlRequest /*req*/,
          std::shared_ptr<CurlRequestContext> /*ctx*/,
          std::function<void(TurboHttpResponse)> on_complete) {
        transport_calls.fetch_add(1);
        on_complete(MakeOkResponse());
      });

  executor.Shutdown();

  auto res = ExecuteSync(
      executor, CreateRequest("req-after-shutdown", "https://example.com/api"));
  ASSERT_NE(nullptr, res.error_code());
  ASSERT_EQ(std::string(TurboErrorCodes::kCancel), *res.error_code());
  ASSERT_EQ(0, transport_calls.load());
}

TURBO_TEST(TurboHttpExecutor_Execute_TransportException_ReturnsUnknownAndCleansUp) {
  TurboSessionManager session_manager;
  TurboHttpExecutor executor(
      session_manager,
      [](ValidatedCurlRequest /*req*/,
         std::shared_ptr<CurlRequestContext> /*ctx*/,
         std::function<void(TurboHttpResponse)> /*on_complete*/) {
        throw std::runtime_error("simulated transport failure");
      });

  auto res = ExecuteSync(
      executor, CreateRequest("req-throw", "https://example.com/api"));
  ASSERT_NE(nullptr, res.error_code());
  ASSERT_EQ(std::string(TurboErrorCodes::kUnknown), *res.error_code());
  ASSERT_NE(nullptr, res.error_message());
  ASSERT_TRUE(res.error_message()->find("simulated transport failure") !=
              std::string::npos);
}

TURBO_TEST(TurboHttpExecutor_ExecuteAndShutdown_ConcurrentStress_CompletesSafely) {
  TurboSessionManager session_manager;
  constexpr int kRequests = 32;

  std::mutex pool_mtx;
  std::vector<std::thread> worker_threads;

  std::mutex start_mtx;
  std::condition_variable start_cv;
  int started = 0;

  TurboHttpExecutor executor(
      session_manager,
      [&](ValidatedCurlRequest /*req*/,
          std::shared_ptr<CurlRequestContext> ctx,
          std::function<void(TurboHttpResponse)> on_complete) {
        {
          std::lock_guard<std::mutex> lock(start_mtx);
          ++started;
        }
        start_cv.notify_one();

        std::lock_guard<std::mutex> lock(pool_mtx);
        worker_threads.emplace_back(
            [ctx = std::move(ctx), on_complete = std::move(on_complete)]() {
              if (ctx->is_cancelled.load()) {
                on_complete(MakeCancelResponse());
              } else {
                on_complete(MakeOkResponse());
              }
            });
      });

  std::mutex done_mtx;
  std::condition_variable done_cv;
  int completed = 0;

  std::thread shutdown_thread([&]() {
    {
      std::unique_lock<std::mutex> lock(start_mtx);
      start_cv.wait_for(lock, std::chrono::seconds(5),
                        [&] { return started >= 4; });
    }
    executor.Shutdown();
  });

  for (int i = 0; i < kRequests; ++i) {
    executor.Execute(
        CreateRequest("stress-" + std::to_string(i)),
        [&](TurboHttpResponse /*res*/) {
          {
            std::lock_guard<std::mutex> lock(done_mtx);
            ++completed;
          }
          done_cv.notify_one();
        });
  }

  shutdown_thread.join();
  {
    std::lock_guard<std::mutex> lock(pool_mtx);
    for (auto& t : worker_threads) {
      if (t.joinable()) t.join();
    }
  }

  std::unique_lock<std::mutex> lock(done_mtx);
  ASSERT_TRUE(done_cv.wait_for(lock, std::chrono::seconds(5),
                               [&] { return completed == kRequests; }));
  ASSERT_EQ(static_cast<size_t>(0), executor.active_request_count());
}

TURBO_TEST(CurlRequestContext_BuildFinalResponse_EnforcesErrorPriority) {
  const auto t0 = std::chrono::steady_clock::now();
  ValidatedCurlRequest validated;
  std::string err;
  ASSERT_TRUE(ValidateCurlRequest(
      CreateRequest("prio", "https://example.com/", "GET", {}, nullptr, 16),
      &validated, &err));

  // 1. maxResponseBytesExceeded takes top priority over timeout and cancel.
  {
    CurlRequestContext ctx;
    ctx.InitializeFromRequest(validated, t0);
    ctx.is_exceeded_limit.store(true);
    ctx.timed_out_error_code = TurboErrorCodes::kReceiveTimeout;
    ctx.is_cancelled.store(true);

    TurboHttpResponse res = CurlMultiTransport::BuildFinalResponse(
        200, CURLE_ABORTED_BY_CALLBACK, ctx);
    ASSERT_NE(nullptr, res.error_code());
    ASSERT_EQ(std::string(TurboErrorCodes::kMaxResponseBytesExceeded),
              *res.error_code());
  }

  // 2. Phase timeout takes priority over cancel/CURLE_ABORTED_BY_CALLBACK.
  {
    CurlRequestContext ctx;
    ctx.InitializeFromRequest(validated, t0);
    ctx.timed_out_error_code = TurboErrorCodes::kSendTimeout;
    ctx.is_cancelled.store(true);

    TurboHttpResponse res = CurlMultiTransport::BuildFinalResponse(
        0, CURLE_ABORTED_BY_CALLBACK, ctx);
    ASSERT_NE(nullptr, res.error_code());
    ASSERT_EQ(std::string(TurboErrorCodes::kSendTimeout), *res.error_code());
  }
}

// =============================================================================
// 6. Real CurlMultiTransport Loopback Verification (No External Network)
// =============================================================================

namespace {

class LoopbackHttpServer {
 public:
  using Handler = std::function<void(int client_fd, const std::string& request)>;

  explicit LoopbackHttpServer(Handler handler)
      : handler_(std::move(handler)) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(listen_fd_ >= 0);

    int opt = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ASSERT_EQ(0, ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr),
                        sizeof(addr)));
    ASSERT_EQ(0, ::listen(listen_fd_, 8));

    socklen_t len = sizeof(addr);
    ASSERT_EQ(0, ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr),
                               &len));
    port_ = ntohs(addr.sin_port);

    server_thread_ = std::thread([this]() { ServeLoop(); });
  }

  ~LoopbackHttpServer() { Stop(); }

  uint16_t port() const { return port_; }

  void Stop() {
    if (stopped_.exchange(true)) return;
    if (listen_fd_ >= 0) {
      ::shutdown(listen_fd_, SHUT_RDWR);
    }
    if (server_thread_.joinable()) {
      server_thread_.join();
    }
    if (listen_fd_ >= 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
    }
  }

 private:
  static size_t ExtractContentLength(const std::string& headers) {
    size_t pos = 0;
    while (pos < headers.size()) {
      size_t end = headers.find("\r\n", pos);
      if (end == std::string::npos) {
        end = headers.size();
      }
      std::string_view line(headers.data() + pos, end - pos);
      std::string key;
      std::string_view val;
      if (ParseHeaderLine(line, &key, &val) && key == "content-length") {
        auto parsed = ParseContentLength(val);
        if (parsed.has_value() && *parsed > 0) {
          return static_cast<size_t>(*parsed);
        }
      }
      pos = end + 2;
    }
    return 0;
  }

  void ServeLoop() {
    while (!stopped_.load()) {
      int client_fd = ::accept(listen_fd_, nullptr, nullptr);
      if (client_fd < 0) {
        break;
      }
      std::string raw_req;
      char buf[1024];
      size_t header_end = std::string::npos;
      while ((header_end = raw_req.find("\r\n\r\n")) == std::string::npos) {
        ssize_t n = ::recv(client_fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        raw_req.append(buf, static_cast<size_t>(n));
      }
      if (header_end != std::string::npos) {
        const size_t body_len =
            ExtractContentLength(raw_req.substr(0, header_end));
        const size_t target_size = header_end + 4 + body_len;
        while (raw_req.size() < target_size) {
          ssize_t n = ::recv(client_fd, buf, sizeof(buf), 0);
          if (n <= 0) break;
          raw_req.append(buf, static_cast<size_t>(n));
        }
      }
      if (!raw_req.empty()) {
        handler_(client_fd, raw_req);
      }
      ::close(client_fd);
    }
  }

  Handler handler_;
  int listen_fd_ = -1;
  uint16_t port_ = 0;
  std::atomic<bool> stopped_{false};
  std::thread server_thread_;
};

}  // namespace

TURBO_TEST(CurlMultiTransport_Loopback_EmptyPostRedirectAndCancel) {
  std::atomic<bool> release_slow{false};
  std::mutex slow_mtx;
  std::condition_variable slow_cv;

  LoopbackHttpServer server([&](int client_fd, const std::string& req) {
    if (req.rfind("POST /empty", 0) == 0) {
      const std::string resp =
          "HTTP/1.1 200 Created\r\n"
          "Content-Length: 2\r\n"
          "Connection: close\r\n"
          "\r\n"
          "ok";
      (void)::send(client_fd, resp.data(), resp.size(), 0);
    } else if (req.rfind("GET /redirect", 0) == 0) {
      const std::string resp =
          "HTTP/1.1 302 Found\r\n"
          "Location: /final\r\n"
          "X-Redirect-Only: drop-me\r\n"
          "Content-Length: 4096\r\n"
          "Connection: close\r\n"
          "\r\n";
      (void)::send(client_fd, resp.data(), resp.size(), 0);
    } else if (req.rfind("GET /final", 0) == 0) {
      const std::string resp =
          "HTTP/1.1 200 Done\r\n"
          "X-Final: kept\r\n"
          "Content-Length: 4\r\n"
          "Connection: close\r\n"
          "\r\n"
          "done";
      (void)::send(client_fd, resp.data(), resp.size(), 0);
    } else if (req.rfind("GET /slow", 0) == 0) {
      std::unique_lock<std::mutex> lock(slow_mtx);
      slow_cv.wait_for(lock, std::chrono::seconds(5),
                       [&] { return release_slow.load(); });
    }
  });

  const std::string base_url =
      "http://127.0.0.1:" + std::to_string(server.port());

  TurboSessionManager session_manager;
  TurboHttpExecutor executor(session_manager);

  // 1. Empty-body POST must complete cleanly without hanging on stdin.
  {
    auto res = ExecuteSync(
        executor, CreateRequest("lb-post", base_url + "/empty", "POST", {}, nullptr));
    ASSERT_TRUE(res.error_code() == nullptr);
    ASSERT_EQ(static_cast<int64_t>(200), *res.status_code());
    ASSERT_EQ(std::string("Created"), *res.status_message());
    ASSERT_EQ(std::vector<uint8_t>({'o', 'k'}), *res.body());
  }

  // 2. Redirect follow drops intermediate 302 headers and ignores 302 Content-Length.
  {
    auto res = ExecuteSync(
        executor,
        CreateRequest("lb-redir", base_url + "/redirect", "GET", {}, nullptr,
                      /*max_response_bytes=*/64, /*follow_redirects=*/true));
    ASSERT_TRUE(res.error_code() == nullptr);
    ASSERT_EQ(static_cast<int64_t>(200), *res.status_code());
    ASSERT_EQ(std::string("Done"), *res.status_message());
    ASSERT_TRUE(res.headers().find("x-redirect-only") == res.headers().end());
    ASSERT_EQ(std::string("kept"), res.headers().at("x-final")[0]);
    ASSERT_EQ(std::vector<uint8_t>({'d', 'o', 'n', 'e'}), *res.body());
  }

  // 3. In-flight slow request cancelled promptly via curl_multi_wakeup().
  {
    std::mutex mtx;
    std::condition_variable cv;
    std::optional<TurboHttpResponse> result_opt;

    executor.Execute(
        CreateRequest("lb-slow", base_url + "/slow", "GET"),
        [&](TurboHttpResponse res) {
          {
            std::lock_guard<std::mutex> lock(mtx);
            result_opt = std::move(res);
          }
          cv.notify_one();
        });

    ASSERT_TRUE(executor.has_active_request("lb-slow"));
    executor.CancelRequest("lb-slow");

    std::unique_lock<std::mutex> lock(mtx);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5),
                            [&] { return result_opt.has_value(); }));
    ASSERT_NE(nullptr, result_opt->error_code());
    ASSERT_EQ(std::string(TurboErrorCodes::kCancel), *result_opt->error_code());
  }

  executor.Shutdown();
  session_manager.Shutdown();
  release_slow.store(true);
  slow_cv.notify_all();
  server.Stop();
}

TURBO_TEST(CurlHelpers_ValidateCurlRequest_MoveOverloadTransfersBodyWithoutCopy) {
  std::optional<std::vector<uint8_t>> body = std::vector<uint8_t>(4096, 0xAB);
  const uint8_t* original_ptr = body->data();

  TurboHttpRequest request("req-move", "https://example.com/upload", "POST", {},
                           std::move(body), nullptr, nullptr, nullptr, true,
                           false, nullptr);

  ValidatedCurlRequest validated;
  std::string err;
  ASSERT_TRUE(ValidateCurlRequest(std::move(request), &validated, &err));
  ASSERT_TRUE(validated.body.has_value());
  ASSERT_EQ(static_cast<size_t>(4096), validated.body->size());
  // Buffer ownership moved directly without copying payload bytes.
  ASSERT_EQ(original_ptr, validated.body->data());
}

TURBO_TEST(CurlRequestContext_RemainingTimeoutMs_ReturnsZeroWhenAlreadyExpired) {
  const auto t0 = std::chrono::steady_clock::now();
  const int64_t conn_ms = 100;
  TurboHttpRequest req("t-exp", "https://example.com/", "GET", {}, nullptr,
                       &conn_ms, nullptr, nullptr, true, false, nullptr);
  ValidatedCurlRequest validated;
  std::string err;
  ASSERT_TRUE(ValidateCurlRequest(req, &validated, &err));

  CurlRequestContext ctx;
  ctx.InitializeFromRequest(validated, t0);

  // At or past deadline, RemainingTimeoutMs must return 0 (no extra sleep).
  auto rem_exact = ctx.RemainingTimeoutMs(t0 + std::chrono::milliseconds(100));
  ASSERT_TRUE(rem_exact.has_value());
  ASSERT_EQ(0, *rem_exact);

  auto rem_past = ctx.RemainingTimeoutMs(t0 + std::chrono::milliseconds(250));
  ASSERT_TRUE(rem_past.has_value());
  ASSERT_EQ(0, *rem_past);
}

TURBO_TEST(CurlMultiTransport_Loopback_NoTimeoutCancelAndEasyHandleReuse) {
  std::atomic<bool> release_slow{false};
  std::mutex slow_mtx;
  std::condition_variable slow_cv;

  LoopbackHttpServer server([&](int client_fd, const std::string& req) {
    if (req.rfind("GET /ping", 0) == 0) {
      const std::string resp =
          "HTTP/1.1 200 OK\r\n"
          "Content-Length: 4\r\n"
          "Connection: close\r\n"
          "\r\n"
          "pong";
      (void)::send(client_fd, resp.data(), resp.size(), 0);
    } else if (req.rfind("GET /slow", 0) == 0) {
      std::unique_lock<std::mutex> lock(slow_mtx);
      slow_cv.wait_for(lock, std::chrono::seconds(5),
                       [&] { return release_slow.load(); });
    }
  });

  const std::string base_url =
      "http://127.0.0.1:" + std::to_string(server.port());

  TurboSessionManager session_manager;
  TurboHttpExecutor executor(session_manager);

  // 1. Sequential requests with no timeouts (CURLOPT_NOPROGRESS = 1L) exercise
  //    easy-handle pooling & curl_easy_reset() reuse across requests.
  for (int i = 0; i < 4; ++i) {
    TurboHttpRequest req("reuse-" + std::to_string(i), base_url + "/ping",
                         "GET");
    auto res = ExecuteSync(executor, std::move(req));
    ASSERT_TRUE(res.error_code() == nullptr);
    ASSERT_EQ(static_cast<int64_t>(200), *res.status_code());
    ASSERT_EQ(std::vector<uint8_t>({'p', 'o', 'n', 'g'}), *res.body());
  }

  // 2. Cancellation still aborts promptly even when no timeouts are configured.
  {
    std::mutex mtx;
    std::condition_variable cv;
    std::optional<TurboHttpResponse> result_opt;

    TurboHttpRequest slow_req("no-timeout-cancel", base_url + "/slow", "GET");
    executor.Execute(std::move(slow_req), [&](TurboHttpResponse res) {
      {
        std::lock_guard<std::mutex> lock(mtx);
        result_opt = std::move(res);
      }
      cv.notify_one();
    });

    ASSERT_TRUE(executor.has_active_request("no-timeout-cancel"));
    executor.CancelRequest("no-timeout-cancel");

    std::unique_lock<std::mutex> lock(mtx);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5),
                            [&] { return result_opt.has_value(); }));
    ASSERT_NE(nullptr, result_opt->error_code());
    ASSERT_EQ(std::string(TurboErrorCodes::kCancel), *result_opt->error_code());
  }

  executor.Shutdown();
  session_manager.Shutdown();
  release_slow.store(true);
  slow_cv.notify_all();
  server.Stop();
}

TURBO_TEST(CurlHelpers_ValidateCurlRequest_ValidatesPortsAndIpv6Authorities) {
  ValidatedCurlRequest validated;
  std::string err;

  // Valid ports and IPv6 literals
  ASSERT_TRUE(ValidateCurlRequest(
      CreateRequest("p1", "http://example.com:1/"), &validated, &err));
  ASSERT_TRUE(ValidateCurlRequest(
      CreateRequest("p2", "https://example.com:65535/api"), &validated, &err));
  ASSERT_TRUE(ValidateCurlRequest(
      CreateRequest("p3", "http://[::1]/"), &validated, &err));
  ASSERT_TRUE(ValidateCurlRequest(
      CreateRequest("p4", "https://[::1]:8443/v1"), &validated, &err));

  // Invalid ports and malformed IPv6 suffixes
  ASSERT_FALSE(ValidateCurlRequest(
      CreateRequest("ip1", "http://example.com:0/"), &validated, &err));
  ASSERT_FALSE(ValidateCurlRequest(
      CreateRequest("ip2", "http://example.com:65536/"), &validated, &err));
  ASSERT_FALSE(ValidateCurlRequest(
      CreateRequest("ip3", "http://example.com:abc/"), &validated, &err));
  ASSERT_FALSE(ValidateCurlRequest(
      CreateRequest("ip4", "http://example.com:/"), &validated, &err));
  ASSERT_FALSE(ValidateCurlRequest(
      CreateRequest("ip5", "http://example.com:80:80/"), &validated, &err));
  ASSERT_FALSE(ValidateCurlRequest(
      CreateRequest("ip6", "http://[::1]garbage/"), &validated, &err));
  ASSERT_FALSE(ValidateCurlRequest(
      CreateRequest("ip7", "http://[::1]:99999/"), &validated, &err));
  ASSERT_FALSE(ValidateCurlRequest(
      CreateRequest("ip8", "http://[]/"), &validated, &err));
}

TURBO_TEST(CurlRequestContext_RedirectWithBodyAndInformational1xx_HandledSafely) {
  const auto t0 = std::chrono::steady_clock::now();

  // 1. 100 Continue must not overwrite kSendingBody phase.
  {
    const std::vector<uint8_t> req_body(128, 0x33);
    ValidatedCurlRequest validated;
    std::string err;
    ASSERT_TRUE(ValidateCurlRequest(
        CreateRequest("r-100", "https://example.com/upload", "POST", {},
                      &req_body, /*max_response_bytes=*/64),
        &validated, &err));

    CurlRequestContext ctx;
    ctx.InitializeFromRequest(validated, t0);
    ctx.phase = ExecutionPhase::kSendingBody;

    ASSERT_TRUE(ctx.OnHeaderLine("HTTP/1.1 100 Continue\r\n", t0));
    ASSERT_TRUE(ctx.OnHeaderLine("\r\n", t0));
    ASSERT_EQ(ExecutionPhase::kSendingBody, ctx.phase);

    // Subsequent 200 OK transitions to kReceiving cleanly.
    ASSERT_TRUE(ctx.OnHeaderLine("HTTP/1.1 200 OK\r\n", t0));
    ASSERT_EQ(ExecutionPhase::kReceiving, ctx.phase);
  }

  // 2. 302 intermediate body exceeding maxResponseBytes must be discarded
  //    without aborting or contaminating the final 200 body.
  {
    ValidatedCurlRequest validated;
    std::string err;
    ASSERT_TRUE(ValidateCurlRequest(
        CreateRequest("r-302-body", "https://example.com/start", "GET", {},
                      nullptr, /*max_response_bytes=*/16,
                      /*follow_redirects=*/true),
        &validated, &err));

    CurlRequestContext ctx;
    ctx.InitializeFromRequest(validated, t0);

    ASSERT_TRUE(ctx.OnHeaderLine("HTTP/1.1 302 Found\r\n", t0));
    ASSERT_TRUE(ctx.OnHeaderLine("Location: /final\r\n", t0));
    ASSERT_TRUE(ctx.OnHeaderLine("Content-Length: 200\r\n", t0));
    ASSERT_TRUE(ctx.OnHeaderLine("\r\n", t0));

    const std::vector<uint8_t> html_body(200, 'X');
    ASSERT_TRUE(ctx.OnBodyChunk(html_body.data(), html_body.size(), t0));
    ASSERT_FALSE(ctx.is_exceeded_limit.load());
    ASSERT_TRUE(ctx.response_body.empty());

    ASSERT_TRUE(ctx.OnHeaderLine("HTTP/1.1 200 OK\r\n", t0));
    ASSERT_TRUE(ctx.OnHeaderLine("Content-Length: 5\r\n", t0));
    ASSERT_TRUE(ctx.OnHeaderLine("\r\n", t0));

    const std::vector<uint8_t> final_body = {'f', 'i', 'n', 'a', 'l'};
    ASSERT_TRUE(ctx.OnBodyChunk(final_body.data(), final_body.size(), t0));

    TurboHttpResponse res =
        CurlMultiTransport::BuildFinalResponse(200, CURLE_OK, ctx);
    ASSERT_TRUE(res.error_code() == nullptr);
    ASSERT_EQ(final_body, *res.body());
  }

  // 3. 3xx response without a Location header (or 300 Multiple Choices) is a
  //    final response whose body must be preserved and bounded by maxResponseBytes.
  {
    ValidatedCurlRequest validated;
    std::string err;
    ASSERT_TRUE(ValidateCurlRequest(
        CreateRequest("r-300-final", "https://example.com/choices", "GET", {},
                      nullptr, /*max_response_bytes=*/16,
                      /*follow_redirects=*/true),
        &validated, &err));

    CurlRequestContext ctx;
    ctx.InitializeFromRequest(validated, t0);
    ASSERT_TRUE(ctx.OnHeaderLine("HTTP/1.1 300 Multiple Choices\r\n", t0));
    ASSERT_TRUE(ctx.OnHeaderLine("Content-Length: 7\r\n", t0));
    ASSERT_TRUE(ctx.OnHeaderLine("\r\n", t0));

    const std::vector<uint8_t> choices = {'c', 'h', 'o', 'i', 'c', 'e', 's'};
    ASSERT_TRUE(ctx.OnBodyChunk(choices.data(), choices.size(), t0));

    TurboHttpResponse res =
        CurlMultiTransport::BuildFinalResponse(300, CURLE_OK, ctx);
    ASSERT_TRUE(res.error_code() == nullptr);
    ASSERT_EQ(static_cast<int64_t>(300), *res.status_code());
    ASSERT_EQ(choices, *res.body());
  }

  // 4. 302 without Location header still enforces maxResponseBytes on its body.
  {
    ValidatedCurlRequest validated;
    std::string err;
    ASSERT_TRUE(ValidateCurlRequest(
        CreateRequest("r-302-noloc", "https://example.com/noloc", "GET", {},
                      nullptr, /*max_response_bytes=*/4,
                      /*follow_redirects=*/true),
        &validated, &err));

    CurlRequestContext ctx;
    ctx.InitializeFromRequest(validated, t0);
    ASSERT_TRUE(ctx.OnHeaderLine("HTTP/1.1 302 Found\r\n", t0));
    ASSERT_TRUE(ctx.OnHeaderLine("Content-Length: 8\r\n", t0));
    ASSERT_TRUE(ctx.OnHeaderLine("\r\n", t0));

    const std::vector<uint8_t> body(8, 'Z');
    ASSERT_FALSE(ctx.OnBodyChunk(body.data(), body.size(), t0));
    ASSERT_TRUE(ctx.is_exceeded_limit.load());
  }

  // 5. Callback exception flag maps cleanly to unknown error.
  {
    CurlRequestContext ctx;
    ctx.callback_exception.store(true);
    ctx.callback_exception_message = "out of memory";
    TurboHttpResponse res =
        CurlMultiTransport::BuildFinalResponse(200, CURLE_WRITE_ERROR, ctx);
    ASSERT_NE(nullptr, res.error_code());
    ASSERT_EQ(std::string(TurboErrorCodes::kUnknown), *res.error_code());
    ASSERT_TRUE(res.error_message()->find("out of memory") != std::string::npos);
  }
}

TURBO_TEST(CurlMultiTransport_Loopback_RedirectHtmlBodyGetWithBodyAndMaxRedirs) {
  std::atomic<bool> release_stall{false};
  std::mutex stall_mtx;
  std::condition_variable stall_cv;

  LoopbackHttpServer server([&](int client_fd, const std::string& req) {
    if (req.rfind("GET /redir-body", 0) == 0) {
      const std::string html(200, 'R');
      const std::string resp =
          "HTTP/1.1 302 Found\r\n"
          "Location: /final-body\r\n"
          "Content-Length: " +
          std::to_string(html.size()) +
          "\r\n"
          "Connection: close\r\n"
          "\r\n" +
          html;
      (void)::send(client_fd, resp.data(), resp.size(), 0);
    } else if (req.rfind("GET /final-body", 0) == 0) {
      const std::string resp =
          "HTTP/1.1 200 OK\r\n"
          "Content-Length: 5\r\n"
          "Connection: close\r\n"
          "\r\n"
          "final";
      (void)::send(client_fd, resp.data(), resp.size(), 0);
    } else if (req.rfind("GET /get-body", 0) == 0) {
      const std::string resp =
          "HTTP/1.1 200 OK\r\n"
          "Content-Length: 6\r\n"
          "Connection: close\r\n"
          "\r\n"
          "gotget";
      (void)::send(client_fd, resp.data(), resp.size(), 0);
    } else if (req.rfind("GET /loop", 0) == 0) {
      const std::string resp =
          "HTTP/1.1 302 Found\r\n"
          "Location: /loop\r\n"
          "Content-Length: 0\r\n"
          "Connection: close\r\n"
          "\r\n";
      (void)::send(client_fd, resp.data(), resp.size(), 0);
    } else if (req.rfind("GET /choices-300", 0) == 0) {
      const std::string resp =
          "HTTP/1.1 300 Multiple Choices\r\n"
          "Content-Length: 7\r\n"
          "Connection: close\r\n"
          "\r\n"
          "choices";
      (void)::send(client_fd, resp.data(), resp.size(), 0);
    } else if (req.rfind("GET /redir-ftp", 0) == 0) {
      const std::string resp =
          "HTTP/1.1 302 Found\r\n"
          "Location: ftp://127.0.0.1/blocked\r\n"
          "Content-Length: 0\r\n"
          "Connection: close\r\n"
          "\r\n";
      (void)::send(client_fd, resp.data(), resp.size(), 0);
    } else if (req.rfind("GET /stall-body", 0) == 0) {
      const std::string headers =
          "HTTP/1.1 200 OK\r\n"
          "Content-Length: 100\r\n"
          "Connection: close\r\n"
          "\r\n"
          "hi";
      (void)::send(client_fd, headers.data(), headers.size(), 0);
      std::unique_lock<std::mutex> lock(stall_mtx);
      stall_cv.wait_for(lock, std::chrono::seconds(5),
                        [&] { return release_stall.load(); });
    } else {
      const std::string resp =
          "HTTP/1.1 405 Method Not Allowed\r\n"
          "Content-Length: 0\r\n"
          "Connection: close\r\n"
          "\r\n";
      (void)::send(client_fd, resp.data(), resp.size(), 0);
    }
  });

  const std::string base_url =
      "http://127.0.0.1:" + std::to_string(server.port());

  TurboSessionManager session_manager;
  TurboHttpExecutor executor(session_manager);

  // 1. 302 response carrying a 200-byte HTML body with maxResponseBytes=32
  //    must succeed and return only the final 5-byte body.
  {
    auto res = ExecuteSync(
        executor,
        CreateRequest("lb-redir-html", base_url + "/redir-body", "GET", {},
                      nullptr, /*max_response_bytes=*/32,
                      /*follow_redirects=*/true));
    ASSERT_TRUE(res.error_code() == nullptr);
    ASSERT_EQ(static_cast<int64_t>(200), *res.status_code());
    ASSERT_EQ(std::vector<uint8_t>({'f', 'i', 'n', 'a', 'l'}), *res.body());
  }

  // 2. GET request with a non-empty body must remain GET (not silently POST).
  {
    const std::vector<uint8_t> body = {'q', '=', '1'};
    auto res = ExecuteSync(
        executor,
        CreateRequest("lb-get-body", base_url + "/get-body", "GET", {}, &body));
    ASSERT_TRUE(res.error_code() == nullptr);
    ASSERT_EQ(static_cast<int64_t>(200), *res.status_code());
    ASSERT_EQ(std::vector<uint8_t>({'g', 'o', 't', 'g', 'e', 't'}),
              *res.body());
  }

  // 3. Infinite redirect loop is capped by CURLOPT_MAXREDIRS -> connectionError.
  {
    auto res = ExecuteSync(
        executor, CreateRequest("lb-redir-loop", base_url + "/loop", "GET"));
    ASSERT_NE(nullptr, res.error_code());
    ASSERT_EQ(std::string(TurboErrorCodes::kConnectionError), *res.error_code());
  }

  // 4. 300 Multiple Choices without Location is not redirected; body is preserved.
  {
    auto res = ExecuteSync(
        executor,
        CreateRequest("lb-300", base_url + "/choices-300", "GET", {}, nullptr,
                      /*max_response_bytes=*/32, /*follow_redirects=*/true));
    ASSERT_TRUE(res.error_code() == nullptr);
    ASSERT_EQ(static_cast<int64_t>(300), *res.status_code());
    ASSERT_EQ(std::vector<uint8_t>({'c', 'h', 'o', 'i', 'c', 'e', 's'}),
              *res.body());
  }

  // 5. Redirect to non-HTTP scheme (ftp://) is blocked -> connectionError.
  {
    auto res = ExecuteSync(
        executor, CreateRequest("lb-redir-ftp", base_url + "/redir-ftp", "GET"));
    ASSERT_NE(nullptr, res.error_code());
    ASSERT_EQ(std::string(TurboErrorCodes::kConnectionError), *res.error_code());
  }

  // 6. Stalled body delivery triggers receiveTimeout through real transport.
  {
    const int64_t connect_ms = 5000;
    const int64_t receive_ms = 150;
    TurboHttpRequest req("lb-recv-timeout", base_url + "/stall-body", "GET", {},
                         nullptr, &connect_ms, &receive_ms, nullptr, true,
                         false, nullptr);
    auto res = ExecuteSync(executor, req);
    ASSERT_NE(nullptr, res.error_code());
    ASSERT_EQ(std::string(TurboErrorCodes::kReceiveTimeout), *res.error_code());
  }

  executor.Shutdown();
  session_manager.Shutdown();
  release_stall.store(true);
  stall_cv.notify_all();
  server.Stop();
}

}  // namespace test
}  // namespace flutter_turbo_fetch
