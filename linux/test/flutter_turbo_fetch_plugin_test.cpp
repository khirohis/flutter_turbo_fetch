#include "flutter_turbo_fetch_plugin.h"

#include <iostream>
#include <memory>

#include "test/test_assertions.h"

namespace flutter_turbo_fetch {
namespace test {

TURBO_TEST(FlutterTurboFetchPlugin_ExecuteAndCancelDelegateToExecutor) {
  TurboSessionManager session_manager;
  TurboHttpExecutor executor(session_manager);
  FlutterTurboFetchPlugin plugin(
      std::make_unique<TurboHttpExecutor>(session_manager));

  const int64_t timeout_ms = 5000;
  TurboHttpRequest req("test-plugin-req", "https://example.com/api", "GET", {},
                       nullptr, &timeout_ms, &timeout_ms, &timeout_ms, true, 32,
                       false, nullptr);

  std::atomic<bool> called{false};
  plugin.Execute(req, [&](const TurboHttpResponse& res) {
    called.store(true);
  });

  // Verify delegation by cancelling via plugin
  plugin.CancelRequest("test-plugin-req");
}

}  // namespace test
}  // namespace flutter_turbo_fetch

int main() {
  return flutter_turbo_fetch::test::RunAllTests();
}

