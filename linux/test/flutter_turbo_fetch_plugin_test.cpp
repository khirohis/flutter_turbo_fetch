#include "flutter_turbo_fetch_plugin.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "test/test_assertions.h"

namespace flutter_turbo_fetch {
namespace test {

TURBO_TEST(FlutterTurboFetchPlugin_ExecuteAndCancelDelegateToExecutor) {
  TurboSessionManager session_manager;
  std::shared_ptr<CurlRequestContext> captured_ctx;
  std::function<void(TurboHttpResponse)> captured_complete;

  auto executor = std::make_unique<TurboHttpExecutor>(
      session_manager,
      [&](ValidatedCurlRequest /*req*/,
          std::shared_ptr<CurlRequestContext> ctx,
          std::function<void(TurboHttpResponse)> on_complete) {
        captured_ctx = std::move(ctx);
        captured_complete = std::move(on_complete);
      });

  FlutterTurboFetchPlugin plugin(std::move(executor));

  const int64_t timeout_ms = 5000;
  TurboHttpRequest req("test-plugin-req", "https://example.com/api", "GET", {},
                       nullptr, &timeout_ms, &timeout_ms, &timeout_ms, true,
                       false, nullptr);

  std::atomic<int> callback_count{0};
  std::string observed_error;
  plugin.Execute(req, [&](const TurboHttpResponse& res) {
    callback_count.fetch_add(1);
    if (res.error_code() != nullptr) {
      observed_error = *res.error_code();
    }
  });

  ASSERT_NE(nullptr, captured_ctx.get());
  plugin.CancelRequest("test-plugin-req");
  ASSERT_TRUE(captured_ctx->is_cancelled.load());

  captured_complete(MakeCancelResponse());
  ASSERT_EQ(1, callback_count.load());
  ASSERT_EQ(std::string(TurboErrorCodes::kCancel), observed_error);

  plugin.Shutdown();
}

TURBO_TEST(FlutterTurboFetchPlugin_Shutdown_CleansUpExecutorAndOwnedSessionSafely) {
  FlutterTurboFetchPlugin plugin;

  // Shutdown before any requests, then Execute after Shutdown must return cancel.
  plugin.Shutdown();

  const int64_t timeout_ms = 5000;
  TurboHttpRequest req("test-plugin-shutdown", "https://example.com/api", "GET",
                       {}, nullptr, &timeout_ms, &timeout_ms, &timeout_ms, true,
                       false, nullptr);

  std::atomic<int> callback_count{0};
  plugin.Execute(req, [&](const TurboHttpResponse& res) {
    if (res.error_code() != nullptr &&
        *res.error_code() == TurboErrorCodes::kCancel) {
      callback_count.fetch_add(1);
    }
  });

  ASSERT_EQ(1, callback_count.load());

  // Repeated Shutdown must be idempotent and safe.
  plugin.Shutdown();
}

}  // namespace test
}  // namespace flutter_turbo_fetch

int main() {
  return flutter_turbo_fetch::test::RunAllTests();
}
