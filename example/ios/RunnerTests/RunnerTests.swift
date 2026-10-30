import Flutter
import UIKit
import XCTest

@testable import flutter_turbo_fetch

class RunnerTests: XCTestCase {

  func testPluginInitialization() {
    let plugin = FlutterTurboFetchPlugin()
    XCTAssertNotNil(plugin)
  }

}
