#include "flutter_turbo_fetch_plugin.h"

#include <iostream>
#include <memory>

#include "test/test_assertions.h"

namespace flutter_turbo_fetch {
namespace test {

TURBO_TEST(FlutterTurboFetchPlugin_ExecuteAndCancelDelegateToExecutor) {
  TurboSessionManager session_manager;
  FlutterTurboFetchPlugin plugin(
      std::make_unique<TurboHttpExecutor>(session_manager));

  const int64_t timeout_ms = 5000;
  TurboHttpRequest req("test-plugin-req", "https://example.com/api", "GET", {},
                       nullptr, &timeout_ms, &timeout_ms, &timeout_ms, true,
                       false, nullptr);

  std::atomic<bool> called{false};
  plugin.Execute(req, [&](const TurboHttpResponse&) {
    called.store(true);
  });

  // Verify delegation by cancelling via plugin
  plugin.CancelRequest("test-plugin-req");
  plugin.Shutdown();
  ASSERT_TRUE(called.load());
}

TURBO_TEST(FlutterTurboFetchPlugin_Shutdown_CleansUpExecutorAndOwnedSessionSafely) {
  FlutterTurboFetchPlugin plugin;

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

  plugin.Shutdown();
  ASSERT_EQ(1, callback_count.load());
}

}  // namespace test
}  // namespace flutter_turbo_fetch

int main() {
  return flutter_turbo_fetch::test::RunAllTests();
}

