#include "turbo_session_manager.h"

#include <atomic>
#include <thread>
#include <vector>

#include "test/test_assertions.h"

namespace flutter_turbo_fetch {
namespace test {

TURBO_TEST(TurboSessionManager_AcquireMultiHandle_ReusesMultiHandle) {
  TurboSessionManager manager;

  CURLM* multi1 = manager.AcquireMultiHandle();
  ASSERT_NE(nullptr, multi1);
  ASSERT_FALSE(manager.IsShutdown());

  // Repeated acquisition reuses the same CURLM handle.
  CURLM* multi2 = manager.AcquireMultiHandle();
  ASSERT_EQ(multi1, multi2);

  manager.Shutdown();
  ASSERT_TRUE(manager.IsShutdown());
}

TURBO_TEST(TurboSessionManager_AcquireMultiHandle_RecoversAfterShutdown) {
  TurboSessionManager manager;

  CURLM* initial_multi = manager.AcquireMultiHandle();
  ASSERT_NE(nullptr, initial_multi);

  manager.Shutdown();
  ASSERT_TRUE(manager.IsShutdown());

  // Re-acquiring after Shutdown creates a new valid multi handle.
  CURLM* recovered_multi = manager.AcquireMultiHandle();
  ASSERT_NE(nullptr, recovered_multi);
  ASSERT_FALSE(manager.IsShutdown());

  manager.Shutdown();
}

TURBO_TEST(TurboSessionManager_Wakeup_BeforeAcquireAndAfterShutdown_IsSafe) {
  TurboSessionManager manager;

  // Wakeup before any handle is created must be a safe no-op.
  manager.Wakeup();

  CURLM* multi = manager.AcquireMultiHandle();
  ASSERT_NE(nullptr, multi);
  manager.Wakeup();

  // Repeated Shutdown and subsequent Wakeup must remain safe.
  manager.Shutdown();
  manager.Shutdown();
  ASSERT_TRUE(manager.IsShutdown());
  manager.Wakeup();
}

TURBO_TEST(TurboSessionManager_ConcurrentLifecycle_InitializesAndCleansUpSafely) {
  constexpr int kThreads = 8;
  constexpr int kIterations = 4;
  std::atomic<int> success_count{0};

  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&]() {
      for (int i = 0; i < kIterations; ++i) {
        TurboSessionManager local_manager;
        CURLM* multi = local_manager.AcquireMultiHandle();
        if (multi != nullptr) {
          local_manager.Wakeup();
          local_manager.Shutdown();
          success_count.fetch_add(1);
        }
      }
    });
  }

  for (auto& worker : workers) {
    worker.join();
  }

  ASSERT_EQ(kThreads * kIterations, success_count.load());
}

}  // namespace test
}  // namespace flutter_turbo_fetch
