#if os(iOS)
  import Flutter
#elseif os(macOS)
  import FlutterMacOS
#endif
import Foundation
import XCTest

@testable import flutter_turbo_fetch

final class FlutterTurboFetchPluginTest: XCTestCase {
  private func createRequest(
    requestId: String = "req-1",
    maxRequestsPerHost: Int64 = 32
  ) -> TurboHttpRequest {
    return TurboHttpRequest(
      requestId: requestId,
      url: "https://example.com/data",
      method: "GET",
      headers: [:],
      body: nil,
      connectTimeoutMs: 5000,
      receiveTimeoutMs: 5000,
      sendTimeoutMs: 5000,
      followRedirects: true,
      maxRequestsPerHost: maxRequestsPerHost,
      allowBadCertificates: false,
      maxResponseBytes: nil
    )
  }

  func testPluginInstance_canBeCreated() {
    let plugin = FlutterTurboFetchPlugin()
    XCTAssertNotNil(plugin)
  }

  func testExecuteRequest_andCancelRequest_delegateToExecutor_andInvokeCallbackExactlyOnce() throws {
    final class FakeTurboHttpExecutor: TurboHttpExecutor {
      var capturedRequest: TurboHttpRequest?
      var capturedOnResult: ((TurboHttpResponse) -> Void)?
      var canceledRequestId: String?

      override func execute(
        request: TurboHttpRequest,
        onResult: @escaping (TurboHttpResponse) -> Void
      ) {
        capturedRequest = request
        capturedOnResult = onResult
      }

      override func cancel(requestId: String) {
        canceledRequestId = requestId
      }
    }

    let fakeExecutor = FakeTurboHttpExecutor()
    let plugin = FlutterTurboFetchPlugin(executor: fakeExecutor)

    let completionExp = expectation(description: "Plugin completion callback")
    var receivedResponse: TurboHttpResponse?
    var callbackCount = 0

    let request = createRequest(requestId: "req-cancel-test")
    plugin.executeRequest(request: request) { result in
      callbackCount += 1
      if case .success(let response) = result {
        receivedResponse = response
      }
      completionExp.fulfill()
    }

    XCTAssertEqual("req-cancel-test", fakeExecutor.capturedRequest?.requestId)
    XCTAssertNil(receivedResponse)
    XCTAssertEqual(0, callbackCount)

    // cancelRequest delegates to executor.cancel(requestId:)
    try plugin.cancelRequest(requestId: "req-cancel-test")
    XCTAssertEqual("req-cancel-test", fakeExecutor.canceledRequestId)

    // When executor finishes and invokes onResult, completion receives the response on main thread
    let expectedResponse = TurboHttpResponse(
      errorCode: "cancel",
      errorMessage: "Canceled"
    )
    fakeExecutor.capturedOnResult?(expectedResponse)

    wait(for: [completionExp], timeout: 5.0)
    XCTAssertEqual("cancel", receivedResponse?.errorCode)
    XCTAssertEqual(1, callbackCount, "Plugin completion must be invoked exactly once")
  }

  func testPostToMain_executesImmediatelyOnMainThread() {
    let plugin = FlutterTurboFetchPlugin()
    var executed = false
    plugin.postToMain {
      executed = true
    }
    XCTAssertTrue(executed, "postToMain should execute synchronously when already on main thread")
  }

  func testDetachFromEngine_delegatesShutdownToExecutor() {
    final class LifecycleTrackingExecutor: TurboHttpExecutor {
      var shutdownCalled = false

      override func shutdown() {
        shutdownCalled = true
      }
    }

    let fakeExecutor = LifecycleTrackingExecutor()
    let plugin = FlutterTurboFetchPlugin(executor: fakeExecutor)

    fakeExecutor.shutdown()
    XCTAssertTrue(fakeExecutor.shutdownCalled, "Executor shutdown must be invoked")
  }
}

