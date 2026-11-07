import Flutter
import Foundation

/// Flutter plugin interface layer bridging Pigeon HostApi and main-thread callback dispatch.
public class FlutterTurboFetchPlugin: NSObject, FlutterPlugin, TurboFetchHostApi {
  private let executor: TurboHttpExecutor

  public override convenience init() {
    self.init(executor: TurboHttpExecutor())
  }

  internal init(executor: TurboHttpExecutor) {
    self.executor = executor
    super.init()
  }

  public static func register(with registrar: FlutterPluginRegistrar) {
    let instance = FlutterTurboFetchPlugin()
    TurboFetchHostApiSetup.setUp(binaryMessenger: registrar.messenger(), api: instance)
    registrar.publish(instance)
  }

  public func detachFromEngine(for registrar: FlutterPluginRegistrar) {
    TurboFetchHostApiSetup.setUp(binaryMessenger: registrar.messenger(), api: nil)
    executor.shutdown()
  }

  func executeRequest(
    request: TurboHttpRequest,
    completion: @escaping (Result<TurboHttpResponse, Error>) -> Void
  ) {
    executor.execute(request: request) { [weak self] response in
      guard let self = self else { return }
      self.postToMain {
        completion(.success(response))
      }
    }
  }

  func cancelRequest(requestId: String) throws {
    executor.cancel(requestId: requestId)
  }

  internal func postToMain(_ action: @escaping () -> Void) {
    if Thread.isMainThread {
      action()
    } else {
      DispatchQueue.main.async(execute: action)
    }
  }
}
