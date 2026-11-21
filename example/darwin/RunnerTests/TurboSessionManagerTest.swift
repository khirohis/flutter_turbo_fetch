import Foundation
import Security
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

  func testDelegate_willPerformHTTPRedirection_respectsFollowRedirectsPolicy() {
    let delegate = TurboSessionTaskDelegate()
    let session = URLSession(configuration: .ephemeral)
    defer {
      delegate.removeAll()
      session.invalidateAndCancel()
    }

    let originalUrl = URL(string: "https://example.com/start")!
    let redirectUrl = URL(string: "https://example.com/redirected")!
    let httpResponse = HTTPURLResponse(
      url: originalUrl,
      statusCode: 302,
      httpVersion: "HTTP/2.0",
      headerFields: ["Location": redirectUrl.absoluteString]
    )!
    let redirectRequest = URLRequest(url: redirectUrl)

    func evaluateRedirect(for task: URLSessionTask) -> URLRequest? {
      let exp = expectation(description: "Redirect completion invoked")
      var result: URLRequest?
      var callCount = 0
      delegate.urlSession(
        session,
        task: task,
        willPerformHTTPRedirection: httpResponse,
        newRequest: redirectRequest
      ) { nextRequest in
        callCount += 1
        result = nextRequest
        exp.fulfill()
      }
      wait(for: [exp], timeout: 1.0)
      XCTAssertEqual(1, callCount, "Redirect completion handler must be called once")
      return result
    }

    let noRedirectTask = session.dataTask(with: originalUrl)
    delegate.registerTask(
      task: noRedirectTask,
      followRedirects: false,
      allowBadCertificates: false,
      hasRequestBody: false,
      connectTimeoutMs: nil,
      sendTimeoutMs: nil,
      receiveTimeoutMs: nil
    )
    XCTAssertNil(evaluateRedirect(for: noRedirectTask))

    let followRedirectTask = session.dataTask(with: originalUrl)
    delegate.registerTask(
      task: followRedirectTask,
      followRedirects: true,
      allowBadCertificates: false,
      hasRequestBody: false,
      connectTimeoutMs: nil,
      sendTimeoutMs: nil,
      receiveTimeoutMs: nil
    )
    XCTAssertEqual(redirectUrl, evaluateRedirect(for: followRedirectTask)?.url)

    let unregisteredTask = session.dataTask(with: originalUrl)
    XCTAssertEqual(redirectUrl, evaluateRedirect(for: unregisteredTask)?.url)

    noRedirectTask.cancel()
    followRedirectTask.cancel()
    unregisteredTask.cancel()
  }

  func testDelegate_didReceiveChallenge_respectsAllowBadCertificatesPolicy() throws {
    let delegate = TurboSessionTaskDelegate()
    let session = URLSession(configuration: .ephemeral)
    defer {
      delegate.removeAll()
      session.invalidateAndCancel()
    }

    let url = URL(string: "https://test.example.local/resource")!
    let serverTrust = try createTestServerTrust(host: "test.example.local")
    let challengeSender = StubAuthenticationChallengeSender()

    func evaluateChallenge(
      task: URLSessionTask,
      authenticationMethod: String,
      trust: SecTrust?
    ) -> (URLSession.AuthChallengeDisposition, URLCredential?) {
      let protectionSpace = StubURLProtectionSpace(
        host: "test.example.local",
        port: 443,
        protocol: NSURLProtectionSpaceHTTPS,
        realm: nil,
        authenticationMethod: authenticationMethod,
        serverTrust: trust
      )
      let challenge = URLAuthenticationChallenge(
        protectionSpace: protectionSpace,
        proposedCredential: nil,
        previousFailureCount: 0,
        failureResponse: nil,
        error: nil,
        sender: challengeSender
      )
      let exp = expectation(description: "Auth challenge completion invoked")
      var capturedDisposition: URLSession.AuthChallengeDisposition?
      var capturedCredential: URLCredential?
      var callCount = 0
      delegate.urlSession(session, task: task, didReceive: challenge) { disposition, credential in
        callCount += 1
        capturedDisposition = disposition
        capturedCredential = credential
        exp.fulfill()
      }
      wait(for: [exp], timeout: 1.0)
      XCTAssertEqual(1, callCount, "Challenge completion handler must be called once")
      return (capturedDisposition!, capturedCredential)
    }

    // 1. allowBadCertificates = true + ServerTrust + non-nil SecTrust -> .useCredential
    let allowTask = session.dataTask(with: url)
    delegate.registerTask(
      task: allowTask,
      followRedirects: true,
      allowBadCertificates: true,
      hasRequestBody: false,
      connectTimeoutMs: nil,
      sendTimeoutMs: nil,
      receiveTimeoutMs: nil
    )
    let allowedResult = evaluateChallenge(
      task: allowTask,
      authenticationMethod: NSURLAuthenticationMethodServerTrust,
      trust: serverTrust
    )
    XCTAssertEqual(.useCredential, allowedResult.0)
    XCTAssertNotNil(allowedResult.1)

    // 2. allowBadCertificates = true + non-ServerTrust method -> .performDefaultHandling
    let nonTrustResult = evaluateChallenge(
      task: allowTask,
      authenticationMethod: NSURLAuthenticationMethodHTTPBasic,
      trust: nil
    )
    XCTAssertEqual(.performDefaultHandling, nonTrustResult.0)
    XCTAssertNil(nonTrustResult.1)

    // 3. allowBadCertificates = true + ServerTrust + nil SecTrust -> .performDefaultHandling
    let nilTrustResult = evaluateChallenge(
      task: allowTask,
      authenticationMethod: NSURLAuthenticationMethodServerTrust,
      trust: nil
    )
    XCTAssertEqual(.performDefaultHandling, nilTrustResult.0)
    XCTAssertNil(nilTrustResult.1)

    // 4. allowBadCertificates = false + ServerTrust + non-nil SecTrust -> .performDefaultHandling
    let strictTask = session.dataTask(with: url)
    delegate.registerTask(
      task: strictTask,
      followRedirects: true,
      allowBadCertificates: false,
      hasRequestBody: false,
      connectTimeoutMs: nil,
      sendTimeoutMs: nil,
      receiveTimeoutMs: nil
    )
    let strictResult = evaluateChallenge(
      task: strictTask,
      authenticationMethod: NSURLAuthenticationMethodServerTrust,
      trust: serverTrust
    )
    XCTAssertEqual(.performDefaultHandling, strictResult.0)
    XCTAssertNil(strictResult.1)

    // 5. Unregistered task defaults to strict validation -> .performDefaultHandling
    let unregisteredTask = session.dataTask(with: url)
    let unregisteredResult = evaluateChallenge(
      task: unregisteredTask,
      authenticationMethod: NSURLAuthenticationMethodServerTrust,
      trust: serverTrust
    )
    XCTAssertEqual(.performDefaultHandling, unregisteredResult.0)
    XCTAssertNil(unregisteredResult.1)

    allowTask.cancel()
    strictTask.cancel()
    unregisteredTask.cancel()
  }

  func testDelegate_didSendBodyData_transitionsTimersAndCompletionState() {
    let delegate = TurboSessionTaskDelegate()
    let session = URLSession(configuration: .ephemeral)
    defer {
      delegate.removeAll()
      session.invalidateAndCancel()
    }

    let url = URL(string: "https://example.com/upload")!
    let postTask = session.dataTask(with: url)
    delegate.registerTask(
      task: postTask,
      followRedirects: true,
      allowBadCertificates: false,
      hasRequestBody: true,
      connectTimeoutMs: 60_000,
      sendTimeoutMs: 60_000,
      receiveTimeoutMs: 60_000
    )

    // Initial state with request body: connect & send timers active, receive timer deferred
    XCTAssertEqual(
      TurboSessionTaskDelegate.TaskTimerSnapshot(
        isBodySendCompleted: false,
        hasConnectTimer: true,
        hasSendTimer: true,
        hasReceiveTimer: false
      ),
      delegate.taskTimerSnapshot(for: postTask)
    )

    // Partial body send cancels connect timer while keeping send timer active
    delegate.urlSession(
      session,
      task: postTask,
      didSendBodyData: 50,
      totalBytesSent: 50,
      totalBytesExpectedToSend: 100
    )
    XCTAssertEqual(
      TurboSessionTaskDelegate.TaskTimerSnapshot(
        isBodySendCompleted: false,
        hasConnectTimer: false,
        hasSendTimer: true,
        hasReceiveTimer: false
      ),
      delegate.taskTimerSnapshot(for: postTask)
    )

    // Completing body send cancels send timer and starts receive timer
    delegate.urlSession(
      session,
      task: postTask,
      didSendBodyData: 50,
      totalBytesSent: 100,
      totalBytesExpectedToSend: 100
    )
    XCTAssertEqual(
      TurboSessionTaskDelegate.TaskTimerSnapshot(
        isBodySendCompleted: true,
        hasConnectTimer: false,
        hasSendTimer: false,
        hasReceiveTimer: true
      ),
      delegate.taskTimerSnapshot(for: postTask)
    )

    // Duplicate callback after completion is idempotent
    delegate.urlSession(
      session,
      task: postTask,
      didSendBodyData: 0,
      totalBytesSent: 100,
      totalBytesExpectedToSend: 100
    )
    XCTAssertEqual(
      TurboSessionTaskDelegate.TaskTimerSnapshot(
        isBodySendCompleted: true,
        hasConnectTimer: false,
        hasSendTimer: false,
        hasReceiveTimer: true
      ),
      delegate.taskTimerSnapshot(for: postTask)
    )

    // Request without body starts with body send completed and receive timer armed immediately
    let getTask = session.dataTask(with: url)
    delegate.registerTask(
      task: getTask,
      followRedirects: true,
      allowBadCertificates: false,
      hasRequestBody: false,
      connectTimeoutMs: 60_000,
      sendTimeoutMs: 60_000,
      receiveTimeoutMs: 60_000
    )
    XCTAssertEqual(
      TurboSessionTaskDelegate.TaskTimerSnapshot(
        isBodySendCompleted: true,
        hasConnectTimer: true,
        hasSendTimer: false,
        hasReceiveTimer: true
      ),
      delegate.taskTimerSnapshot(for: getTask)
    )

    delegate.removeTask(for: postTask)
    XCTAssertNil(delegate.taskTimerSnapshot(for: postTask))

    postTask.cancel()
    getTask.cancel()
  }

  private func createTestServerTrust(host: String = "test.example.local") throws -> SecTrust {
    // Self-signed ECDSA P-256 public certificate (CN=test.example.local, no private key).
    let certBase64 = [
      "MIIBLzCB1qADAgECAggt93rkPXp0iDAKBggqhkjOPQQDAjAdMRswGQYDVQQDExJ0",
      "ZXN0LmV4YW1wbGUubG9jYWwwIBcNMjYxMTE5MTgyNDU5WhgPMjA3NjExMjAxODI0",
      "NTlaMB0xGzAZBgNVBAMTEnRlc3QuZXhhbXBsZS5sb2NhbDBZMBMGByqGSM49AgEG",
      "CCqGSM49AwEHA0IABCZFZFWxbFES+kw+tS6Cl1ZqjZcZW1I10jgEgxAy05aR4f47",
      "2oB0JtDrgl4eYyvpgPVrkpe6A5U7i5Xu8tUc/RowCgYIKoZIzj0EAwIDSAAwRQIh",
      "AMN6xYAAycRzpVqym+mf1+R/QftWF16fXS50sVnY1owTAiAOCjiIKNgXh4ZDFJ2W",
      "hY0GocQLiQODu83MtxnIgeaC+Q==",
    ].joined()
    let certData = try XCTUnwrap(Data(base64Encoded: certBase64))
    let certificate = try XCTUnwrap(SecCertificateCreateWithData(nil, certData as CFData))
    let policy = SecPolicyCreateSSL(true, host as CFString)
    var trust: SecTrust?
    let status = SecTrustCreateWithCertificates([certificate] as CFArray, policy, &trust)
    XCTAssertEqual(errSecSuccess, status)
    return try XCTUnwrap(trust)
  }
}

private final class StubURLProtectionSpace: URLProtectionSpace {
  private let stubServerTrust: SecTrust?

  init(
    host: String,
    port: Int,
    protocol protocolName: String?,
    realm: String?,
    authenticationMethod: String?,
    serverTrust: SecTrust?
  ) {
    self.stubServerTrust = serverTrust
    super.init(
      host: host,
      port: port,
      protocol: protocolName,
      realm: realm,
      authenticationMethod: authenticationMethod
    )
  }

  required init?(coder: NSCoder) {
    fatalError("init(coder:) has not been implemented")
  }

  override var serverTrust: SecTrust? {
    return stubServerTrust
  }

  override func copy(with zone: NSZone? = nil) -> Any {
    return self
  }
}

private final class StubAuthenticationChallengeSender: NSObject, URLAuthenticationChallengeSender {
  func use(_ credential: URLCredential, for challenge: URLAuthenticationChallenge) {}
  func continueWithoutCredential(for challenge: URLAuthenticationChallenge) {}
  func cancel(_ challenge: URLAuthenticationChallenge) {}
}

