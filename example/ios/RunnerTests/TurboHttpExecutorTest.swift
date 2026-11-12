#if os(iOS)
  import Flutter
#elseif os(macOS)
  import FlutterMacOS
#endif
import Foundation
import XCTest

@testable import flutter_turbo_fetch

final class TurboHttpExecutorTest: XCTestCase {
  override func tearDown() {
    MockURLProtocol.requestHandler = nil
    super.tearDown()
  }

  private func createRequest(
    requestId: String = "test-req",
    url: String = "https://example.com/api",
    method: String = "GET",
    headers: [String?: String?] = [:],
    body: Data? = nil,
    connectTimeoutMs: Int64? = 5000,
    receiveTimeoutMs: Int64? = 5000,
    sendTimeoutMs: Int64? = 5000,
    maxResponseBytes: Int64? = nil
  ) -> TurboHttpRequest {
    return TurboHttpRequest(
      requestId: requestId,
      url: url,
      method: method,
      headers: headers,
      body: body.map { FlutterStandardTypedData(bytes: $0) },
      connectTimeoutMs: connectTimeoutMs,
      receiveTimeoutMs: receiveTimeoutMs,
      sendTimeoutMs: sendTimeoutMs,
      followRedirects: true,
      maxRequestsPerHost: 32,
      allowBadCertificates: false,
      maxResponseBytes: maxResponseBytes
    )
  }

  private func createMockedExecutor(
    handler: @escaping (URLRequest) throws -> (URLResponse, Data?)
  ) -> TurboHttpExecutor {
    MockURLProtocol.requestHandler = handler
    let sessionManager = TurboSessionManager { maxPerHost, delegate in
      let config = URLSessionConfiguration.ephemeral
      config.httpMaximumConnectionsPerHost = maxPerHost
      config.protocolClasses = [MockURLProtocol.self]
      return URLSession(configuration: config, delegate: delegate, delegateQueue: nil)
    }
    return TurboHttpExecutor(sessionManager: sessionManager)
  }

  private func executeSync(
    executor: TurboHttpExecutor,
    request: TurboHttpRequest
  ) -> TurboHttpResponse {
    let exp = expectation(description: "TurboHttpExecutor.execute completion")
    var capturedResponse: TurboHttpResponse?
    var callCount = 0
    executor.execute(request: request) { response in
      callCount += 1
      capturedResponse = response
      exp.fulfill()
    }
    wait(for: [exp], timeout: 5.0)
    XCTAssertEqual(1, callCount, "Callback must be invoked exactly once")
    XCTAssertEqual(0, executor.activeTaskCount, "activeTasks must be empty after execution")
    XCTAssertFalse(executor.hasActiveTask(requestId: request.requestId), "activeTasks must not retain finished request")
    return capturedResponse!
  }

  func testClassifyError_classifiesCanceledRequest() {
    let executor = TurboHttpExecutor()
    let cancelError = NSError(domain: NSURLErrorDomain, code: NSURLErrorCancelled)
    let (code, _) = executor.classifyError(
      cancelError,
      task: nil,
      context: nil,
      request: createRequest()
    )
    XCTAssertEqual("cancel", code)
  }

  func testClassifyError_classifiesTimeoutsCorrectly() {
    let executor = TurboHttpExecutor()
    let nsTimedOut = NSError(domain: NSURLErrorDomain, code: NSURLErrorTimedOut)

    // 1. Explicit phase timer fired in TaskContext takes precedence
    let connectCtx = TaskContext(
      followRedirects: true,
      allowBadCertificates: false,
      hasRequestBody: false,
      receiveTimeoutMs: 5000
    )
    connectCtx.timedOutErrorCode = "connectionTimeout"
    XCTAssertEqual(
      "connectionTimeout",
      executor.classifyError(
        NSError(domain: NSURLErrorDomain, code: NSURLErrorCancelled),
        task: nil,
        context: connectCtx,
        request: createRequest()
      ).0
    )

    // 2. OS NSURLErrorTimedOut with 0 bytes sent/received and connectTimeoutMs set -> connectionTimeout
    XCTAssertEqual(
      "connectionTimeout",
      executor.classifyError(
        nsTimedOut,
        task: nil,
        context: nil,
        request: createRequest(connectTimeoutMs: 3000)
      ).0
    )

    // 3. OS NSURLErrorTimedOut while body send is incomplete -> sendTimeout
    let sendingCtx = TaskContext(
      followRedirects: true,
      allowBadCertificates: false,
      hasRequestBody: true,
      receiveTimeoutMs: 5000
    )
    XCTAssertFalse(sendingCtx.isBodySendCompleted)
    XCTAssertEqual(
      "sendTimeout",
      executor.classifyError(
        nsTimedOut,
        task: nil,
        context: sendingCtx,
        request: createRequest(connectTimeoutMs: nil, sendTimeoutMs: 3000)
      ).0
    )

    // 4. OS NSURLErrorTimedOut after body send completed -> receiveTimeout
    sendingCtx.isBodySendCompleted = true
    XCTAssertEqual(
      "receiveTimeout",
      executor.classifyError(
        nsTimedOut,
        task: nil,
        context: sendingCtx,
        request: createRequest(connectTimeoutMs: nil, sendTimeoutMs: 3000)
      ).0
    )
  }

  func testClassifyError_classifiesCertificateAndConnectionErrors() {
    let executor = TurboHttpExecutor()
    let req = createRequest()

    let certErrorCodes = [
      NSURLErrorServerCertificateHasBadDate,
      NSURLErrorServerCertificateUntrusted,
      NSURLErrorServerCertificateHasUnknownRoot,
      NSURLErrorServerCertificateNotYetValid,
      NSURLErrorClientCertificateRejected,
      NSURLErrorClientCertificateRequired,
      NSURLErrorSecureConnectionFailed,
    ]
    for errCode in certErrorCodes {
      let err = NSError(domain: NSURLErrorDomain, code: errCode)
      XCTAssertEqual(
        "badCertificate",
        executor.classifyError(err, task: nil, context: nil, request: req).0
      )
    }

    let connectionErrorCodes = [
      NSURLErrorNotConnectedToInternet,
      NSURLErrorCannotFindHost,
      NSURLErrorCannotConnectToHost,
      NSURLErrorNetworkConnectionLost,
      NSURLErrorDNSLookupFailed,
      NSURLErrorHTTPTooManyRedirects,
      NSURLErrorResourceUnavailable,
      NSURLErrorRedirectToNonExistentLocation,
      NSURLErrorInternationalRoamingOff,
      NSURLErrorCallIsActive,
      NSURLErrorDataNotAllowed,
    ]
    for errCode in connectionErrorCodes {
      let err = NSError(domain: NSURLErrorDomain, code: errCode)
      XCTAssertEqual(
        "connectionError",
        executor.classifyError(err, task: nil, context: nil, request: req).0
      )
    }

    let unknownUrlError = NSError(domain: NSURLErrorDomain, code: NSURLErrorCannotDecodeRawData)
    XCTAssertEqual(
      "unknown",
      executor.classifyError(unknownUrlError, task: nil, context: nil, request: req).0
    )

    let customDomainError = NSError(domain: "CustomDomain", code: 999)
    XCTAssertEqual(
      "unknown",
      executor.classifyError(customDomainError, task: nil, context: nil, request: req).0
    )
  }

  func testExecute_invalidUrl_returnsConnectionError() {
    let executor = TurboHttpExecutor()
    defer { executor.shutdown() }

    // Malformed URL
    let malformedResponse = executeSync(
      executor: executor,
      request: createRequest(url: "http://[::1")
    )
    XCTAssertEqual("connectionError", malformedResponse.errorCode)
    XCTAssertNotNil(malformedResponse.errorMessage)

    // Unsupported scheme
    let ftpResponse = executeSync(
      executor: executor,
      request: createRequest(url: "ftp://example.com/file")
    )
    XCTAssertEqual("connectionError", ftpResponse.errorCode)

    // Missing host
    let emptyHostResponse = executeSync(
      executor: executor,
      request: createRequest(url: "https://")
    )
    XCTAssertEqual("connectionError", emptyHostResponse.errorCode)
  }

  func testExecute_invalidHeaderCharacters_returnsConnectionErrorSafelyWithoutCrash() {
    let executor = TurboHttpExecutor()
    defer { executor.shutdown() }

    let crlfHeaders: [String?: String?] = [
      "X-Invalid\r\nHeader": "value\r\ninjection"
    ]
    let crlfResponse = executeSync(
      executor: executor,
      request: createRequest(headers: crlfHeaders)
    )
    XCTAssertEqual("connectionError", crlfResponse.errorCode)

    let emptyKeyHeaders: [String?: String?] = [
      "   ": "value"
    ]
    let emptyKeyResponse = executeSync(
      executor: executor,
      request: createRequest(headers: emptyKeyHeaders)
    )
    XCTAssertEqual("connectionError", emptyKeyResponse.errorCode)
  }

  func testCancel_cancelsInFlightRequest() {
    let requestStarted = expectation(description: "Request started in URLProtocol")
    let resultReceived = expectation(description: "Cancel result received")

    let executor = createMockedExecutor { request in
      requestStarted.fulfill()
      Thread.sleep(forTimeInterval: 5.0)
      let response = HTTPURLResponse(
        url: request.url!,
        statusCode: 200,
        httpVersion: "HTTP/2.0",
        headerFields: nil
      )!
      return (response, Data("ok".utf8))
    }
    defer { executor.shutdown() }

    var capturedResponse: TurboHttpResponse?
    var callCount = 0
    executor.execute(request: createRequest(requestId: "cancel-target")) { response in
      callCount += 1
      capturedResponse = response
      resultReceived.fulfill()
    }

    wait(for: [requestStarted], timeout: 5.0)
    executor.cancel(requestId: "cancel-target")

    wait(for: [resultReceived], timeout: 5.0)
    XCTAssertEqual("cancel", capturedResponse?.errorCode)
    XCTAssertEqual(1, callCount, "Callback must be invoked exactly once on cancel")
    XCTAssertEqual(0, executor.activeTaskCount, "activeTasks must be cleared after cancellation")
    XCTAssertFalse(executor.hasActiveTask(requestId: "cancel-target"))
  }

  func testCancel_immediatelyAfterExecute_cancelsSafelyAndInvokesCallbackExactlyOnce() {
    let allowFinish = expectation(description: "Allow URLProtocol to finish")
    let resultReceived = expectation(description: "Immediate cancel result received")

    let executor = createMockedExecutor { request in
      _ = XCTWaiter.wait(for: [allowFinish], timeout: 5.0)
      let response = HTTPURLResponse(
        url: request.url!,
        statusCode: 200,
        httpVersion: "HTTP/2.0",
        headerFields: nil
      )!
      return (response, Data("ok".utf8))
    }
    defer { executor.shutdown() }

    var capturedResponse: TurboHttpResponse?
    var callCount = 0

    executor.execute(request: createRequest(requestId: "race-cancel")) { response in
      callCount += 1
      capturedResponse = response
      resultReceived.fulfill()
    }

    // Cancel immediately right after execute (testing race before/during request dispatch)
    executor.cancel(requestId: "race-cancel")
    allowFinish.fulfill()

    wait(for: [resultReceived], timeout: 5.0)
    XCTAssertEqual("cancel", capturedResponse?.errorCode)
    XCTAssertEqual(1, callCount, "Callback must be invoked exactly once under immediate cancellation")
    XCTAssertEqual(0, executor.activeTaskCount)
    XCTAssertFalse(executor.hasActiveTask(requestId: "race-cancel"))
  }

  func testShutdown_cancelsInFlightRequestsAndCleansUpActiveTasks() {
    let requestStarted = expectation(description: "Request started in URLProtocol")
    let allowFinish = expectation(description: "Allow URLProtocol to finish")
    let resultReceived = expectation(description: "Shutdown cancel result received")

    let executor = createMockedExecutor { request in
      requestStarted.fulfill()
      _ = XCTWaiter.wait(for: [allowFinish], timeout: 5.0)
      let response = HTTPURLResponse(
        url: request.url!,
        statusCode: 200,
        httpVersion: "HTTP/2.0",
        headerFields: nil
      )!
      return (response, Data("ok".utf8))
    }

    var capturedResponse: TurboHttpResponse?
    var callCount = 0

    executor.execute(request: createRequest(requestId: "shutdown-target")) { response in
      callCount += 1
      capturedResponse = response
      resultReceived.fulfill()
    }

    wait(for: [requestStarted], timeout: 5.0)
    // Shutdown cancels all active tasks and cleans up state
    executor.shutdown()
    allowFinish.fulfill()

    wait(for: [resultReceived], timeout: 5.0)
    XCTAssertEqual("cancel", capturedResponse?.errorCode)
    XCTAssertEqual(1, callCount, "Callback must be invoked exactly once on shutdown cancellation")
    XCTAssertEqual(0, executor.activeTaskCount)
    XCTAssertFalse(executor.hasActiveTask(requestId: "shutdown-target"))
  }

  func testExecute_maxResponseBytesNil_returnsFullBodyAndHeaders() {
    let expectedBytes = Data("turbo-fetch-payload".utf8)

    let executor = createMockedExecutor { request in
      let response = HTTPURLResponse(
        url: request.url!,
        statusCode: 201,
        httpVersion: "HTTP/2.0",
        headerFields: ["X-Custom": "v1"]
      )!
      return (response, expectedBytes)
    }
    defer { executor.shutdown() }

    let response = executeSync(
      executor: executor,
      request: createRequest(maxResponseBytes: nil)
    )
    XCTAssertNil(response.errorCode)
    XCTAssertEqual(201, response.statusCode)
    XCTAssertEqual(expectedBytes, response.body?.data)
    XCTAssertEqual(["v1"], response.headers?["x-custom"] as? [String])
  }

  func testExecute_maxResponseBytesExceededByContentLength_returnsExceededError() {
    let payload = Data(repeating: 1, count: 100)

    let executor = createMockedExecutor { request in
      let response = HTTPURLResponse(
        url: request.url!,
        statusCode: 200,
        httpVersion: "HTTP/2.0",
        headerFields: ["Content-Length": "100"]
      )!
      return (response, payload)
    }
    defer { executor.shutdown() }

    let response = executeSync(
      executor: executor,
      request: createRequest(maxResponseBytes: 50)
    )
    XCTAssertEqual("maxResponseBytesExceeded", response.errorCode)
    XCTAssertNil(response.body)
  }

  func testExecute_maxResponseBytesExceededWhenContentLengthUnknown_returnsExceededError() {
    let payload = Data(repeating: 2, count: 64)

    let executor = createMockedExecutor { request in
      // No Content-Length header -> expectedContentLength is -1
      let response = HTTPURLResponse(
        url: request.url!,
        statusCode: 200,
        httpVersion: "HTTP/2.0",
        headerFields: nil
      )!
      return (response, payload)
    }
    defer { executor.shutdown() }

    let response = executeSync(
      executor: executor,
      request: createRequest(maxResponseBytes: 32)
    )
    XCTAssertEqual("maxResponseBytesExceeded", response.errorCode)
    XCTAssertNil(response.body)
  }

  func testExecute_maxResponseBytesExactBoundary_succeeds() {
    let payload = Data(repeating: 3, count: 32)

    let executor = createMockedExecutor { request in
      let response = HTTPURLResponse(
        url: request.url!,
        statusCode: 200,
        httpVersion: "HTTP/2.0",
        headerFields: ["Content-Length": "32"]
      )!
      return (response, payload)
    }
    defer { executor.shutdown() }

    let response = executeSync(
      executor: executor,
      request: createRequest(maxResponseBytes: 32)
    )
    XCTAssertNil(response.errorCode)
    XCTAssertEqual(payload, response.body?.data)
  }

  func testExecute_maxResponseBytesInt64Max_returnsFullBodyWithoutTruncation() {
    let payload = Data("full-body-at-int64-max-value".utf8)

    let executor = createMockedExecutor { request in
      let response = HTTPURLResponse(
        url: request.url!,
        statusCode: 200,
        httpVersion: "HTTP/2.0",
        headerFields: nil
      )!
      return (response, payload)
    }
    defer { executor.shutdown() }

    let response = executeSync(
      executor: executor,
      request: createRequest(maxResponseBytes: Int64.max)
    )
    XCTAssertNil(response.errorCode)
    XCTAssertEqual(payload, response.body?.data)
  }

  func testExecute_postWithRequestBody_transmitsPayloadCorrectly() {
    let requestPayload = Data("turbo-post-payload".utf8)
    var capturedPayload: Data?

    let executor = createMockedExecutor { request in
      XCTAssertEqual("POST", request.httpMethod)
      capturedPayload = MockURLProtocol.extractBodyData(from: request)

      let response = HTTPURLResponse(
        url: request.url!,
        statusCode: 200,
        httpVersion: "HTTP/2.0",
        headerFields: nil
      )!
      return (response, Data("response-ok".utf8))
    }
    defer { executor.shutdown() }

    let response = executeSync(
      executor: executor,
      request: createRequest(method: "POST", body: requestPayload)
    )

    XCTAssertNil(response.errorCode)
    XCTAssertEqual(200, response.statusCode)
    XCTAssertEqual(requestPayload, capturedPayload)
  }

  func testExecute_postWithNullBody_fallsBackSafely() {
    var capturedBodyLength: Int?

    let executor = createMockedExecutor { request in
      XCTAssertEqual("POST", request.httpMethod)
      let bodyData = MockURLProtocol.extractBodyData(from: request)
      capturedBodyLength = bodyData?.count ?? 0

      let response = HTTPURLResponse(
        url: request.url!,
        statusCode: 204,
        httpVersion: "HTTP/2.0",
        headerFields: nil
      )!
      return (response, nil)
    }
    defer { executor.shutdown() }

    let response = executeSync(
      executor: executor,
      request: createRequest(method: "POST", body: nil)
    )

    XCTAssertNil(response.errorCode)
    XCTAssertEqual(204, response.statusCode)
    XCTAssertEqual(0, capturedBodyLength)
    XCTAssertEqual(Data(), response.body?.data)
  }

  func testExecute_emptyResponseBody_returnsEmptyByteArraySafely() {
    let executor = createMockedExecutor { request in
      let response = HTTPURLResponse(
        url: request.url!,
        statusCode: 200,
        httpVersion: "HTTP/2.0",
        headerFields: nil
      )!
      return (response, Data())
    }
    defer { executor.shutdown() }

    let response = executeSync(executor: executor, request: createRequest())
    XCTAssertNil(response.errorCode)
    XCTAssertEqual(200, response.statusCode)
    XCTAssertEqual(Data(), response.body?.data)
  }
}

private final class MockURLProtocol: URLProtocol {
  static var requestHandler: ((URLRequest) throws -> (URLResponse, Data?))?

  static func extractBodyData(from request: URLRequest) -> Data? {
    if let body = request.httpBody {
      return body
    }
    guard let stream = request.httpBodyStream else { return nil }
    stream.open()
    defer { stream.close() }
    var buffer = [UInt8](repeating: 0, count: 1024)
    var data = Data()
    while stream.hasBytesAvailable {
      let read = stream.read(&buffer, maxLength: buffer.count)
      if read > 0 {
        data.append(buffer, count: read)
      } else {
        break
      }
    }
    return data
  }

  override class func canInit(with request: URLRequest) -> Bool {
    return true
  }

  override class func canonicalRequest(for request: URLRequest) -> URLRequest {
    return request
  }

  override func startLoading() {
    guard let handler = MockURLProtocol.requestHandler else {
      client?.urlProtocol(
        self,
        didFailWithError: NSError(domain: NSURLErrorDomain, code: NSURLErrorUnknown)
      )
      return
    }

    DispatchQueue.global().async { [weak self] in
      guard let self = self else { return }
      do {
        let (response, data) = try handler(self.request)
        self.client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
        if let data = data {
          self.client?.urlProtocol(self, didLoad: data)
        }
        self.client?.urlProtocolDidFinishLoading(self)
      } catch {
        self.client?.urlProtocol(self, didFailWithError: error)
      }
    }
  }

  override func stopLoading() {
    // No-op when cancelled
  }
}

