package io.github.khirohis.flutter_turbo_fetch

import android.content.Context
import org.chromium.net.NetworkException
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executor
import java.util.concurrent.ExecutorService
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.ThreadPoolExecutor
import java.util.concurrent.TimeUnit

internal fun createDefaultTurboHttpExecutor(): TurboHttpExecutor = CronetTurboHttpExecutor()

/**
 * Cronet-backed [TurboHttpExecutor] orchestrating request validation, asynchronous engine
 * initialization, in-flight call tracking, and bounded callback/buffer resources.
 */
internal open class CronetTurboHttpExecutor(
    private val clientManager: CronetClientManager = CronetClientManager(),
    private val callbackExecutor: Executor? = null,
    private val timerScheduler: CronetTimerScheduler = ScheduledCronetTimerScheduler()
) : TurboHttpExecutor {
    private val activeCalls = ConcurrentHashMap<String, ActiveCronetCall>()
    private val bufferPool = CronetDirectByteBufferPool(READ_BUFFER_SIZE, MAX_POOLED_BUFFERS)

    @Volatile
    private var ownedCallbackExecutor: ExecutorService? = null

    internal val activeCallCount: Int
        get() = activeCalls.size

    internal val pooledBufferCount: Int
        get() = bufferPool.size

    internal fun hasActiveCall(requestId: String): Boolean = activeCalls.containsKey(requestId)

    private fun ensureCallbackExecutor(): Executor {
        callbackExecutor?.let { return it }
        val current = ownedCallbackExecutor
        if (current != null && !current.isShutdown) {
            return current
        }
        synchronized(this) {
            val latest = ownedCallbackExecutor
            if (latest != null && !latest.isShutdown) {
                return latest
            }
            val created = createBoundedCallbackExecutor()
            ownedCallbackExecutor = created
            return created
        }
    }

    // -------------------------------------------------------------------------
    // Lifecycle & Call Management
    // -------------------------------------------------------------------------

    override fun ensureReady(context: Context?) {
        clientManager.ensureReady(context)
    }

    override fun shutdown() {
        clientManager.shutdown()
        for (requestId in activeCalls.keys) {
            activeCalls.remove(requestId)?.cancel()
        }
        timerScheduler.shutdown()
        bufferPool.clear()
        synchronized(this) {
            ownedCallbackExecutor?.shutdown()
            ownedCallbackExecutor = null
        }
    }

    override fun cancel(requestId: String) {
        activeCalls.remove(requestId)?.cancel()
    }

    // -------------------------------------------------------------------------
    // Request Execution Pipeline
    // -------------------------------------------------------------------------

    override fun execute(
        request: TurboHttpRequest,
        onResult: (TurboHttpResponse) -> Unit
    ) {
        if (request.allowBadCertificates) {
            onResult(
                TurboHttpResponse(
                    errorCode = "unknown",
                    errorMessage = "allowBadCertificates is not supported by the Android Cronet engine. " +
                        "Configure Android Network Security Config for development certificates, or use the OkHttp3 engine."
                )
            )
            return
        }

        val validatedRequest = try {
            CronetRequestValidator.validateAndNormalize(request)
        } catch (e: IllegalArgumentException) {
            onResult(
                TurboHttpResponse(
                    errorCode = "connectionError",
                    errorMessage = e.message ?: "Invalid HTTP request parameters."
                )
            )
            return
        }

        val executor = ensureCallbackExecutor()
        val activeCall = ActiveCronetCall(
            request = request,
            bufferPool = bufferPool,
            timerScheduler = timerScheduler,
            onComplete = { call, response ->
                activeCalls.remove(request.requestId, call)
                onResult(response)
            }
        )
        activeCalls[request.requestId] = activeCall

        val fastHandle = clientManager.tryAcquireIfInitialized()
        if (fastHandle != null) {
            startRequestWithEngine(activeCall, validatedRequest, fastHandle, executor)
        } else {
            startWithAsyncEngineInit(activeCall, validatedRequest, executor)
        }
    }

    private fun startWithAsyncEngineInit(
        activeCall: ActiveCronetCall,
        validatedRequest: ValidatedCronetRequest,
        executor: Executor
    ) {
        try {
            executor.execute {
                if (activeCall.isCompleted) {
                    return@execute
                }
                val engineHandle = try {
                    clientManager.acquireEngine()
                } catch (t: Throwable) {
                    activeCall.fail("unknown", t.message ?: "Failed to initialize CronetEngine.")
                    return@execute
                }
                if (activeCall.isCompleted) {
                    engineHandle.release()
                    return@execute
                }
                startRequestWithEngine(activeCall, validatedRequest, engineHandle, executor)
            }
        } catch (t: Throwable) {
            activeCall.fail("unknown", t.message ?: "Failed to schedule Cronet request.")
        }
    }

    private fun startRequestWithEngine(
        activeCall: ActiveCronetCall,
        validatedRequest: ValidatedCronetRequest,
        engineHandle: CronetClientManager.EngineHandle,
        executor: Executor
    ) {
        activeCall.attachEngineHandle(engineHandle)

        val builder = try {
            engineHandle.engine.newUrlRequestBuilder(
                validatedRequest.url,
                activeCall.callback,
                executor
            ).apply {
                setHttpMethod(validatedRequest.method)
                for ((key, value) in validatedRequest.headers) {
                    addHeader(key, value)
                }
                if (validatedRequest.body != null) {
                    setUploadDataProvider(
                        activeCall.createUploadProvider(validatedRequest.body),
                        executor
                    )
                }
            }
        } catch (t: Throwable) {
            activeCall.failAndRelease("unknown", t.message ?: "Failed to build Cronet UrlRequest.")
            return
        }

        val urlRequest = try {
            builder.build()
        } catch (t: Throwable) {
            activeCall.failAndRelease("connectionError", t.message ?: "Failed to build Cronet UrlRequest.")
            return
        }

        activeCall.attachUrlRequest(urlRequest)
        if (activeCall.isCompleted) {
            urlRequest.cancel()
            activeCall.releaseResources()
            return
        }

        activeCall.schedulePhaseTimer(
            delayMs = activeCall.request.connectTimeoutMs,
            errorCode = "connectionTimeout",
            message = "Connection timed out after ${activeCall.request.connectTimeoutMs}ms."
        )

        try {
            urlRequest.start()
        } catch (t: Throwable) {
            activeCall.failAndRelease("unknown", t.message ?: "Failed to start Cronet UrlRequest.")
        }
    }

    internal fun classifyNetworkException(e: NetworkException, phase: RequestPhase): String =
        CronetErrorClassifier.classify(e, phase)

    companion object {
        private const val READ_BUFFER_SIZE = 64 * 1024
        private const val MAX_POOLED_BUFFERS = 16

        private fun createBoundedCallbackExecutor(): ExecutorService {
            val maxThreads = Runtime.getRuntime().availableProcessors().coerceIn(2, 4)
            return ThreadPoolExecutor(
                maxThreads,
                maxThreads,
                30L,
                TimeUnit.SECONDS,
                LinkedBlockingQueue()
            ) { runnable ->
                Thread(runnable, "turbo-fetch-cronet-callback").apply {
                    isDaemon = true
                }
            }.apply {
                allowCoreThreadTimeOut(true)
            }
        }
    }
}
