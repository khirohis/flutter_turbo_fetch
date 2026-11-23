package io.github.khirohis.flutter_turbo_fetch

import android.content.Context
import org.chromium.net.CronetException
import org.chromium.net.NetworkException
import org.chromium.net.UploadDataProvider
import org.chromium.net.UploadDataSink
import org.chromium.net.UrlRequest
import org.chromium.net.UrlResponseInfo
import java.io.ByteArrayOutputStream
import java.net.URI
import java.net.URISyntaxException
import java.nio.ByteBuffer
import java.util.Locale
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executor
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
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
        val created = Executors.newSingleThreadScheduledExecutor { runnable ->
            Thread(runnable, "turbo-fetch-cronet-timer").apply {
                isDaemon = true
            }
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

/**
 * Cronet-backed [TurboHttpExecutor] performing one-shot response body aggregation via a fixed
 * 64KiB reusable direct [ByteBuffer], phase-specific timeouts, strict pre-flight validation, and
 * deferred engine cleanup.
 */
internal open class CronetTurboHttpExecutor(
    private val clientManager: CronetClientManager = CronetClientManager(),
    private val callbackExecutor: Executor = defaultCallbackExecutor(),
    private val timerScheduler: CronetTimerScheduler = ScheduledCronetTimerScheduler()
) : TurboHttpExecutor {
    private val activeCalls = ConcurrentHashMap<String, ActiveCronetCall>()

    internal val activeCallCount: Int
        get() = activeCalls.size

    internal fun hasActiveCall(requestId: String): Boolean = activeCalls.containsKey(requestId)

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
            validateAndNormalizeRequest(request)
        } catch (e: IllegalArgumentException) {
            onResult(
                TurboHttpResponse(
                    errorCode = "connectionError",
                    errorMessage = e.message ?: "Invalid HTTP request parameters."
                )
            )
            return
        }

        val engineHandle = try {
            clientManager.acquireEngine()
        } catch (t: Throwable) {
            onResult(
                TurboHttpResponse(
                    errorCode = "unknown",
                    errorMessage = t.message ?: "Failed to initialize CronetEngine."
                )
            )
            return
        }

        val activeCall = ActiveCronetCall(request, engineHandle, onResult)
        val builder = try {
            engineHandle.engine.newUrlRequestBuilder(
                validatedRequest.url,
                activeCall.callback,
                callbackExecutor
            ).apply {
                setHttpMethod(validatedRequest.method)
                for ((key, value) in validatedRequest.headers) {
                    addHeader(key, value)
                }
                if (validatedRequest.body != null) {
                    setUploadDataProvider(
                        activeCall.createUploadProvider(validatedRequest.body),
                        callbackExecutor
                    )
                }
            }
        } catch (t: Throwable) {
            engineHandle.release()
            onResult(
                TurboHttpResponse(
                    errorCode = "unknown",
                    errorMessage = t.message ?: "Failed to build Cronet UrlRequest."
                )
            )
            return
        }

        val urlRequest = try {
            builder.build()
        } catch (t: Throwable) {
            engineHandle.release()
            onResult(
                TurboHttpResponse(
                    errorCode = "connectionError",
                    errorMessage = t.message ?: "Failed to build Cronet UrlRequest."
                )
            )
            return
        }

        activeCall.attachUrlRequest(urlRequest)
        activeCalls[request.requestId] = activeCall

        activeCall.schedulePhaseTimer(
            delayMs = request.connectTimeoutMs,
            errorCode = "connectionTimeout",
            message = "Connection timed out after ${request.connectTimeoutMs}ms."
        )

        try {
            urlRequest.start()
        } catch (t: Throwable) {
            activeCall.finish(
                TurboHttpResponse(
                    errorCode = "unknown",
                    errorMessage = t.message ?: "Failed to start Cronet UrlRequest."
                )
            )
            engineHandle.release()
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
        private val request: TurboHttpRequest,
        private val engineHandle: CronetClientManager.EngineHandle,
        private val onResult: (TurboHttpResponse) -> Unit
    ) {
        private val completed = AtomicBoolean(false)
        private val urlRequestRef = AtomicReference<UrlRequest?>()
        private val timerLock = Any()
        private var currentTimer: CronetTimerHandle? = null

        @Volatile
        private var phase: RequestPhase = RequestPhase.CONNECTING

        private val readBuffer: ByteBuffer = ByteBuffer.allocateDirect(READ_BUFFER_SIZE)
        private val transferBuffer = ByteArray(READ_BUFFER_SIZE)
        private val outputStream = ByteArrayOutputStream()
        private var totalBytesRead = 0L

        fun attachUrlRequest(urlRequest: UrlRequest) {
            urlRequestRef.set(urlRequest)
        }

        fun createUploadProvider(bodyBytes: ByteArray): UploadDataProvider =
            TrackingUploadDataProvider(bodyBytes)

        fun schedulePhaseTimer(delayMs: Long?, errorCode: String, message: String) {
            synchronized(timerLock) {
                currentTimer?.cancel()
                currentTimer = null
                if (!completed.get() && delayMs != null && delayMs > 0L) {
                    currentTimer = timerScheduler.schedule(delayMs) {
                        finish(
                            TurboHttpResponse(
                                errorCode = errorCode,
                                errorMessage = message
                            )
                        )
                        urlRequestRef.get()?.cancel()
                    }
                }
            }
        }

        fun cancelTimer() {
            synchronized(timerLock) {
                currentTimer?.cancel()
                currentTimer = null
            }
        }

        fun cancel() {
            cancelTimer()
            urlRequestRef.get()?.cancel()
        }

        fun finish(response: TurboHttpResponse) {
            if (completed.compareAndSet(false, true)) {
                cancelTimer()
                activeCalls.remove(request.requestId, this)
                onResult(response)
            }
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
                    finish(
                        TurboHttpResponse(
                            errorCode = "maxResponseBytesExceeded",
                            errorMessage = "Response body exceeded maxResponseBytes limit ($maxBytes bytes, contentLength: $contentLength)"
                        )
                    )
                    urlRequest.cancel()
                    return
                }

                schedulePhaseTimer(
                    delayMs = request.receiveTimeoutMs,
                    errorCode = "receiveTimeout",
                    message = "Response receive timed out after ${request.receiveTimeoutMs}ms."
                )

                readBuffer.clear()
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
                if (maxBytes != null && bytesRead.toLong() > maxBytes - totalBytesRead) {
                    finish(
                        TurboHttpResponse(
                            errorCode = "maxResponseBytesExceeded",
                            errorMessage = "Response body exceeded maxResponseBytes limit ($maxBytes bytes)"
                        )
                    )
                    urlRequest.cancel()
                    return
                }

                if (bytesRead > 0) {
                    byteBuffer.get(transferBuffer, 0, bytesRead)
                    outputStream.write(transferBuffer, 0, bytesRead)
                    totalBytesRead += bytesRead
                }
                byteBuffer.clear()

                // Reset idle receive timer before requesting the next chunk.
                schedulePhaseTimer(
                    delayMs = request.receiveTimeoutMs,
                    errorCode = "receiveTimeout",
                    message = "Response receive timed out after ${request.receiveTimeoutMs}ms."
                )

                urlRequest.read(byteBuffer)
            }

            override fun onSucceeded(urlRequest: UrlRequest, info: UrlResponseInfo) {
                finish(
                    TurboHttpResponse(
                        statusCode = info.httpStatusCode.toLong(),
                        statusMessage = info.httpStatusText,
                        headers = normalizeHeaders(info),
                        body = outputStream.toByteArray()
                    )
                )
                engineHandle.release()
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
                finish(
                    TurboHttpResponse(
                        errorCode = errorCode,
                        errorMessage = error?.message ?: "Cronet request failed."
                    )
                )
                engineHandle.release()
            }

            override fun onCanceled(urlRequest: UrlRequest, info: UrlResponseInfo?) {
                finish(
                    TurboHttpResponse(
                        errorCode = "cancel",
                        errorMessage = "Request was cancelled."
                    )
                )
                engineHandle.release()
            }
        }

        private inner class TrackingUploadDataProvider(
            private val bytes: ByteArray
        ) : UploadDataProvider() {
            private var offset = 0
            private var uploadStarted = false

            override fun getLength(): Long = bytes.size.toLong()

            override fun read(uploadDataSink: UploadDataSink, byteBuffer: ByteBuffer) {
                if (!uploadStarted) {
                    uploadStarted = true
                    phase = RequestPhase.SENDING_BODY
                    schedulePhaseTimer(
                        delayMs = request.sendTimeoutMs,
                        errorCode = "sendTimeout",
                        message = "Request send timed out after ${request.sendTimeoutMs}ms."
                    )
                }

                val remainingBytes = bytes.size - offset
                val toWrite = minOf(byteBuffer.remaining(), remainingBytes)
                if (toWrite > 0) {
                    byteBuffer.put(bytes, offset, toWrite)
                    offset += toWrite
                }

                if (offset >= bytes.size) {
                    phase = RequestPhase.RECEIVING
                    schedulePhaseTimer(
                        delayMs = request.receiveTimeoutMs,
                        errorCode = "receiveTimeout",
                        message = "Response receive timed out after ${request.receiveTimeoutMs}ms."
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
        val grouped = HashMap<String?, MutableList<String>>()
        for (entry in info.allHeadersAsList) {
            val key = entry.key.lowercase(Locale.ROOT)
            grouped.getOrPut(key) { mutableListOf() }.add(entry.value)
        }
        val result = HashMap<String?, Any?>(grouped.size)
        for ((k, v) in grouped) {
            result[k] = v
        }
        return result
    }

    private fun extractContentLength(info: UrlResponseInfo): Long? {
        for (entry in info.allHeadersAsList) {
            if (entry.key.equals("content-length", ignoreCase = true)) {
                return entry.value.trim().toLongOrNull()
            }
        }
        return null
    }

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

        private fun defaultCallbackExecutor(): Executor =
            Executors.newCachedThreadPool { runnable ->
                Thread(runnable, "turbo-fetch-cronet-callback").apply {
                    isDaemon = true
                }
            }
    }
}
