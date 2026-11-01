package io.github.khirohis.flutter_turbo_fetch

import android.os.Handler
import android.os.Looper
import io.flutter.embedding.engine.plugins.FlutterPlugin

/**
 * Flutter plugin interface layer bridging Pigeon HostApi and main-thread callback dispatch.
 */
class FlutterTurboFetchPlugin internal constructor(
    private val executor: TurboHttpExecutor = TurboHttpExecutor()
) : FlutterPlugin,
    TurboFetchHostApi {

    constructor() : this(TurboHttpExecutor())

    private val mainHandler = Handler(Looper.getMainLooper())

    private fun postToMain(action: () -> Unit) {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            action()
        } else {
            mainHandler.post(action)
        }
    }

    override fun onAttachedToEngine(flutterPluginBinding: FlutterPlugin.FlutterPluginBinding) {
        executor.ensureReady()
        TurboFetchHostApi.setUp(flutterPluginBinding.binaryMessenger, this)
    }

    override fun onDetachedFromEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        TurboFetchHostApi.setUp(binding.binaryMessenger, null)
        executor.shutdown()
    }

    override fun executeRequest(
        request: TurboHttpRequest,
        callback: (Result<TurboHttpResponse>) -> Unit
    ) {
        executor.execute(
            request = request,
            onResult = { response ->
                postToMain {
                    callback(Result.success(response))
                }
            }
        )
    }

    override fun cancelRequest(requestId: String) {
        executor.cancel(requestId)
    }
}
