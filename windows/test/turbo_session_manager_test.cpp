#include "turbo_session_manager.h"

#include <string>

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

TURBO_TEST(
    TurboSessionManager_AcquireConnection_CachesPerOriginAndDistinguishesHostPort) {
  TurboSessionManager manager;

  auto conn1 = manager.AcquireConnection(L"example.com", 443);
  ASSERT_NE(nullptr, conn1);
  ASSERT_NE(nullptr, conn1->handle);
  ASSERT_EQ(static_cast<size_t>(1), manager.CachedConnectionCount());

  // Same host and port must reuse the cached connection handle
  auto conn1_again = manager.AcquireConnection(L"example.com", 443);
  ASSERT_NE(nullptr, conn1_again);
  ASSERT_EQ(conn1->handle, conn1_again->handle);
  ASSERT_EQ(static_cast<size_t>(1), manager.CachedConnectionCount());

  // Different port must allocate a separate connection handle
  auto conn_diff_port = manager.AcquireConnection(L"example.com", 8443);
  ASSERT_NE(nullptr, conn_diff_port);
  ASSERT_NE(nullptr, conn_diff_port->handle);
  ASSERT_NE(conn1->handle, conn_diff_port->handle);
  ASSERT_EQ(static_cast<size_t>(2), manager.CachedConnectionCount());

  // Different host must allocate a separate connection handle
  auto conn_diff_host = manager.AcquireConnection(L"api.example.com", 443);
  ASSERT_NE(nullptr, conn_diff_host);
  ASSERT_NE(nullptr, conn_diff_host->handle);
  ASSERT_NE(conn1->handle, conn_diff_host->handle);
  ASSERT_EQ(static_cast<size_t>(3), manager.CachedConnectionCount());

  manager.Shutdown();
}

TURBO_TEST(
    TurboSessionManager_AcquireConnection_EvictsWhenExceedingMaxCacheSize) {
  TurboSessionManager manager;

  const size_t total_origins = TurboSessionManager::kMaxCachedConnections + 5;
  for (size_t i = 0; i < total_origins; ++i) {
    const std::wstring host = L"host" + std::to_wstring(i) + L".example.com";
    auto conn = manager.AcquireConnection(host, 443);
    ASSERT_NE(nullptr, conn);
    ASSERT_NE(nullptr, conn->handle);
  }

  ASSERT_TRUE(manager.CachedConnectionCount() <=
              TurboSessionManager::kMaxCachedConnections);
  ASSERT_EQ(TurboSessionManager::kMaxCachedConnections,
            manager.CachedConnectionCount());

  manager.Shutdown();
}

TURBO_TEST(TurboSessionManager_Shutdown_ClearsConnectionCacheAndRecovers) {
  TurboSessionManager manager;

  auto conn_before = manager.AcquireConnection(L"example.com", 443);
  ASSERT_NE(nullptr, conn_before);
  ASSERT_EQ(static_cast<size_t>(1), manager.CachedConnectionCount());

  manager.Shutdown();
  ASSERT_TRUE(manager.IsShutdown());
  ASSERT_EQ(static_cast<size_t>(0), manager.CachedConnectionCount());

  // Re-acquiring after Shutdown transparently recreates session & connection
  auto conn_after = manager.AcquireConnection(L"example.com", 443);
  ASSERT_NE(nullptr, conn_after);
  ASSERT_NE(nullptr, conn_after->handle);
  ASSERT_FALSE(manager.IsShutdown());
  ASSERT_EQ(static_cast<size_t>(1), manager.CachedConnectionCount());

  manager.Shutdown();
}

TURBO_TEST(
    TurboSessionManager_AcquireConnection_LongHostAndMultiPortOrdering) {
  TurboSessionManager manager;

  const std::wstring long_host_a = L"very-long-subdomain-a.api.example.com";
  const std::wstring long_host_b = L"very-long-subdomain-b.api.example.com";

  auto a443 = manager.AcquireConnection(long_host_a, 443);
  auto a8443 = manager.AcquireConnection(long_host_a, 8443);
  auto b443 = manager.AcquireConnection(long_host_b, 443);

  ASSERT_NE(nullptr, a443);
  ASSERT_NE(nullptr, a8443);
  ASSERT_NE(nullptr, b443);
  ASSERT_EQ(static_cast<size_t>(3), manager.CachedConnectionCount());

  // Repeated lookups must hit exact cached entries
  ASSERT_EQ(a443->handle, manager.AcquireConnection(long_host_a, 443)->handle);
  ASSERT_EQ(a8443->handle,
            manager.AcquireConnection(long_host_a, 8443)->handle);
  ASSERT_EQ(b443->handle, manager.AcquireConnection(long_host_b, 443)->handle);
  ASSERT_EQ(static_cast<size_t>(3), manager.CachedConnectionCount());

  manager.Shutdown();
}

}  // namespace test
}  // namespace flutter_turbo_fetch

