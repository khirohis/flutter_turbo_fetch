package io.github.khirohis.flutter_turbo_fetch

import android.os.Handler
import android.os.Looper
import io.flutter.embedding.engine.plugins.FlutterPlugin
import okhttp3.Call
import okhttp3.ConnectionPool
import okhttp3.Dispatcher
import okhttp3.OkHttpClient
import okhttp3.Protocol
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.TimeUnit

/**
 * Flutter plugin interface and OkHttp resource/request lifecycle manager.
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

    private val activeCalls = ConcurrentHashMap<String, Call>()
    private val clientLock = Any()

    private var dispatcher: Dispatcher = createDispatcher()
    private var baseClient: OkHttpClient = createBaseClient(dispatcher)

    private fun createDispatcher(): Dispatcher = Dispatcher().apply {
        maxRequests = DEFAULT_MAX_REQUESTS
        maxRequestsPerHost = DEFAULT_MAX_REQUESTS_PER_HOST
    }

    private fun createBaseClient(targetDispatcher: Dispatcher): OkHttpClient =
        OkHttpClient.Builder()
            .dispatcher(targetDispatcher)
            .connectionPool(ConnectionPool(5, 5, TimeUnit.MINUTES))
            .protocols(listOf(Protocol.HTTP_2, Protocol.HTTP_1_1))
            .build()

    internal fun ensureClient(requestedPerHost: Int): OkHttpClient {
        synchronized(clientLock) {
            if (dispatcher.executorService.isShutdown) {
                dispatcher = createDispatcher()
                baseClient = createBaseClient(dispatcher)
            }
            if (requestedPerHost > dispatcher.maxRequests) {
                dispatcher.maxRequests = requestedPerHost
            } else if (dispatcher.maxRequests > DEFAULT_MAX_REQUESTS && requestedPerHost <= DEFAULT_MAX_REQUESTS) {
                dispatcher.maxRequests = DEFAULT_MAX_REQUESTS
            }
            if (dispatcher.maxRequestsPerHost != requestedPerHost) {
                dispatcher.maxRequestsPerHost = requestedPerHost
            }
            return baseClient
        }
    }

    override fun onAttachedToEngine(flutterPluginBinding: FlutterPlugin.FlutterPluginBinding) {
        synchronized(clientLock) {
            if (dispatcher.executorService.isShutdown) {
                dispatcher = createDispatcher()
                baseClient = createBaseClient(dispatcher)
            }
        }
        TurboFetchHostApi.setUp(flutterPluginBinding.binaryMessenger, this)
    }

    override fun onDetachedFromEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        TurboFetchHostApi.setUp(binding.binaryMessenger, null)
        for (call in activeCalls.values) {
            call.cancel()
        }
        activeCalls.clear()
        synchronized(clientLock) {
            dispatcher.executorService.shutdown()
            baseClient.connectionPool.evictAll()
        }
    }

    override fun executeRequest(
        request: TurboHttpRequest,
        callback: (Result<TurboHttpResponse>) -> Unit
    ) {
        val requestedPerHost = request.maxRequestsPerHost.toInt().coerceAtLeast(1)
        val sharedClient = ensureClient(requestedPerHost)

        executor.execute(
            client = sharedClient,
            request = request,
            onCallCreated = { call ->
                activeCalls[request.requestId] = call
            },
            onResult = { response ->
                activeCalls.remove(request.requestId)
                postToMain {
                    callback(Result.success(response))
                }
            }
        )
    }

    override fun cancelRequest(requestId: String) {
        activeCalls.remove(requestId)?.cancel()
    }

    companion object {
        private const val DEFAULT_MAX_REQUESTS = 64
        private const val DEFAULT_MAX_REQUESTS_PER_HOST = 32
    }
}
