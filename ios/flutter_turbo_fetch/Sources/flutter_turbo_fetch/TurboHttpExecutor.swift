import Flutter
import Foundation

/// URLSession request execution and in-flight task management layer responsible for
/// defensive request building, one-shot response body buffering, and error classification.
internal class TurboHttpExecutor {
  private let sessionManager: TurboSessionManager
  private let lock = NSLock()
  private var activeTasks: [String: URLSessionDataTask] = [:]

  init(sessionManager: TurboSessionManager = TurboSessionManager()) {
    self.sessionManager = sessionManager
  }

  // ---------------------------------------------------------------------------
  // Lifecycle & Task Management
  // ---------------------------------------------------------------------------

  func shutdown() {
    lock.lock()
    let tasks = Array(activeTasks.values)
    activeTasks.removeAll()
    lock.unlock()

    for task in tasks {
      sessionManager.removeTask(for: task)
      task.cancel()
    }
    sessionManager.shutdown()
  }

  func cancel(requestId: String) {
    lock.lock()
    let task = activeTasks.removeValue(forKey: requestId)
    lock.unlock()

    if let task = task {
      sessionManager.removeTask(for: task)
      task.cancel()
    }
  }

  // ---------------------------------------------------------------------------
  // Request Execution Pipeline
  // ---------------------------------------------------------------------------

  func execute(
    request: TurboHttpRequest,
    onResult: @escaping (TurboHttpResponse) -> Void
  ) {
    let urlRequest: URLRequest
    let hasRequestBody: Bool
    do {
      (urlRequest, hasRequestBody) = try buildUrlRequest(from: request)
    } catch let RequestBuildError.invalidParameter(message) {
      let errorResponse = TurboHttpResponse(
        errorCode: "connectionError",
        errorMessage: message
      )
      onResult(errorResponse)
      return
    } catch {
      let errorResponse = TurboHttpResponse(
        errorCode: "connectionError",
        errorMessage: error.localizedDescription
      )
      onResult(errorResponse)
      return
    }

    let requestedPerHost = max(1, Int(request.maxRequestsPerHost))
    let session = sessionManager.acquireSession(maxConnectionsPerHost: requestedPerHost)

    // Use completionHandler API (never URLSessionDataDelegate) so CFNetwork concatenates chunks in C/C++ layer.
    var createdTask: URLSessionDataTask?
    let task = session.dataTask(with: urlRequest) { [weak self] data, response, error in
      guard let self = self else { return }
      let (resolvedTask, taskContext) = self.cleanupTask(
        for: request.requestId,
        fallbackTask: createdTask
      )

      if let error = error {
        let (errorCode, errorMessage) = self.classifyError(
          error,
          task: resolvedTask,
          context: taskContext,
          request: request
        )
        onResult(
          TurboHttpResponse(
            errorCode: errorCode,
            errorMessage: errorMessage
          )
        )
        return
      }

      guard let httpResponse = response as? HTTPURLResponse else {
        onResult(
          TurboHttpResponse(
            errorCode: "connectionError",
            errorMessage: "Non-HTTP response received"
          )
        )
        return
      }

      onResult(
        self.buildResponse(
          httpResponse: httpResponse,
          data: data,
          maxResponseBytes: request.maxResponseBytes
        )
      )
    }

    // Register active task and delegate context synchronously before starting the task
    lock.lock()
    createdTask = task
    activeTasks[request.requestId] = task
    sessionManager.registerTask(
      task: task,
      followRedirects: request.followRedirects,
      allowBadCertificates: request.allowBadCertificates,
      hasRequestBody: hasRequestBody,
      connectTimeoutMs: request.connectTimeoutMs,
      sendTimeoutMs: request.sendTimeoutMs,
      receiveTimeoutMs: request.receiveTimeoutMs
    )
    lock.unlock()

    task.resume()
  }

  private func buildUrlRequest(from request: TurboHttpRequest) throws -> (URLRequest, Bool) {
    guard let components = URLComponents(string: request.url),
          let scheme = components.scheme?.lowercased(),
          scheme == "http" || scheme == "https",
          let host = components.host,
          !host.isEmpty,
          let url = components.url
    else {
      throw RequestBuildError.invalidParameter("Invalid request URL: \(request.url)")
    }

    var urlRequest = URLRequest(url: url)
    let method = request.method.uppercased()
    urlRequest.httpMethod = method

    let timeoutCandidates = [
      request.receiveTimeoutMs,
      request.connectTimeoutMs,
      request.sendTimeoutMs,
    ].compactMap { $0 }.filter { $0 > 0 }
    if let maxTimeoutMs = timeoutCandidates.max() {
      urlRequest.timeoutInterval = TimeInterval(maxTimeoutMs) / 1000.0
    }

    for (rawKey, rawValue) in request.headers {
      guard let key = rawKey, let value = rawValue else { continue }
      if Self.containsForbiddenHeaderCharacters(key, isName: true)
        || Self.containsForbiddenHeaderCharacters(value, isName: false)
      {
        throw RequestBuildError.invalidParameter("Invalid characters in request header.")
      }
      urlRequest.addValue(value, forHTTPHeaderField: key)
    }

    let hasRequestBody: Bool
    if method != "GET" && method != "HEAD", let bodyTypedData = request.body {
      urlRequest.httpBody = bodyTypedData.data
      hasRequestBody = !bodyTypedData.data.isEmpty
    } else {
      hasRequestBody = false
    }

    return (urlRequest, hasRequestBody)
  }

  private func buildResponse(
    httpResponse: HTTPURLResponse,
    data: Data?,
    maxResponseBytes: Int64?
  ) -> TurboHttpResponse {
    if let maxBytes = maxResponseBytes {
      let safeMaxBytes = max(0, maxBytes)
      let expectedLength = httpResponse.expectedContentLength
      if expectedLength > 0 && expectedLength > safeMaxBytes {
        return TurboHttpResponse(
          errorCode: "maxResponseBytesExceeded",
          errorMessage:
            "Response body exceeded maxResponseBytes limit (\(safeMaxBytes) bytes, contentLength: \(expectedLength))"
        )
      }

      let actualBytes = Int64(data?.count ?? 0)
      if actualBytes > safeMaxBytes {
        return TurboHttpResponse(
          errorCode: "maxResponseBytesExceeded",
          errorMessage:
            "Response body exceeded maxResponseBytes limit (\(safeMaxBytes) bytes, actual: \(actualBytes) bytes)"
        )
      }
    }

    var headersMap: [String?: Any?] = [:]
    for (rawKey, rawValue) in httpResponse.allHeaderFields {
      let key = String(describing: rawKey).lowercased()
      let value = String(describing: rawValue)
      if var existingList = headersMap[key] as? [String] {
        existingList.append(value)
        headersMap[key] = existingList
      } else {
        headersMap[key] = [value]
      }
    }

    let bodyData = FlutterStandardTypedData(bytes: data ?? Data())
    let statusCode = Int64(httpResponse.statusCode)
    let statusMessage = HTTPURLResponse.localizedString(forStatusCode: httpResponse.statusCode)

    return TurboHttpResponse(
      statusCode: statusCode,
      statusMessage: statusMessage,
      headers: headersMap,
      body: bodyData
    )
  }

  private func cleanupTask(
    for requestId: String,
    fallbackTask: URLSessionDataTask?
  ) -> (URLSessionDataTask?, TaskContext?) {
    lock.lock()
    let removedTask = activeTasks.removeValue(forKey: requestId)
    let resolvedTask = removedTask ?? fallbackTask
    lock.unlock()

    guard let task = resolvedTask else {
      return (nil, nil)
    }
    let context = sessionManager.removeTask(for: task)
    return (task, context)
  }

  // ---------------------------------------------------------------------------
  // Error Classification & Validation Helpers
  // ---------------------------------------------------------------------------

  func classifyError(
    _ error: Error,
    task: URLSessionTask?,
    context: TaskContext?,
    request: TurboHttpRequest
  ) -> (String, String) {
    if let timedOutCode = context?.timedOutErrorCode {
      return (timedOutCode, "Request timed out (\(timedOutCode))")
    }

    let nsError = error as NSError
    if nsError.domain == NSURLErrorDomain {
      switch nsError.code {
      case NSURLErrorCancelled:
        return ("cancel", nsError.localizedDescription)
      case NSURLErrorTimedOut:
        let sentBytes = task?.countOfBytesSent ?? 0
        let receivedBytes = task?.countOfBytesReceived ?? 0
        if sentBytes == 0 && receivedBytes == 0 && request.connectTimeoutMs != nil {
          return ("connectionTimeout", nsError.localizedDescription)
        }
        if let ctx = context,
           ctx.hasRequestBody && !ctx.isBodySendCompleted && request.sendTimeoutMs != nil
        {
          return ("sendTimeout", nsError.localizedDescription)
        }
        return ("receiveTimeout", nsError.localizedDescription)
      case NSURLErrorServerCertificateHasBadDate,
           NSURLErrorServerCertificateUntrusted,
           NSURLErrorServerCertificateHasUnknownRoot,
           NSURLErrorServerCertificateNotYetValid,
           NSURLErrorClientCertificateRejected,
           NSURLErrorClientCertificateRequired,
           NSURLErrorSecureConnectionFailed:
        return ("badCertificate", nsError.localizedDescription)
      case NSURLErrorNotConnectedToInternet,
           NSURLErrorCannotFindHost,
           NSURLErrorCannotConnectToHost,
           NSURLErrorNetworkConnectionLost,
           NSURLErrorDNSLookupFailed,
           NSURLErrorHTTPTooManyRedirects,
           NSURLErrorResourceUnavailable,
           NSURLErrorRedirectToNonExistentLocation,
           NSURLErrorInternationalRoamingOff,
           NSURLErrorCallIsActive,
           NSURLErrorDataNotAllowed:
        return ("connectionError", nsError.localizedDescription)
      default:
        return ("unknown", nsError.localizedDescription)
      }
    }
    return ("unknown", error.localizedDescription)
  }

  private static func containsForbiddenHeaderCharacters(_ text: String, isName: Bool) -> Bool {
    if isName && text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
      return true
    }
    for scalar in text.unicodeScalars {
      let v = scalar.value
      if v == 0x0D || v == 0x0A || v == 0x00 {
        return true
      }
      if isName && (v <= 0x20 || v == 0x3A || v >= 0x7F) {
        return true
      }
    }
    return false
  }

  private enum RequestBuildError: Error {
    case invalidParameter(String)
  }
}

