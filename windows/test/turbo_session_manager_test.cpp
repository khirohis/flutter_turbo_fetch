#include "turbo_session_manager.h"
#include "test/test_assertions.h"

namespace flutter_turbo_fetch {
namespace test {

TURBO_TEST(TurboSessionManager_AcquireSession_ReusesSharedSession) {
  TurboSessionManager manager;

  HINTERNET session1 = manager.AcquireSession();
  ASSERT_NE(nullptr, session1);
  ASSERT_FALSE(manager.IsShutdown());

  // Repeated acquisition reuses the same shared session handle
  HINTERNET session2 = manager.AcquireSession();
  ASSERT_EQ(session1, session2);

  manager.Shutdown();
  ASSERT_TRUE(manager.IsShutdown());
}

TURBO_TEST(TurboSessionManager_AcquireSession_RecoversAfterShutdown) {
  TurboSessionManager manager;

  HINTERNET initial_session = manager.AcquireSession();
  ASSERT_NE(nullptr, initial_session);

  manager.Shutdown();
  ASSERT_TRUE(manager.IsShutdown());

  // Re-acquiring after Shutdown creates a new valid session handle
  HINTERNET recovered_session = manager.AcquireSession();
  ASSERT_NE(nullptr, recovered_session);
  ASSERT_FALSE(manager.IsShutdown());

  manager.Shutdown();
}

}  // namespace test
}  // namespace flutter_turbo_fetch

