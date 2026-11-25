package io.github.khirohis.flutter_turbo_fetch

import android.content.Context
import org.chromium.net.CronetException
import org.chromium.net.NetworkException
import org.chromium.net.UploadDataProvider
import org.chromium.net.UploadDataSink
import org.chromium.net.UrlRequest
import org.chromium.net.UrlResponseInfo
import java.net.URI
import java.net.URISyntaxException
import java.nio.ByteBuffer
import java.util.Locale
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.Executor
import java.util.concurrent.ExecutorService
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.ScheduledThreadPoolExecutor
import java.util.concurrent.ThreadPoolExecutor
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference

internal fun createDefaultTurboHttpExecutor(): TurboHttpExecutor = CronetTurboHttpExecutor()

internal enum class RequestPhase {
    CONNECTING,
    SENDING_BODY,
    RECEIVING
}

internal interface CronetTimerHandle {
    fun cancel()
}

internal interface CronetTimerScheduler {
    fun nowMs(): Long = System.nanoTime() / 1_000_000L

    fun schedule(delayMs: Long, task: () -> Unit): CronetTimerHandle

    fun shutdown()
}

internal class ScheduledCronetTimerScheduler : CronetTimerScheduler {
    private var executor: ScheduledExecutorService? = null

    @Synchronized
    private fun ensureExecutor(): ScheduledExecutorService {
        val current = executor
        if (current != null && !current.isShutdown) {
            return current
        }
        val created = ScheduledThreadPoolExecutor(1) { runnable ->
            Thread(runnable, "turbo-fetch-cronet-timer").apply {
                isDaemon = true
            }
        }.apply {
            removeOnCancelPolicy = true
        }
        executor = created
        return created
    }

    override fun schedule(delayMs: Long, task: () -> Unit): CronetTimerHandle {
        val future: ScheduledFuture<*> = ensureExecutor().schedule(task, delayMs, TimeUnit.MILLISECONDS)
        return object : CronetTimerHandle {
            override fun cancel() {
                future.cancel(false)
            }
        }
    }

    @Synchronized
    override fun shutdown() {
        executor?.shutdownNow()
        executor = null
    }
}

private class DirectByteBufferPool(
    private val bufferSize: Int,
    private val maxPooled: Int
) {
    private val pool = ConcurrentLinkedQueue<ByteBuffer>()
    private val pooledCount = AtomicInteger(0)

    val size: Int
        get() = pooledCount.get()

    fun acquire(): ByteBuffer {
        val pooled = pool.poll()
        if (pooled != null) {
            pooledCount.decrementAndGet()
            pooled.clear()
            return pooled
        }
        return ByteBuffer.allocateDirect(bufferSize)
    }

    fun release(buffer: ByteBuffer) {
        buffer.clear()
        if (pooledCount.incrementAndGet() <= maxPooled) {
            pool.offer(buffer)
        } else {
            pooledCount.decrementAndGet()
        }
    }

    fun clear() {
        while (pool.poll() != null) {
            pooledCount.decrementAndGet()
        }
    }
}

private class ResponseBodyAccumulator {
    private val chunks = ArrayList<ByteArray>(4)

    var totalBytes: Long = 0L
        private set

    fun append(byteBuffer: ByteBuffer, bytesRead: Int): Boolean {
        val newTotal = totalBytes + bytesRead.toLong()
        if (newTotal > Int.MAX_VALUE) {
            return false
        }
        totalBytes = newTotal
        val chunk = ByteArray(bytesRead)
        byteBuffer.get(chunk)
        chunks.add(chunk)
        return true
    }

    fun toByteArray(): ByteArray = when (chunks.size) {
        0 -> ByteArray(0)
        1 -> chunks[0]
        else -> {
            val merged = ByteArray(totalBytes.toInt())
            var offset = 0
            for (chunk in chunks) {
                System.arraycopy(chunk, 0, merged, offset, chunk.size)
                offset += chunk.size
            }
            merged
        }
    }
}

/**
 * Cronet-backed [TurboHttpExecutor] performing one-shot response body aggregation via a bounded
 * pool of 64KiB direct [ByteBuffer]s, watchdog phase timeouts, strict pre-flight validation, and
 * deferred engine cleanup.
 */
internal open class CronetTurboHttpExecutor(
    private val clientManager: CronetClientManager = CronetClientManager(),
    private val callbackExecutor: Executor? = null,
    private val timerScheduler: CronetTimerScheduler = ScheduledCronetTimerScheduler()
) : TurboHttpExecutor {
    private val activeCalls = ConcurrentHashMap<String, ActiveCronetCall>()
    private val bufferPool = DirectByteBufferPool(READ_BUFFER_SIZE, MAX_POOLED_BUFFERS)

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
                errorResponse(
                    code = "unknown",
                    message = "allowBadCertificates is not supported by the Android Cronet engine. " +
                            "Configure Android Network Security Config for development certificates, or use the OkHttp3 engine."
                )
            )
            return
        }

        val validatedRequest = try {
            validateAndNormalizeRequest(request)
        } catch (e: IllegalArgumentException) {
            onResult(errorResponse("connectionError", e.message ?: "Invalid HTTP request parameters."))
            return
        }

        val executor = ensureCallbackExecutor()
        val activeCall = ActiveCronetCall(request, onResult)
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
        validatedRequest: ValidatedRequest,
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
        validatedRequest: ValidatedRequest,
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

    // -------------------------------------------------------------------------
    // Pre-flight Validation
    // -------------------------------------------------------------------------

    private data class ValidatedRequest(
        val url: String,
        val method: String,
        val headers: List<Pair<String, String>>,
        val body: ByteArray?
    )

    private fun validateAndNormalizeRequest(request: TurboHttpRequest): ValidatedRequest {
        val method = request.method.trim().uppercase(Locale.ROOT)
        if (method.isEmpty()) {
            throw IllegalArgumentException("HTTP method must not be blank.")
        }

        val uri = try {
            URI(request.url)
        } catch (e: URISyntaxException) {
            throw IllegalArgumentException("Malformed request URL: ${e.message}", e)
        }
        val scheme = uri.scheme?.lowercase(Locale.ROOT)
        if (scheme != "http" && scheme != "https") {
            throw IllegalArgumentException("Unsupported URL scheme: '${uri.scheme}'. Only http and https are supported.")
        }
        if (uri.host.isNullOrBlank()) {
            throw IllegalArgumentException("Request URL must specify a non-empty host.")
        }

        val validatedHeaders = ArrayList<Pair<String, String>>(request.headers.size)
        var hasNonBlankContentType = false
        for ((rawKey, rawValue) in request.headers) {
            if (rawKey == null || rawValue == null) {
                continue
            }
            validateHeaderName(rawKey)
            validateHeaderValue(rawKey, rawValue)
            if (rawKey.equals("content-type", ignoreCase = true) && rawValue.isNotBlank()) {
                hasNonBlankContentType = true
            }
            validatedHeaders.add(rawKey to rawValue)
        }

        val rawBody = request.body
        val nonEmptyBody = if (rawBody != null && rawBody.isNotEmpty()) {
            if (!hasNonBlankContentType) {
                throw IllegalArgumentException(
                    "Non-empty request body requires a Content-Type header when using the Android Cronet engine."
                )
            }
            rawBody
        } else {
            null
        }

        return ValidatedRequest(
            url = request.url,
            method = method,
            headers = validatedHeaders,
            body = nonEmptyBody
        )
    }

    private fun validateHeaderName(name: String) {
        if (name.isEmpty()) {
            throw IllegalArgumentException("Header name must not be empty.")
        }
        for (ch in name) {
            if (ch !in '\u0021'..'\u007e' || ch == ':') {
                throw IllegalArgumentException("Invalid character in HTTP header name: '$name'.")
            }
        }
    }

    private fun validateHeaderValue(name: String, value: String) {
        for (ch in value) {
            if (ch != '\t' && ch !in '\u0020'..'\u007e') {
                throw IllegalArgumentException("Invalid character in HTTP header value for '$name'.")
            }
        }
    }

    // -------------------------------------------------------------------------
    // Per-Request State & Callbacks
    // -------------------------------------------------------------------------

    private inner class ActiveCronetCall(
        val request: TurboHttpRequest,
        private val onResult: (TurboHttpResponse) -> Unit
    ) {
        private val completed = AtomicBoolean(false)
        private val urlRequestRef = AtomicReference<UrlRequest?>()
        private val engineHandleRef = AtomicReference<CronetClientManager.EngineHandle?>()
        private val leasedBufferRef = AtomicReference<ByteBuffer?>()
        private val timerLock = Any()
        private var currentTimer: CronetTimerHandle? = null

        @Volatile
        private var phase: RequestPhase = RequestPhase.CONNECTING

        @Volatile
        private var lastReceiveActivityMs: Long = 0L

        private val bodyAccumulator = ResponseBodyAccumulator()

        val isCompleted: Boolean
            get() = completed.get()

        fun attachEngineHandle(handle: CronetClientManager.EngineHandle) {
            engineHandleRef.set(handle)
        }

        fun attachUrlRequest(urlRequest: UrlRequest) {
            urlRequestRef.set(urlRequest)
        }

        fun releaseResources() {
            leasedBufferRef.getAndSet(null)?.let { bufferPool.release(it) }
            engineHandleRef.getAndSet(null)?.release()
        }

        fun createUploadProvider(bodyBytes: ByteArray): UploadDataProvider =
            TrackingUploadDataProvider(bodyBytes)

        private fun replaceTimer(delayMs: Long?, onTimeout: () -> Unit) {
            synchronized(timerLock) {
                currentTimer?.cancel()
                currentTimer = null
                if (!completed.get() && delayMs != null && delayMs > 0L) {
                    currentTimer = timerScheduler.schedule(delayMs, onTimeout)
                }
            }
        }

        fun cancelTimer() {
            replaceTimer(null) {}
        }

        fun schedulePhaseTimer(delayMs: Long?, errorCode: String, message: String) {
            replaceTimer(delayMs) {
                fail(errorCode, message)
                urlRequestRef.get()?.cancel()
            }
        }

        fun startReceiveWatchdog() {
            val timeoutMs = request.receiveTimeoutMs
            if (timeoutMs == null || timeoutMs <= 0L) {
                cancelTimer()
                return
            }
            lastReceiveActivityMs = timerScheduler.nowMs()
            scheduleReceiveWatchdog(timeoutMs, timeoutMs)
        }

        private fun scheduleReceiveWatchdog(timeoutMs: Long, delayMs: Long) {
            replaceTimer(delayMs) {
                if (completed.get()) {
                    return@replaceTimer
                }
                val elapsedMs = (timerScheduler.nowMs() - lastReceiveActivityMs).coerceAtLeast(0L)
                val remainingMs = timeoutMs - elapsedMs
                if (remainingMs > 0L) {
                    scheduleReceiveWatchdog(timeoutMs, remainingMs)
                } else {
                    fail("receiveTimeout", "Response receive timed out after ${timeoutMs}ms.")
                    urlRequestRef.get()?.cancel()
                }
            }
        }

        fun cancel() {
            cancelTimer()
            val urlRequest = urlRequestRef.get()
            if (urlRequest != null) {
                urlRequest.cancel()
            } else {
                failAndRelease("cancel", "Request was cancelled.")
            }
        }

        fun finish(response: TurboHttpResponse) {
            if (completed.compareAndSet(false, true)) {
                cancelTimer()
                activeCalls.remove(request.requestId, this)
                onResult(response)
            }
        }

        fun fail(errorCode: String, errorMessage: String) {
            finish(errorResponse(errorCode, errorMessage))
        }

        fun failAndRelease(errorCode: String, errorMessage: String) {
            releaseResources()
            fail(errorCode, errorMessage)
        }

        fun failAndCancel(urlRequest: UrlRequest, errorCode: String, errorMessage: String) {
            fail(errorCode, errorMessage)
            urlRequest.cancel()
        }

        val callback: UrlRequest.Callback = object : UrlRequest.Callback() {
            override fun onRedirectReceived(
                urlRequest: UrlRequest,
                info: UrlResponseInfo,
                newLocationUrl: String
            ) {
                if (completed.get()) {
                    urlRequest.cancel()
                    return
                }
                if (request.followRedirects) {
                    urlRequest.followRedirect()
                } else {
                    finish(
                        TurboHttpResponse(
                            statusCode = info.httpStatusCode.toLong(),
                            statusMessage = info.httpStatusText,
                            headers = normalizeHeaders(info),
                            body = ByteArray(0)
                        )
                    )
                    urlRequest.cancel()
                }
            }

            override fun onResponseStarted(urlRequest: UrlRequest, info: UrlResponseInfo) {
                if (completed.get()) {
                    urlRequest.cancel()
                    return
                }
                phase = RequestPhase.RECEIVING

                val maxBytes = request.maxResponseBytes?.coerceAtLeast(0L)
                val contentLength = extractContentLength(info)
                if (maxBytes != null && contentLength != null && contentLength > maxBytes) {
                    failAndCancel(
                        urlRequest,
                        "maxResponseBytesExceeded",
                        "Response body exceeded maxResponseBytes limit ($maxBytes bytes, contentLength: $contentLength)"
                    )
                    return
                }

                startReceiveWatchdog()

                val readBuffer = bufferPool.acquire()
                leasedBufferRef.set(readBuffer)
                urlRequest.read(readBuffer)
            }

            override fun onReadCompleted(
                urlRequest: UrlRequest,
                info: UrlResponseInfo,
                byteBuffer: ByteBuffer
            ) {
                if (completed.get()) {
                    urlRequest.cancel()
                    return
                }

                byteBuffer.flip()
                val bytesRead = byteBuffer.remaining()
                val maxBytes = request.maxResponseBytes?.coerceAtLeast(0L)
                if (maxBytes != null && bytesRead.toLong() > maxBytes - bodyAccumulator.totalBytes) {
                    failAndCancel(
                        urlRequest,
                        "maxResponseBytesExceeded",
                        "Response body exceeded maxResponseBytes limit ($maxBytes bytes)"
                    )
                    return
                }

                if (bytesRead > 0 && !bodyAccumulator.append(byteBuffer, bytesRead)) {
                    failAndCancel(urlRequest, "unknown", "Response body exceeds maximum ByteArray size")
                    return
                }
                byteBuffer.clear()

                // Record activity timestamp without canceling/re-scheduling timer on every chunk.
                lastReceiveActivityMs = timerScheduler.nowMs()

                urlRequest.read(byteBuffer)
            }

            override fun onSucceeded(urlRequest: UrlRequest, info: UrlResponseInfo) {
                val bodyBytes = bodyAccumulator.toByteArray()
                releaseResources()
                finish(
                    TurboHttpResponse(
                        statusCode = info.httpStatusCode.toLong(),
                        statusMessage = info.httpStatusText,
                        headers = normalizeHeaders(info),
                        body = bodyBytes
                    )
                )
            }

            override fun onFailed(
                urlRequest: UrlRequest,
                info: UrlResponseInfo?,
                error: CronetException?
            ) {
                val errorCode = if (error is NetworkException) {
                    classifyNetworkException(error, phase)
                } else {
                    "unknown"
                }
                failAndRelease(errorCode, error?.message ?: "Cronet request failed.")
            }

            override fun onCanceled(urlRequest: UrlRequest, info: UrlResponseInfo?) {
                failAndRelease("cancel", "Request was cancelled.")
            }
        }

        private inner class TrackingUploadDataProvider(
            private val bytes: ByteArray
        ) : UploadDataProvider() {
            private var offset = 0
            private var uploadStarted = false

            override fun getLength(): Long = bytes.size.toLong()

            override fun read(uploadDataSink: UploadDataSink, byteBuffer: ByteBuffer) {
                val remainingBytes = bytes.size - offset
                val toWrite = minOf(byteBuffer.remaining(), remainingBytes)
                if (toWrite > 0) {
                    byteBuffer.put(bytes, offset, toWrite)
                    offset += toWrite
                }

                if (offset >= bytes.size) {
                    uploadStarted = true
                    phase = RequestPhase.RECEIVING
                    startReceiveWatchdog()
                } else if (!uploadStarted) {
                    uploadStarted = true
                    phase = RequestPhase.SENDING_BODY
                    schedulePhaseTimer(
                        delayMs = request.sendTimeoutMs,
                        errorCode = "sendTimeout",
                        message = "Request send timed out after ${request.sendTimeoutMs}ms."
                    )
                }

                uploadDataSink.onReadSucceeded(false)
            }

            override fun rewind(uploadDataSink: UploadDataSink) {
                offset = 0
                uploadStarted = false
                uploadDataSink.onRewindSucceeded()
            }
        }
    }

    // -------------------------------------------------------------------------
    // Response Normalization & Error Classification
    // -------------------------------------------------------------------------

    private fun normalizeHeaders(info: UrlResponseInfo): Map<String?, Any?> {
        val list = info.allHeadersAsList
        val grouped = HashMap<String?, MutableList<String>>(list.size)
        for (entry in list) {
            val key = entry.key.lowercase(Locale.ROOT)
            grouped.getOrPut(key) { ArrayList(1) }.add(entry.value)
        }
        return grouped
    }

    private fun errorResponse(code: String, message: String): TurboHttpResponse =
        TurboHttpResponse(errorCode = code, errorMessage = message)

    private fun extractContentLength(info: UrlResponseInfo): Long? =
        info.allHeadersAsList
            .firstOrNull { it.key.equals("content-length", ignoreCase = true) }
            ?.value
            ?.trim()
            ?.toLongOrNull()

    internal fun classifyNetworkException(e: NetworkException, phase: RequestPhase): String {
        val errorCode = e.errorCode
        if (errorCode == NetworkException.ERROR_CONNECTION_TIMED_OUT) {
            return "connectionTimeout"
        }
        if (errorCode == NetworkException.ERROR_TIMED_OUT) {
            return when (phase) {
                RequestPhase.CONNECTING -> "connectionTimeout"
                RequestPhase.SENDING_BODY -> "sendTimeout"
                RequestPhase.RECEIVING -> "receiveTimeout"
            }
        }

        val internalCode = e.cronetInternalErrorCode
        if (internalCode in -299..-200 || internalCode in SSL_HANDSHAKE_INTERNAL_ERROR_CODES) {
            return "badCertificate"
        }

        return when (errorCode) {
            NetworkException.ERROR_HOSTNAME_NOT_RESOLVED,
            NetworkException.ERROR_INTERNET_DISCONNECTED,
            NetworkException.ERROR_NETWORK_CHANGED,
            NetworkException.ERROR_CONNECTION_CLOSED,
            NetworkException.ERROR_CONNECTION_REFUSED,
            NetworkException.ERROR_CONNECTION_RESET,
            NetworkException.ERROR_ADDRESS_UNREACHABLE,
            NetworkException.ERROR_QUIC_PROTOCOL_FAILED,
            NetworkException.ERROR_OTHER -> "connectionError"

            else -> "unknown"
        }
    }

    companion object {
        private const val READ_BUFFER_SIZE = 64 * 1024
        private const val MAX_POOLED_BUFFERS = 16

        // Chromium net_error_list.h SSL/TLS handshake error codes outside -200..-299.
        private val SSL_HANDSHAKE_INTERNAL_ERROR_CODES = setOf(
            -107, // ERR_SSL_PROTOCOL_ERROR
            -110, // ERR_SSL_CLIENT_AUTH_CERT_NEEDED
            -113, // ERR_SSL_VERSION_OR_CIPHER_MISMATCH
            -114, // ERR_SSL_RENEGOTIATION_REQUESTED
            -117, // ERR_BAD_SSL_CLIENT_AUTH_CERT
            -125, // ERR_SSL_UNRECOGNIZED_NAME_ALERT
            -126, // ERR_SSL_SERVER_CERT_BAD_FORMAT
            -148, // ERR_SSL_OBSOLETE_CIPHER
            -150, // ERR_SSL_PINNED_KEY_NOT_IN_CERT_CHAIN
            -156  // ERR_SSL_CLIENT_AUTH_SIGNATURE_FAILED
        )

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
