import Flutter
import Foundation
import UIKit

public class FlutterTurboFetchPlugin: NSObject, FlutterPlugin, TurboFetchHostApi {
  private static let defaultMaxRequestsPerHost = 32

  private let lock = NSLock()
  private var activeTasks: [String: URLSessionDataTask] = [:]
  private var currentMaxConnectionsPerHost: Int = FlutterTurboFetchPlugin.defaultMaxRequestsPerHost

  private lazy var standardSession: URLSession = makeSession(
    maxConnectionsPerHost: FlutterTurboFetchPlugin.defaultMaxRequestsPerHost,
    delegate: nil
  )

  private lazy var customSessionDelegate = TurboSessionTaskDelegate()
  private lazy var customSession: URLSession = makeSession(
    maxConnectionsPerHost: FlutterTurboFetchPlugin.defaultMaxRequestsPerHost,
    delegate: customSessionDelegate
  )

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
    lock.unlock()
    for task in tasks {
      task.cancel()
    }
  }

  func executeRequest(
    request: TurboHttpRequest,
    completion: @escaping (Result<TurboHttpResponse, Error>) -> Void
  ) {
    guard let url = URL(string: request.url) else {
      let errorResponse = TurboHttpResponse(
        errorType: "connectionError",
        errorMessage: "Invalid request URL: \(request.url)"
      )
      DispatchQueue.main.async {
        completion(.success(errorResponse))
      }
      return
    }

    var urlRequest = URLRequest(url: url)
    urlRequest.httpMethod = request.method.uppercased()

    if let timeoutMs = request.receiveTimeoutMs ?? request.connectTimeoutMs, timeoutMs > 0 {
      urlRequest.timeoutInterval = TimeInterval(timeoutMs) / 1000.0
    }

    for (key, value) in request.headers {
      if let key = key, let value = value {
        urlRequest.addValue(value, forHTTPHeaderField: key)
      }
    }

    if let bodyTypedData = request.body {
      urlRequest.httpBody = bodyTypedData.data
    }

    let requestedPerHost = max(1, Int(request.maxRequestsPerHost))
    let session = selectSession(
      maxConnectionsPerHost: requestedPerHost,
      followRedirects: request.followRedirects,
      allowBadCertificates: request.allowBadCertificates
    )

    // Use completionHandler API (never URLSessionDataDelegate) so CFNetwork concatenates chunks in C/C++ layer.
    let task = session.dataTask(with: urlRequest) { [weak self] data, response, error in
      guard let self = self else { return }
      self.removeTask(for: request.requestId)

      if let error = error {
        let (errorType, errorMessage) = self.classifyError(error)
        let turboResponse = TurboHttpResponse(
          errorType: errorType,
          errorMessage: errorMessage
        )
        DispatchQueue.main.async {
          completion(.success(turboResponse))
        }
        return
      }

      guard let httpResponse = response as? HTTPURLResponse else {
        let turboResponse = TurboHttpResponse(
          errorType: "connectionError",
          errorMessage: "Non-HTTP response received"
        )
        DispatchQueue.main.async {
          completion(.success(turboResponse))
        }
        return
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

    lock.lock()
    activeTasks[request.requestId] = task
    if !request.followRedirects || request.allowBadCertificates {
      customSessionDelegate.registerOptions(
        for: task.taskIdentifier,
        followRedirects: request.followRedirects,
        allowBadCertificates: request.allowBadCertificates
      )
    }
    lock.unlock()

    task.resume()
  }

  func cancelRequest(requestId: String) throws {
    lock.lock()
    let task = activeTasks.removeValue(forKey: requestId)
    if let task = task {
      customSessionDelegate.removeOptions(for: task.taskIdentifier)
    }
    lock.unlock()
    task?.cancel()
  }

  private func removeTask(for requestId: String) {
    lock.lock()
    if let task = activeTasks.removeValue(forKey: requestId) {
      customSessionDelegate.removeOptions(for: task.taskIdentifier)
    }
    lock.unlock()
  }

  private func selectSession(
    maxConnectionsPerHost: Int,
    followRedirects: Bool,
    allowBadCertificates: Bool
  ) -> URLSession {
    lock.lock()
    defer { lock.unlock() }

    if maxConnectionsPerHost != currentMaxConnectionsPerHost {
      currentMaxConnectionsPerHost = maxConnectionsPerHost
      standardSession = makeSession(maxConnectionsPerHost: maxConnectionsPerHost, delegate: nil)
      customSession = makeSession(
        maxConnectionsPerHost: maxConnectionsPerHost,
        delegate: customSessionDelegate
      )
    }

    if !followRedirects || allowBadCertificates {
      return customSession
    }
    return standardSession
  }

  private func makeSession(
    maxConnectionsPerHost: Int,
    delegate: URLSessionDelegate?
  ) -> URLSession {
    let config = URLSessionConfiguration.default
    config.httpMaximumConnectionsPerHost = maxConnectionsPerHost
    return URLSession(configuration: config, delegate: delegate, delegateQueue: nil)
  }

  private func classifyError(_ error: Error) -> (String, String) {
    let nsError = error as NSError
    if nsError.domain == NSURLErrorDomain {
      switch nsError.code {
      case NSURLErrorCancelled:
        return ("cancel", nsError.localizedDescription)
      case NSURLErrorTimedOut:
        return ("receiveTimeout", nsError.localizedDescription)
      case NSURLErrorServerCertificateHasBadDate,
           NSURLErrorServerCertificateUntrusted,
           NSURLErrorServerCertificateHasUnknownRoot,
           NSURLErrorServerCertificateNotYetValid,
           NSURLErrorClientCertificateRejected,
           NSURLErrorClientCertificateRequired,
           NSURLErrorSecureConnectionFailed:
        return ("badCertificate", nsError.localizedDescription)
      default:
        return ("connectionError", nsError.localizedDescription)
      }
    }
    return ("connectionError", error.localizedDescription)
  }
}

/// Task-level delegate used ONLY when custom redirect handling or self-signed certificate trust is requested.
/// Does NOT implement URLSessionDataDelegate, preserving CFNetwork one-shot completionHandler buffering.
private final class TurboSessionTaskDelegate: NSObject, URLSessionTaskDelegate {
  private struct TaskOptions {
    let followRedirects: Bool
    let allowBadCertificates: Bool
  }

  private let lock = NSLock()
  private var optionsByTaskId: [Int: TaskOptions] = [:]

  func registerOptions(
    for taskIdentifier: Int,
    followRedirects: Bool,
    allowBadCertificates: Bool
  ) {
    lock.lock()
    optionsByTaskId[taskIdentifier] = TaskOptions(
      followRedirects: followRedirects,
      allowBadCertificates: allowBadCertificates
    )
    lock.unlock()
  }

  func removeOptions(for taskIdentifier: Int) {
    lock.lock()
    optionsByTaskId.removeValue(forKey: taskIdentifier)
    lock.unlock()
  }

  private func options(for taskIdentifier: Int) -> TaskOptions? {
    lock.lock()
    defer { lock.unlock() }
    return optionsByTaskId[taskIdentifier]
  }

  func urlSession(
    _ session: URLSession,
    task: URLSessionTask,
    willPerformHTTPRedirection response: HTTPURLResponse,
    newRequest request: URLRequest,
    completionHandler: @escaping (URLRequest?) -> Void
  ) {
    if let opts = options(for: task.taskIdentifier), !opts.followRedirects {
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
    if let opts = options(for: task.taskIdentifier),
       opts.allowBadCertificates,
       challenge.protectionSpace.authenticationMethod == NSURLAuthenticationMethodServerTrust,
       let serverTrust = challenge.protectionSpace.serverTrust {
      completionHandler(.useCredential, URLCredential(trust: serverTrust))
      return
    }
    completionHandler(.performDefaultHandling, nil)
  }
}
