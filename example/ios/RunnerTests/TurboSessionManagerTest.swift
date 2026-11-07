import Foundation
import XCTest

@testable import flutter_turbo_fetch

final class TurboSessionManagerTest: XCTestCase {
  func testAcquireSession_reusesSessionAndScalesLimitsDynamically() {
    let manager = TurboSessionManager()
    defer { manager.shutdown() }

    // Default scale (32)
    let session1 = manager.acquireSession(maxConnectionsPerHost: 32)
    XCTAssertEqual(32, session1.configuration.httpMaximumConnectionsPerHost)
    XCTAssertEqual(32, manager.currentMaxConnectionsPerHost)

    // Same limit reuses existing URLSession instance
    let session1Same = manager.acquireSession(maxConnectionsPerHost: 32)
    XCTAssertTrue(session1 === session1Same)

    // Scale up to 64 recreates URLSession with updated limit
    let session2 = manager.acquireSession(maxConnectionsPerHost: 64)
    XCTAssertFalse(session1 === session2)
    XCTAssertEqual(64, session2.configuration.httpMaximumConnectionsPerHost)
    XCTAssertEqual(64, manager.currentMaxConnectionsPerHost)

    // Scale down to 16 recreates URLSession with updated limit
    let session3 = manager.acquireSession(maxConnectionsPerHost: 16)
    XCTAssertFalse(session2 === session3)
    XCTAssertEqual(16, session3.configuration.httpMaximumConnectionsPerHost)
    XCTAssertEqual(16, manager.currentMaxConnectionsPerHost)
  }

  func testAcquireSession_recoversAfterShutdown() {
    let manager = TurboSessionManager()

    let initialSession = manager.acquireSession(maxConnectionsPerHost: 32)
    XCTAssertFalse(manager.isShutdown)

    manager.shutdown()
    XCTAssertTrue(manager.isShutdown)

    // Re-acquiring after shutdown creates a fresh URLSession instance
    let recoveredSession = manager.acquireSession(maxConnectionsPerHost: 48)
    XCTAssertFalse(initialSession === recoveredSession)
    XCTAssertFalse(manager.isShutdown)
    XCTAssertEqual(48, recoveredSession.configuration.httpMaximumConnectionsPerHost)
    XCTAssertEqual(48, manager.currentMaxConnectionsPerHost)

    manager.shutdown()
  }

  func testTaskContext_usesObjectIdentifierToPreventCrossSessionTaskIdCollision() {
    let manager = TurboSessionManager()
    defer { manager.shutdown() }

    let url = URL(string: "https://example.com/test")!
    let sessionA = manager.acquireSession(maxConnectionsPerHost: 8)
    let taskA = sessionA.dataTask(with: url)

    // Switching maxConnectionsPerHost creates a new URLSession whose first task also gets taskIdentifier == 1
    let sessionB = manager.acquireSession(maxConnectionsPerHost: 16)
    let taskB = sessionB.dataTask(with: url)

    manager.registerTask(
      task: taskA,
      followRedirects: false,
      allowBadCertificates: false,
      hasRequestBody: false,
      connectTimeoutMs: nil,
      sendTimeoutMs: nil,
      receiveTimeoutMs: nil
    )
    manager.registerTask(
      task: taskB,
      followRedirects: true,
      allowBadCertificates: true,
      hasRequestBody: false,
      connectTimeoutMs: nil,
      sendTimeoutMs: nil,
      receiveTimeoutMs: nil
    )

    let contextA = manager.removeTask(for: taskA)
    let contextB = manager.removeTask(for: taskB)

    XCTAssertNotNil(contextA)
    XCTAssertNotNil(contextB)
    XCTAssertEqual(false, contextA?.followRedirects)
    XCTAssertEqual(false, contextA?.allowBadCertificates)
    XCTAssertEqual(true, contextB?.followRedirects)
    XCTAssertEqual(true, contextB?.allowBadCertificates)

    taskA.cancel()
    taskB.cancel()
  }
}

