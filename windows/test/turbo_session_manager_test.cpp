#include "turbo_session_manager.h"
#include "test/test_assertions.h"

namespace flutter_turbo_fetch {
namespace test {

TURBO_TEST(TurboSessionManager_AcquireSession_CreatesAndScalesLimitsDynamically) {
  TurboSessionManager manager;

  // Default scale (32)
  HINTERNET session1 = manager.AcquireSession(32);
  ASSERT_NE(nullptr, session1);
  ASSERT_EQ(32, manager.current_max_per_host());

  // Scale up to 100 on the same shared session
  HINTERNET session2 = manager.AcquireSession(100);
  ASSERT_EQ(session1, session2);
  ASSERT_EQ(100, manager.current_max_per_host());

  // Scale down to 16 on the same shared session
  HINTERNET session3 = manager.AcquireSession(16);
  ASSERT_EQ(session1, session3);
  ASSERT_EQ(16, manager.current_max_per_host());

  manager.Shutdown();
  ASSERT_TRUE(manager.IsShutdown());
}

TURBO_TEST(TurboSessionManager_AcquireSession_RecoversAfterShutdown) {
  TurboSessionManager manager;

  HINTERNET initial_session = manager.AcquireSession(32);
  ASSERT_NE(nullptr, initial_session);

  manager.Shutdown();
  ASSERT_TRUE(manager.IsShutdown());

  // Re-acquiring after Shutdown creates a new valid session handle
  HINTERNET recovered_session = manager.AcquireSession(48);
  ASSERT_NE(nullptr, recovered_session);
  ASSERT_FALSE(manager.IsShutdown());
  ASSERT_EQ(48, manager.current_max_per_host());

  manager.Shutdown();
}

}  // namespace test
}  // namespace flutter_turbo_fetch

