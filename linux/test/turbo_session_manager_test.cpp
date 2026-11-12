#include "turbo_session_manager.h"
#include "test/test_assertions.h"

namespace flutter_turbo_fetch {
namespace test {

TURBO_TEST(TurboSessionManager_AcquireShareHandle_CreatesAndScalesLimitsDynamically) {
  TurboSessionManager manager;

  CURLSH* share1 = manager.AcquireShareHandle(32);
  ASSERT_NE(nullptr, share1);
  ASSERT_EQ(32, manager.current_max_per_host());

  // Scale up to 100 on the same shared session
  CURLSH* share2 = manager.AcquireShareHandle(100);
  ASSERT_EQ(share1, share2);
  ASSERT_EQ(100, manager.current_max_per_host());

  // Scale down to 16 on the same shared session
  CURLSH* share3 = manager.AcquireShareHandle(16);
  ASSERT_EQ(share1, share3);
  ASSERT_EQ(16, manager.current_max_per_host());

  manager.Shutdown();
  ASSERT_TRUE(manager.IsShutdown());
}

TURBO_TEST(TurboSessionManager_AcquireShareHandle_RecoversAfterShutdown) {
  TurboSessionManager manager;

  CURLSH* initial_share = manager.AcquireShareHandle(32);
  ASSERT_NE(nullptr, initial_share);

  manager.Shutdown();
  ASSERT_TRUE(manager.IsShutdown());

  // Re-acquiring after Shutdown creates a new valid share handle
  CURLSH* recovered_share = manager.AcquireShareHandle(48);
  ASSERT_NE(nullptr, recovered_share);
  ASSERT_FALSE(manager.IsShutdown());
  ASSERT_EQ(48, manager.current_max_per_host());

  manager.Shutdown();
}

TURBO_TEST(
    TurboSessionManager_AcquireShareHandle_WithZeroOrNegativeLimit_CoercesToOneSafely) {
  TurboSessionManager manager;

  // Zero limit coerces to 1
  CURLSH* share_zero = manager.AcquireShareHandle(0);
  ASSERT_NE(nullptr, share_zero);
  ASSERT_EQ(1, manager.current_max_per_host());

  // Negative limit coerces to 1
  CURLSH* share_neg = manager.AcquireShareHandle(-10);
  ASSERT_NE(nullptr, share_neg);
  ASSERT_EQ(1, manager.current_max_per_host());

  manager.Shutdown();
}

}  // namespace test
}  // namespace flutter_turbo_fetch

