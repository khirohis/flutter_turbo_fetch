package io.github.khirohis.flutter_turbo_fetch

import okhttp3.Call
import okhttp3.Callback
import okhttp3.Headers
import okhttp3.MediaType
import okhttp3.Request
import okhttp3.RequestBody
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import okhttp3.internal.http.HttpMethod
import okio.BufferedSink
import java.io.IOException
import java.io.InterruptedIOException
import java.net.ConnectException
import java.net.NoRouteToHostException
import java.net.ProtocolException
import java.net.SocketException
import java.net.SocketTimeoutException
import java.net.UnknownHostException
import java.net.UnknownServiceException
import java.security.cert.CertificateException
import java.util.concurrent.ConcurrentHashMap
import javax.net.ssl.SSLException

/**
 * OkHttp request execution and in-flight call management layer responsible for
 * defensive request building, one-shot response body buffering, and error classification.
 */
internal open class TurboHttpExecutor(
    private val clientManager: TurboClientManager = TurboClientManager()
) {
    private val activeCalls = ConcurrentHashMap<String, Call>()

    // -------------------------------------------------------------------------
    // Lifecycle & Call Management
    // -------------------------------------------------------------------------

    open fun ensureReady() {
        clientManager.ensureReady()
    }

    open fun shutdown() {
        for (call in activeCalls.values) {
            call.cancel()
        }
        activeCalls.clear()
        clientManager.shutdown()
    }

    open fun cancel(requestId: String) {
        activeCalls.remove(requestId)?.cancel()
    }

    // -------------------------------------------------------------------------
    // Request Execution Pipeline
    // -------------------------------------------------------------------------

    open fun execute(
        request: TurboHttpRequest,
        onResult: (TurboHttpResponse) -> Unit
    ) {
        val requestClient = clientManager.acquireClient(request)
        val sendPhaseTracker = SendPhaseTracker()

        val httpRequest = try {
            buildRequest(request, sendPhaseTracker)
        } catch (e: Exception) {
            val errorResponse = TurboHttpResponse(
                errorCode = "connectionError",
                errorMessage = e.message ?: "Invalid request URL or parameters"
            )
            onResult(errorResponse)
            return
        }

        val call = requestClient.newCall(httpRequest)
        activeCalls[request.requestId] = call

        call.enqueue(createCallback(request, sendPhaseTracker, onResult))
    }

    private fun buildRequest(
        request: TurboHttpRequest,
        sendPhaseTracker: SendPhaseTracker
    ): Request {
        val headersBuilder = Headers.Builder()
        for ((key, value) in request.headers) {
            if (key != null && value != null) {
                headersBuilder.add(key, value)
            }
        }

        val method = request.method.uppercase()
        val requestBody: RequestBody? = when {
            !HttpMethod.permitsRequestBody(method) -> null
            request.body != null -> TrackingRequestBody(
                delegate = request.body.toRequestBody(null),
                tracker = sendPhaseTracker
            )
            HttpMethod.requiresRequestBody(method) -> ByteArray(0).toRequestBody(null)
            else -> null
        }

        return Request.Builder()
            .url(request.url)
            .headers(headersBuilder.build())
            .method(method, requestBody)
            .build()
    }

    private fun createCallback(
        request: TurboHttpRequest,
        sendPhaseTracker: SendPhaseTracker,
        onResult: (TurboHttpResponse) -> Unit
    ): Callback = object : Callback {
        private fun complete(response: TurboHttpResponse) {
            activeCalls.remove(request.requestId)
            onResult(response)
        }

        private fun completeWithIoError(call: Call, e: IOException) {
            val errorCode = classifyException(call, e, sendPhaseTracker.isSendingBody)
            complete(
                TurboHttpResponse(
                    errorCode = errorCode,
                    errorMessage = e.message ?: e.javaClass.simpleName
                )
            )
        }

        override fun onFailure(call: Call, e: IOException) {
            completeWithIoError(call, e)
        }

        override fun onResponse(call: Call, response: Response) {
            try {
                response.use { res ->
                    complete(buildResponse(res, request.maxResponseBytes))
                }
            } catch (e: IOException) {
                completeWithIoError(call, e)
            } catch (e: Throwable) {
                val errorCode = if (call.isCanceled()) "cancel" else "unknown"
                complete(
                    TurboHttpResponse(
                        errorCode = errorCode,
                        errorMessage = e.message ?: e.javaClass.simpleName
                    )
                )
            }
        }
    }

    private fun buildResponse(
        res: Response,
        maxBytes: Long?
    ): TurboHttpResponse {
        val bodyBytes: ByteArray
        if (maxBytes == null) {
            // Unlimited: read & concatenate all HTTP/2 frames into a single ByteArray
            bodyBytes = res.body?.bytes() ?: ByteArray(0)
        } else {
            val safeMaxBytes = maxBytes.coerceAtLeast(0L)
            val responseBody = res.body
            if (responseBody == null) {
                bodyBytes = ByteArray(0)
            } else {
                val contentLength = responseBody.contentLength()
                if (contentLength > safeMaxBytes) {
                    return TurboHttpResponse(
                        errorCode = "maxResponseBytesExceeded",
                        errorMessage = "Response body exceeded maxResponseBytes limit ($safeMaxBytes bytes, contentLength: $contentLength)"
                    )
                }

                val source = responseBody.source()
                if (safeMaxBytes < Long.MAX_VALUE && source.request(safeMaxBytes + 1L)) {
                    return TurboHttpResponse(
                        errorCode = "maxResponseBytesExceeded",
                        errorMessage = "Response body exceeded maxResponseBytes limit ($safeMaxBytes bytes)"
                    )
                }
                bodyBytes = source.readByteArray()
            }
        }

        val rawMultimap = res.headers.toMultimap()
        val headersMap = HashMap<String?, Any?>(rawMultimap.size)
        for ((k, v) in rawMultimap) {
            headersMap[k] = v
        }

        return TurboHttpResponse(
            statusCode = res.code.toLong(),
            statusMessage = res.message,
            headers = headersMap,
            body = bodyBytes
        )
    }

    // -------------------------------------------------------------------------
    // Error Classification & Tracking Helpers
    // -------------------------------------------------------------------------

    internal fun classifyException(
        call: Call,
        e: IOException,
        isSendingBody: Boolean
    ): String {
        if (call.isCanceled()) {
            return "cancel"
        }
        return when {
            e is SocketTimeoutException -> {
                val msg = e.message?.lowercase() ?: ""
                when {
                    msg.contains("connect") -> "connectionTimeout"
                    isSendingBody -> "sendTimeout"
                    else -> "receiveTimeout"
                }
            }
            e is InterruptedIOException -> {
                val msg = e.message?.lowercase() ?: ""
                if (msg.contains("timeout")) {
                    if (isSendingBody) "sendTimeout" else "receiveTimeout"
                } else {
                    "cancel"
                }
            }
            e is SSLException || e.cause is CertificateException || e.cause is SSLException -> {
                "badCertificate"
            }
            e is UnknownHostException ||
                e is ConnectException ||
                e is NoRouteToHostException ||
                e is SocketException ||
                e is ProtocolException ||
                e is UnknownServiceException -> "connectionError"
            else -> "unknown"
        }
    }

    /**
     * Tracks whether the request is currently in the body-sending phase so that
     * [classifyException] can distinguish `sendTimeout` from `receiveTimeout`
     * (OkHttp throws the same timeout exceptions for both phases).
     */
    private class SendPhaseTracker {
        @Volatile
        var isSendingBody: Boolean = false
    }

    /**
     * Wraps a [RequestBody] to mark [SendPhaseTracker.isSendingBody] as `true`
     * while writing the request payload to the network sink. If an [IOException]
     * occurs during [writeTo], the flag remains `true` so the failure is classified
     * as `sendTimeout`.
     */
    private class TrackingRequestBody(
        private val delegate: RequestBody,
        private val tracker: SendPhaseTracker
    ) : RequestBody() {
        override fun contentType(): MediaType? = delegate.contentType()

        override fun contentLength(): Long = delegate.contentLength()

        override fun writeTo(sink: BufferedSink) {
            tracker.isSendingBody = true
            delegate.writeTo(sink)
            sink.flush()
            tracker.isSendingBody = false
        }
    }
}
