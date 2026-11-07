import Flutter
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
    executor.execute(request: request) { response in
      capturedResponse = response
      exp.fulfill()
    }
    wait(for: [exp], timeout: 5.0)
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
    executor.execute(request: createRequest(requestId: "cancel-target")) { response in
      capturedResponse = response
      resultReceived.fulfill()
    }

    wait(for: [requestStarted], timeout: 5.0)
    executor.cancel(requestId: "cancel-target")

    wait(for: [resultReceived], timeout: 5.0)
    XCTAssertEqual("cancel", capturedResponse?.errorCode)
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
}

private final class MockURLProtocol: URLProtocol {
  static var requestHandler: ((URLRequest) throws -> (URLResponse, Data?))?

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

