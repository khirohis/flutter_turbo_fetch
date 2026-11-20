import Foundation
import XCTest

@testable import flutter_turbo_fetch

final class TurboSessionManagerTest: XCTestCase {
  func testAcquireSession_reusesSingleSharedSessionWithOsDefaults() {
    let manager = TurboSessionManager()
    defer { manager.shutdown() }

    let session1 = manager.acquireSession()
    XCTAssertFalse(manager.isShutdown)
    XCTAssertEqual(
      URLSessionConfiguration.default.httpMaximumConnectionsPerHost,
      session1.configuration.httpMaximumConnectionsPerHost
    )

    // Repeated acquisition reuses the same URLSession instance
    let session2 = manager.acquireSession()
    XCTAssertTrue(session1 === session2)
  }

  func testAcquireSession_recoversAfterShutdown() {
    let manager = TurboSessionManager()

    let initialSession = manager.acquireSession()
    XCTAssertFalse(manager.isShutdown)

    manager.shutdown()
    XCTAssertTrue(manager.isShutdown)

    // Re-acquiring after shutdown creates a fresh URLSession instance
    let recoveredSession = manager.acquireSession()
    XCTAssertFalse(initialSession === recoveredSession)
    XCTAssertFalse(manager.isShutdown)

    manager.shutdown()
  }

  func testTaskContext_usesObjectIdentifierToPreventCrossSessionTaskIdCollision() {
    let manager = TurboSessionManager()
    defer { manager.shutdown() }

    let url = URL(string: "https://example.com/test")!
    let sessionA = manager.acquireSession()
    let taskA = sessionA.dataTask(with: url)

    // Recreating URLSession after shutdown yields a new session whose first task also gets taskIdentifier == 1
    manager.shutdown()
    let sessionB = manager.acquireSession()
    let taskB = sessionB.dataTask(with: url)
    XCTAssertEqual(taskA.taskIdentifier, taskB.taskIdentifier)

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

