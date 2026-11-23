#include "turbo_session_manager.h"
#include "test/test_assertions.h"

namespace flutter_turbo_fetch {
namespace test {

TURBO_TEST(TurboSessionManager_AcquireMultiHandle_ReusesMultiHandle) {
  TurboSessionManager manager;

  CURLM* multi1 = manager.AcquireMultiHandle();
  ASSERT_NE(nullptr, multi1);
  ASSERT_FALSE(manager.IsShutdown());

  // Repeated acquisition reuses the same CURLM handle
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

  // Re-acquiring after Shutdown creates a new valid multi handle
  CURLM* recovered_multi = manager.AcquireMultiHandle();
  ASSERT_NE(nullptr, recovered_multi);
  ASSERT_FALSE(manager.IsShutdown());

  manager.Shutdown();
}

}  // namespace test
}  // namespace flutter_turbo_fetch

