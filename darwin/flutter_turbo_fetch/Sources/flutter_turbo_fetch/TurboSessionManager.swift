import Foundation

/// Per-task execution context tracking redirect policy, certificate trust, and timeout phases.
internal final class TaskContext {
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

/// Shared URLSession and connection pool manager responsible for thread-safe connection scaling,
/// post-shutdown recovery, and per-task delegate policy management.
internal final class TurboSessionManager {
  static let defaultMaxRequestsPerHost = 32

  private let lock = NSLock()
  private var sharedSession: URLSession?
  private var _currentMaxConnectionsPerHost: Int = TurboSessionManager.defaultMaxRequestsPerHost
  private let sessionDelegate = TurboSessionTaskDelegate()
  private let sessionFactory: (Int, URLSessionTaskDelegate) -> URLSession

  init(
    sessionFactory: @escaping (Int, URLSessionTaskDelegate) -> URLSession =
      TurboSessionManager.createDefaultSession
  ) {
    self.sessionFactory = sessionFactory
  }

  var currentMaxConnectionsPerHost: Int {
    lock.lock()
    defer { lock.unlock() }
    return _currentMaxConnectionsPerHost
  }

  var isShutdown: Bool {
    lock.lock()
    defer { lock.unlock() }
    return sharedSession == nil
  }

  /// Acquires the shared URLSession configured with the requested maximum connections per host.
  /// Automatically recreates the session if the limit changes or if previously shut down.
  func acquireSession(maxConnectionsPerHost: Int) -> URLSession {
    let safeMax = max(1, maxConnectionsPerHost)
    lock.lock()
    defer { lock.unlock() }

    if let existing = sharedSession, safeMax == _currentMaxConnectionsPerHost {
      return existing
    }

    if let oldSession = sharedSession {
      oldSession.finishTasksAndInvalidate()
    }

    _currentMaxConnectionsPerHost = safeMax
    let newSession = sessionFactory(safeMax, sessionDelegate)
    sharedSession = newSession
    return newSession
  }

  /// Registers a task context and starts phase-specific timeout timers.
  func registerTask(
    task: URLSessionTask,
    followRedirects: Bool,
    allowBadCertificates: Bool,
    hasRequestBody: Bool,
    connectTimeoutMs: Int64?,
    sendTimeoutMs: Int64?,
    receiveTimeoutMs: Int64?
  ) {
    sessionDelegate.registerTask(
      task: task,
      followRedirects: followRedirects,
      allowBadCertificates: allowBadCertificates,
      hasRequestBody: hasRequestBody,
      connectTimeoutMs: connectTimeoutMs,
      sendTimeoutMs: sendTimeoutMs,
      receiveTimeoutMs: receiveTimeoutMs
    )
  }

  /// Removes and returns the task context for the specified task, cancelling all associated timers.
  @discardableResult
  func removeTask(for task: URLSessionTask) -> TaskContext? {
    return sessionDelegate.removeTask(for: task)
  }

  /// Cancels all tracked task timers and invalidates the shared URLSession.
  func shutdown() {
    lock.lock()
    let sessionToInvalidate = sharedSession
    sharedSession = nil
    lock.unlock()

    sessionDelegate.removeAll()
    sessionToInvalidate?.invalidateAndCancel()
  }

  private static func createDefaultSession(
    maxConnectionsPerHost: Int,
    delegate: URLSessionTaskDelegate
  ) -> URLSession {
    let config = URLSessionConfiguration.default
    config.httpMaximumConnectionsPerHost = maxConnectionsPerHost
    return URLSession(configuration: config, delegate: delegate, delegateQueue: nil)
  }
}

/// Task-level delegate for per-request redirect policy, self-signed certificate trust, and timeout phase tracking.
/// Uses `ObjectIdentifier(task)` as the lookup key so tasks from different URLSession instances never collide.
/// Does NOT implement URLSessionDataDelegate, preserving CFNetwork one-shot completionHandler buffering.
internal final class TurboSessionTaskDelegate: NSObject, URLSessionTaskDelegate {
  private let lock = NSLock()
  private var contextsByTaskKey: [ObjectIdentifier: TaskContext] = [:]
  private let timerQueue = DispatchQueue(label: "io.github.khirohis.flutter_turbo_fetch.timers")

  func registerTask(
    task: URLSessionTask,
    followRedirects: Bool,
    allowBadCertificates: Bool,
    hasRequestBody: Bool,
    connectTimeoutMs: Int64?,
    sendTimeoutMs: Int64?,
    receiveTimeoutMs: Int64?
  ) {
    let taskKey = ObjectIdentifier(task)
    let context = TaskContext(
      followRedirects: followRedirects,
      allowBadCertificates: allowBadCertificates,
      hasRequestBody: hasRequestBody,
      receiveTimeoutMs: receiveTimeoutMs
    )

    if let connectMs = connectTimeoutMs, connectMs > 0 {
      let workItem = DispatchWorkItem { [weak self, weak task] in
        guard let self = self, let task = task else { return }
        self.handleConnectTimerFired(for: taskKey, task: task)
      }
      context.connectTimer = workItem
      timerQueue.asyncAfter(deadline: .now() + .milliseconds(Int(connectMs)), execute: workItem)
    }

    if hasRequestBody, let sendMs = sendTimeoutMs, sendMs > 0 {
      let workItem = DispatchWorkItem { [weak self, weak task] in
        guard let self = self, let task = task else { return }
        self.handleSendTimerFired(for: taskKey, task: task)
      }
      context.sendTimer = workItem
      timerQueue.asyncAfter(deadline: .now() + .milliseconds(Int(sendMs)), execute: workItem)
    } else if !hasRequestBody, let receiveMs = receiveTimeoutMs, receiveMs > 0 {
      let workItem = DispatchWorkItem { [weak self, weak task] in
        guard let self = self, let task = task else { return }
        self.handleReceiveTimerFired(for: taskKey, task: task)
      }
      context.receiveTimer = workItem
      timerQueue.asyncAfter(deadline: .now() + .milliseconds(Int(receiveMs)), execute: workItem)
    }

    lock.lock()
    contextsByTaskKey[taskKey] = context
    lock.unlock()
  }

  @discardableResult
  func removeTask(for task: URLSessionTask) -> TaskContext? {
    let taskKey = ObjectIdentifier(task)
    lock.lock()
    let context = contextsByTaskKey.removeValue(forKey: taskKey)
    lock.unlock()
    context?.cancelAllTimers()
    return context
  }

  func removeAll() {
    lock.lock()
    let allContexts = Array(contextsByTaskKey.values)
    contextsByTaskKey.removeAll()
    lock.unlock()
    for ctx in allContexts {
      ctx.cancelAllTimers()
    }
  }

  private func handleConnectTimerFired(for taskKey: ObjectIdentifier, task: URLSessionTask) {
    lock.lock()
    guard let context = contextsByTaskKey[taskKey], context.timedOutErrorCode == nil else {
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

  private func handleSendTimerFired(for taskKey: ObjectIdentifier, task: URLSessionTask) {
    lock.lock()
    guard let context = contextsByTaskKey[taskKey],
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

  private func handleReceiveTimerFired(for taskKey: ObjectIdentifier, task: URLSessionTask) {
    lock.lock()
    guard let context = contextsByTaskKey[taskKey], context.timedOutErrorCode == nil else {
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
    let taskKey = ObjectIdentifier(task)
    lock.lock()
    guard let context = contextsByTaskKey[taskKey] else {
      lock.unlock()
      return
    }
    // Connection is established once body bytes are being sent.
    context.connectTimer?.cancel()
    context.connectTimer = nil

    if totalBytesExpectedToSend > 0
      && totalBytesSent >= totalBytesExpectedToSend
      && !context.isBodySendCompleted
    {
      context.isBodySendCompleted = true
      context.sendTimer?.cancel()
      context.sendTimer = nil

      if let receiveMs = context.receiveTimeoutMs, receiveMs > 0, context.receiveTimer == nil {
        let workItem = DispatchWorkItem { [weak self, weak task] in
          guard let self = self, let task = task else { return }
          self.handleReceiveTimerFired(for: taskKey, task: task)
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
    let taskKey = ObjectIdentifier(task)
    lock.lock()
    let followRedirects = contextsByTaskKey[taskKey]?.followRedirects ?? true
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
    let taskKey = ObjectIdentifier(task)
    lock.lock()
    let allowBadCertificates = contextsByTaskKey[taskKey]?.allowBadCertificates ?? false
    lock.unlock()
    if allowBadCertificates,
       challenge.protectionSpace.authenticationMethod == NSURLAuthenticationMethodServerTrust,
       let serverTrust = challenge.protectionSpace.serverTrust
    {
      completionHandler(.useCredential, URLCredential(trust: serverTrust))
      return
    }
    completionHandler(.performDefaultHandling, nil)
  }
}

