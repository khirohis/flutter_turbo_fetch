#include "turbo_session_manager.h"
#include "test/test_assertions.h"

namespace flutter_turbo_fetch {
namespace test {

TURBO_TEST(TurboSessionManager_AcquireShareHandle_ReusesSharedHandle) {
  TurboSessionManager manager;

  CURLSH* share1 = manager.AcquireShareHandle();
  ASSERT_NE(nullptr, share1);
  ASSERT_FALSE(manager.IsShutdown());

  // Repeated acquisition reuses the same shared CURLSH handle
  CURLSH* share2 = manager.AcquireShareHandle();
  ASSERT_EQ(share1, share2);

  manager.Shutdown();
  ASSERT_TRUE(manager.IsShutdown());
}

TURBO_TEST(TurboSessionManager_AcquireShareHandle_RecoversAfterShutdown) {
  TurboSessionManager manager;

  CURLSH* initial_share = manager.AcquireShareHandle();
  ASSERT_NE(nullptr, initial_share);

  manager.Shutdown();
  ASSERT_TRUE(manager.IsShutdown());

  // Re-acquiring after Shutdown creates a new valid share handle
  CURLSH* recovered_share = manager.AcquireShareHandle();
  ASSERT_NE(nullptr, recovered_share);
  ASSERT_FALSE(manager.IsShutdown());

  manager.Shutdown();
}

}  // namespace test
}  // namespace flutter_turbo_fetch

