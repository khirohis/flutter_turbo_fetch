package io.github.khirohis.flutter_turbo_fetch

import android.os.Handler
import android.os.Looper
import io.flutter.embedding.engine.plugins.FlutterPlugin

/**
 * Flutter plugin interface layer bridging Pigeon HostApi and main-thread callback dispatch.
 */
class FlutterTurboFetchPlugin internal constructor(
    handler: Handler? = null,
    private val executor: TurboHttpExecutor = TurboHttpExecutor()
) : FlutterPlugin,
    TurboFetchHostApi {

    constructor() : this(null, TurboHttpExecutor())

    constructor(handler: Handler?) : this(handler, TurboHttpExecutor())

    private val mainHandler: Handler? = handler ?: try {
        val looper = Looper.getMainLooper()
        if (looper != null) Handler(looper) else null
    } catch (_: Exception) {
        null
    }

    private fun postToMain(action: () -> Unit) {
        val h = mainHandler
        if (h != null) {
            h.post(action)
        } else {
            action()
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
