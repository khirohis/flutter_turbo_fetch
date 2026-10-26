import Flutter
import Foundation
import UIKit

public class FlutterTurboFetchPlugin: NSObject, FlutterPlugin, TurboFetchHostApi {
  private static let defaultMaxRequestsPerHost = 32

  private let lock = NSLock()
  private var activeTasks: [String: URLSessionDataTask] = [:]
  private var currentMaxConnectionsPerHost: Int = FlutterTurboFetchPlugin.defaultMaxRequestsPerHost

  private let sessionDelegate = TurboSessionTaskDelegate()
  private var sharedSession: URLSession?

  public static func register(with registrar: FlutterPluginRegistrar) {
    let instance = FlutterTurboFetchPlugin()
    TurboFetchHostApiSetup.setUp(binaryMessenger: registrar.messenger(), api: instance)
    registrar.publish(instance)
  }

  public func detachFromEngine(for registrar: FlutterPluginRegistrar) {
    TurboFetchHostApiSetup.setUp(binaryMessenger: registrar.messenger(), api: nil)
    lock.lock()
    let tasks = Array(activeTasks.values)
    activeTasks.removeAll()
    let sessionToInvalidate = sharedSession
    sharedSession = nil
    lock.unlock()

    sessionDelegate.removeAll()
    for task in tasks {
      task.cancel()
    }
    sessionToInvalidate?.invalidateAndCancel()
  }

  func executeRequest(
    request: TurboHttpRequest,
    completion: @escaping (Result<TurboHttpResponse, Error>) -> Void
  ) {
    guard let url = URL(string: request.url) else {
      let errorResponse = TurboHttpResponse(
        errorCode: "connectionError",
        errorMessage: "Invalid request URL: \(request.url)"
      )
      DispatchQueue.main.async {
        completion(.success(errorResponse))
      }
      return
    }

    var urlRequest = URLRequest(url: url)
    urlRequest.httpMethod = request.method.uppercased()

    let timeoutCandidates = [
      request.receiveTimeoutMs,
      request.connectTimeoutMs,
      request.sendTimeoutMs
    ].compactMap { $0 }.filter { $0 > 0 }
    if let maxTimeoutMs = timeoutCandidates.max() {
      urlRequest.timeoutInterval = TimeInterval(maxTimeoutMs) / 1000.0
    }

    for (key, value) in request.headers {
      if let key = key, let value = value {
        urlRequest.addValue(value, forHTTPHeaderField: key)
      }
    }

    let requestBodySize: Int64
    if let bodyTypedData = request.body {
      urlRequest.httpBody = bodyTypedData.data
      requestBodySize = Int64(bodyTypedData.data.count)
    } else {
      requestBodySize = 0
    }

    let requestedPerHost = max(1, Int(request.maxRequestsPerHost))
    let session = ensureSession(maxConnectionsPerHost: requestedPerHost)

    // Use completionHandler API (never URLSessionDataDelegate) so CFNetwork concatenates chunks in C/C++ layer.
    var createdTask: URLSessionDataTask?
    let task = session.dataTask(with: urlRequest) { [weak self] data, response, error in
      guard let self = self else { return }
      let taskContext = self.cleanupTask(
        for: request.requestId,
        taskIdentifier: createdTask?.taskIdentifier
      )

      if let error = error {
        let (errorCode, errorMessage) = self.classifyError(
          error,
          task: createdTask,
          context: taskContext,
          request: request
        )
        let turboResponse = TurboHttpResponse(
          errorCode: errorCode,
          errorMessage: errorMessage
        )
        DispatchQueue.main.async {
          completion(.success(turboResponse))
        }
        return
      }

      guard let httpResponse = response as? HTTPURLResponse else {
        let turboResponse = TurboHttpResponse(
          errorCode: "connectionError",
          errorMessage: "Non-HTTP response received"
        )
        DispatchQueue.main.async {
          completion(.success(turboResponse))
        }
        return
      }

      if let maxBytes = request.maxResponseBytes {
        let safeMaxBytes = max(0, maxBytes)
        let expectedLength = httpResponse.expectedContentLength
        if expectedLength > 0 && expectedLength > safeMaxBytes {
          let exceededResponse = TurboHttpResponse(
            errorCode: "maxResponseBytesExceeded",
            errorMessage: "Response body exceeded maxResponseBytes limit (\(safeMaxBytes) bytes, contentLength: \(expectedLength))"
          )
          DispatchQueue.main.async {
            completion(.success(exceededResponse))
          }
          return
        }

        let actualBytes = Int64(data?.count ?? 0)
        if actualBytes > safeMaxBytes {
          let exceededResponse = TurboHttpResponse(
            errorCode: "maxResponseBytesExceeded",
            errorMessage: "Response body exceeded maxResponseBytes limit (\(safeMaxBytes) bytes, actual: \(actualBytes) bytes)"
          )
          DispatchQueue.main.async {
            completion(.success(exceededResponse))
          }
          return
        }
      }

      var headersMap: [String?: Any?] = [:]
      for (rawKey, rawValue) in httpResponse.allHeaderFields {
        let key = String(describing: rawKey).lowercased()
        let value = String(describing: rawValue)
        headersMap[key] = [value]
      }

      let bodyData = FlutterStandardTypedData(bytes: data ?? Data())
      let statusCode = Int64(httpResponse.statusCode)
      let statusMessage = HTTPURLResponse.localizedString(forStatusCode: httpResponse.statusCode)

      let turboResponse = TurboHttpResponse(
        statusCode: statusCode,
        statusMessage: statusMessage,
        headers: headersMap,
        body: bodyData
      )

      DispatchQueue.main.async {
        completion(.success(turboResponse))
      }
    }
    createdTask = task

    lock.lock()
    activeTasks[request.requestId] = task
    sessionDelegate.registerTask(
      task: task,
      followRedirects: request.followRedirects,
      allowBadCertificates: request.allowBadCertificates,
      hasRequestBody: requestBodySize > 0,
      connectTimeoutMs: request.connectTimeoutMs,
      sendTimeoutMs: request.sendTimeoutMs,
      receiveTimeoutMs: request.receiveTimeoutMs
    )
    lock.unlock()

    task.resume()
  }

  func cancelRequest(requestId: String) throws {
    lock.lock()
    let task = activeTasks.removeValue(forKey: requestId)
    lock.unlock()
    if let task = task {
      sessionDelegate.removeTask(for: task.taskIdentifier)
      task.cancel()
    }
  }

  private func cleanupTask(for requestId: String, taskIdentifier: Int?) -> TaskContext? {
    lock.lock()
    let removedTask = activeTasks.removeValue(forKey: requestId)
    lock.unlock()
    let id = taskIdentifier ?? removedTask?.taskIdentifier
    if let id = id {
      return sessionDelegate.removeTask(for: id)
    }
    return nil
  }

  private func ensureSession(maxConnectionsPerHost: Int) -> URLSession {
    lock.lock()
    defer { lock.unlock() }

    if let existing = sharedSession, maxConnectionsPerHost == currentMaxConnectionsPerHost {
      return existing
    }

    if let oldSession = sharedSession {
      oldSession.finishTasksAndInvalidate()
    }

    currentMaxConnectionsPerHost = maxConnectionsPerHost
    let config = URLSessionConfiguration.default
    config.httpMaximumConnectionsPerHost = maxConnectionsPerHost
    let newSession = URLSession(configuration: config, delegate: sessionDelegate, delegateQueue: nil)
    sharedSession = newSession
    return newSession
  }

  private func classifyError(
    _ error: Error,
    task: URLSessionDataTask?,
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
        if let ctx = context, ctx.hasRequestBody && !ctx.isBodySendCompleted && request.sendTimeoutMs != nil {
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
}

private final class TaskContext {
  let followRedirects: Bool
  let allowBadCertificates: Bool
  let hasRequestBody: Bool
  let receiveTimeoutMs: Int64?

  var isBodySendCompleted: Bool
  var timedOutErrorCode: String?

  var connectTimer: DispatchWorkItem?
  var sendTimer: DispatchWorkItem?
  var receiveTimer: DispatchWorkItem?

  init(
    followRedirects: Bool,
    allowBadCertificates: Bool,
    hasRequestBody: Bool,
    receiveTimeoutMs: Int64?
  ) {
    self.followRedirects = followRedirects
    self.allowBadCertificates = allowBadCertificates
    self.hasRequestBody = hasRequestBody
    self.receiveTimeoutMs = receiveTimeoutMs
    self.isBodySendCompleted = !hasRequestBody
  }

  func cancelAllTimers() {
    connectTimer?.cancel()
    connectTimer = nil
    sendTimer?.cancel()
    sendTimer = nil
    receiveTimer?.cancel()
    receiveTimer = nil
  }
}

/// Task-level delegate for per-request redirect policy, self-signed certificate trust, and timeout phase tracking.
/// Does NOT implement URLSessionDataDelegate, preserving CFNetwork one-shot completionHandler buffering.
private final class TurboSessionTaskDelegate: NSObject, URLSessionTaskDelegate {
  private let lock = NSLock()
  private var contextsByTaskId: [Int: TaskContext] = [:]
  private let timerQueue = DispatchQueue(label: "io.github.khirohis.flutter_turbo_fetch.timers")

  func registerTask(
    task: URLSessionDataTask,
    followRedirects: Bool,
    allowBadCertificates: Bool,
    hasRequestBody: Bool,
    connectTimeoutMs: Int64?,
    sendTimeoutMs: Int64?,
    receiveTimeoutMs: Int64?
  ) {
    let taskId = task.taskIdentifier
    let context = TaskContext(
      followRedirects: followRedirects,
      allowBadCertificates: allowBadCertificates,
      hasRequestBody: hasRequestBody,
      receiveTimeoutMs: receiveTimeoutMs
    )

    if let connectMs = connectTimeoutMs, connectMs > 0 {
      let workItem = DispatchWorkItem { [weak self, weak task] in
        guard let self = self, let task = task else { return }
        self.handleConnectTimerFired(for: taskId, task: task)
      }
      context.connectTimer = workItem
      timerQueue.asyncAfter(deadline: .now() + .milliseconds(Int(connectMs)), execute: workItem)
    }

    if hasRequestBody, let sendMs = sendTimeoutMs, sendMs > 0 {
      let workItem = DispatchWorkItem { [weak self, weak task] in
        guard let self = self, let task = task else { return }
        self.handleSendTimerFired(for: taskId, task: task)
      }
      context.sendTimer = workItem
      timerQueue.asyncAfter(deadline: .now() + .milliseconds(Int(sendMs)), execute: workItem)
    } else if !hasRequestBody, let receiveMs = receiveTimeoutMs, receiveMs > 0 {
      let workItem = DispatchWorkItem { [weak self, weak task] in
        guard let self = self, let task = task else { return }
        self.handleReceiveTimerFired(for: taskId, task: task)
      }
      context.receiveTimer = workItem
      timerQueue.asyncAfter(deadline: .now() + .milliseconds(Int(receiveMs)), execute: workItem)
    }

    lock.lock()
    contextsByTaskId[taskId] = context
    lock.unlock()
  }

  @discardableResult
  func removeTask(for taskIdentifier: Int) -> TaskContext? {
    lock.lock()
    let context = contextsByTaskId.removeValue(forKey: taskIdentifier)
    lock.unlock()
    context?.cancelAllTimers()
    return context
  }

  func removeAll() {
    lock.lock()
    let allContexts = Array(contextsByTaskId.values)
    contextsByTaskId.removeAll()
    lock.unlock()
    for ctx in allContexts {
      ctx.cancelAllTimers()
    }
  }

  private func handleConnectTimerFired(for taskId: Int, task: URLSessionDataTask) {
    lock.lock()
    guard let context = contextsByTaskId[taskId], context.timedOutErrorCode == nil else {
      lock.unlock()
      return
    }
    // If no bytes have been sent or received and no response header has arrived, connection establishment timed out.
    if task.countOfBytesSent == 0 && task.countOfBytesReceived == 0 && task.response == nil {
      context.timedOutErrorCode = "connectionTimeout"
      context.cancelAllTimers()
      lock.unlock()
      task.cancel()
    } else {
      context.connectTimer = nil
      lock.unlock()
    }
  }

  private func handleSendTimerFired(for taskId: Int, task: URLSessionDataTask) {
    lock.lock()
    guard let context = contextsByTaskId[taskId],
          context.timedOutErrorCode == nil,
          !context.isBodySendCompleted else {
      lock.unlock()
      return
    }
    context.timedOutErrorCode = "sendTimeout"
    context.cancelAllTimers()
    lock.unlock()
    task.cancel()
  }

  private func handleReceiveTimerFired(for taskId: Int, task: URLSessionDataTask) {
    lock.lock()
    guard let context = contextsByTaskId[taskId], context.timedOutErrorCode == nil else {
      lock.unlock()
      return
    }
    context.timedOutErrorCode = "receiveTimeout"
    context.cancelAllTimers()
    lock.unlock()
    task.cancel()
  }

  func urlSession(
    _ session: URLSession,
    task: URLSessionTask,
    didSendBodyData bytesSent: Int64,
    totalBytesSent: Int64,
    totalBytesExpectedToSend: Int64
  ) {
    let taskId = task.taskIdentifier
    lock.lock()
    guard let context = contextsByTaskId[taskId] else {
      lock.unlock()
      return
    }
    // Connection is definitely established once body bytes are being sent.
    context.connectTimer?.cancel()
    context.connectTimer = nil

    if totalBytesExpectedToSend > 0 && totalBytesSent >= totalBytesExpectedToSend && !context.isBodySendCompleted {
      context.isBodySendCompleted = true
      context.sendTimer?.cancel()
      context.sendTimer = nil

      if let receiveMs = context.receiveTimeoutMs, receiveMs > 0, context.receiveTimer == nil {
        let workItem = DispatchWorkItem { [weak self, weak task] in
          guard let self = self, let dataTask = task as? URLSessionDataTask else { return }
          self.handleReceiveTimerFired(for: taskId, task: dataTask)
        }
        context.receiveTimer = workItem
        timerQueue.asyncAfter(deadline: .now() + .milliseconds(Int(receiveMs)), execute: workItem)
      }
    }
    lock.unlock()
  }

  func urlSession(
    _ session: URLSession,
    task: URLSessionTask,
    willPerformHTTPRedirection response: HTTPURLResponse,
    newRequest request: URLRequest,
    completionHandler: @escaping (URLRequest?) -> Void
  ) {
    lock.lock()
    let followRedirects = contextsByTaskId[task.taskIdentifier]?.followRedirects ?? true
    lock.unlock()
    if !followRedirects {
      completionHandler(nil)
    } else {
      completionHandler(request)
    }
  }

  func urlSession(
    _ session: URLSession,
    task: URLSessionTask,
    didReceive challenge: URLAuthenticationChallenge,
    completionHandler: @escaping (URLSession.AuthChallengeDisposition, URLCredential?) -> Void
  ) {
    lock.lock()
    let allowBadCertificates = contextsByTaskId[task.taskIdentifier]?.allowBadCertificates ?? false
    lock.unlock()
    if allowBadCertificates,
       challenge.protectionSpace.authenticationMethod == NSURLAuthenticationMethodServerTrust,
       let serverTrust = challenge.protectionSpace.serverTrust {
      completionHandler(.useCredential, URLCredential(trust: serverTrust))
      return
    }
    completionHandler(.performDefaultHandling, nil)
  }
}
