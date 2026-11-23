package io.github.khirohis.flutter_turbo_fetch

import android.content.Context

/**
 * Engine-agnostic HTTP execution contract implemented by the build-time selected Android engine
 * (OkHttp3 or Cronet).
 */
internal interface TurboHttpExecutor {
    fun ensureReady(context: Context? = null)

    fun execute(request: TurboHttpRequest, onResult: (TurboHttpResponse) -> Unit)

    fun cancel(requestId: String)

    fun shutdown()
}
