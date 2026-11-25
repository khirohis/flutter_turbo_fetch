package io.github.khirohis.flutter_turbo_fetch

import android.content.Context
import okhttp3.Call
import okhttp3.Callback
import okhttp3.Headers
import okhttp3.MediaType
import okhttp3.Request
import okhttp3.RequestBody
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import okhttp3.ResponseBody
import okhttp3.internal.http.HttpMethod
import okio.BufferedSink
import okio.BufferedSource
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
import java.util.Locale
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicBoolean
import javax.net.ssl.SSLException

internal fun createDefaultTurboHttpExecutor(): TurboHttpExecutor = OkHttpTurboHttpExecutor()

/**
 * OkHttp request execution and in-flight call management layer responsible for
 * defensive request building, one-shot response body buffering, and error classification.
 */
internal open class OkHttpTurboHttpExecutor(
    private val clientManager: OkHttpClientManager = OkHttpClientManager()
) : TurboHttpExecutor {
    private val activeCalls = ConcurrentHashMap<String, Call>()

    internal val activeCallCount: Int
        get() = activeCalls.size

    internal fun hasActiveCall(requestId: String): Boolean = activeCalls.containsKey(requestId)

    // -------------------------------------------------------------------------
    // Lifecycle & Call Management
    // -------------------------------------------------------------------------

    override fun ensureReady(context: Context?) {
        clientManager.ensureReady()
    }

    override fun shutdown() {
        for (requestId in activeCalls.keys) {
            activeCalls.remove(requestId)?.cancel()
        }
        clientManager.shutdown()
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
        val httpRequest: Request
        val call = try {
            httpRequest = buildRequest(request)
            val requestClient = clientManager.acquireClient(request)
            requestClient.newCall(httpRequest)
        } catch (e: Exception) {
            val errorResponse = TurboHttpResponse(
                errorCode = "connectionError",
                errorMessage = e.message ?: "Invalid request URL or parameters"
            )
            onResult(errorResponse)
            return
        }

        val trackingBody = httpRequest.body as? TrackingRequestBody
        activeCalls[request.requestId] = call

        val completed = AtomicBoolean(false)
        val finishOnce: (TurboHttpResponse) -> Unit = { response ->
            if (completed.compareAndSet(false, true)) {
                activeCalls.remove(request.requestId, call)
                onResult(response)
            }
        }

        try {
            call.enqueue(createCallback(request, trackingBody, finishOnce))
        } catch (t: Throwable) {
            val errorCode = if (call.isCanceled()) "cancel" else "unknown"
            finishOnce(
                TurboHttpResponse(
                    errorCode = errorCode,
                    errorMessage = t.message ?: t.javaClass.simpleName
                )
            )
        }
    }

    private fun buildRequest(request: TurboHttpRequest): Request {
        val headersBuilder = Headers.Builder()
        for ((key, value) in request.headers) {
            if (key != null && value != null) {
                headersBuilder.add(key, value)
            }
        }

        val method = request.method.uppercase(Locale.ROOT)
        val requestBody: RequestBody? = when {
            !HttpMethod.permitsRequestBody(method) -> null
            request.body != null -> TrackingRequestBody(request.body.toRequestBody(null))
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
        trackingBody: TrackingRequestBody?,
        finishOnce: (TurboHttpResponse) -> Unit
    ): Callback = object : Callback {
        private fun buildIoErrorResponse(call: Call, e: IOException): TurboHttpResponse {
            val errorCode = classifyException(call, e, trackingBody?.isSendingBody == true)
            return TurboHttpResponse(
                errorCode = errorCode,
                errorMessage = e.message ?: e.javaClass.simpleName
            )
        }

        override fun onFailure(call: Call, e: IOException) {
            finishOnce(buildIoErrorResponse(call, e))
        }

        override fun onResponse(call: Call, response: Response) {
            val result = try {
                try {
                    buildResponse(response, request.maxResponseBytes)
                } finally {
                    runCatching { response.close() }
                }
            } catch (e: IOException) {
                buildIoErrorResponse(call, e)
            } catch (e: Throwable) {
                val errorCode = if (call.isCanceled()) "cancel" else "unknown"
                TurboHttpResponse(
                    errorCode = errorCode,
                    errorMessage = e.message ?: e.javaClass.simpleName
                )
            }
            finishOnce(result)
        }
    }

    private fun buildResponse(
        res: Response,
        maxBytes: Long?
    ): TurboHttpResponse {
        val responseBody = res.body
        val bodyBytes: ByteArray = if (responseBody == null) {
            ByteArray(0)
        } else {
            val safeMaxBytes = maxBytes?.coerceAtLeast(0L)
            when (val readResult = readResponseBody(responseBody, safeMaxBytes)) {
                is BodyReadResult.Success -> readResult.bytes
                is BodyReadResult.Exceeded -> return TurboHttpResponse(
                    errorCode = "maxResponseBytesExceeded",
                    errorMessage = readResult.message
                )
            }
        }

        return TurboHttpResponse(
            statusCode = res.code.toLong(),
            statusMessage = res.message,
            headers = normalizeHeaders(res.headers),
            body = bodyBytes
        )
    }

    private fun normalizeHeaders(headers: Headers): Map<String?, Any?> {
        val headerCount = headers.size
        val result = HashMap<String?, MutableList<String>>(headerCount)
        for (i in 0 until headerCount) {
            val name = headers.name(i).lowercase(Locale.ROOT)
            val value = headers.value(i)
            result.getOrPut(name) { ArrayList(1) }.add(value)
        }
        return result
    }

    private sealed class BodyReadResult {
        class Success(val bytes: ByteArray) : BodyReadResult()
        class Exceeded(val message: String) : BodyReadResult()
    }

    private fun exceededResult(
        maxBytes: Long,
        contentLength: Long? = null
    ): BodyReadResult.Exceeded {
        val detail = if (contentLength != null) {
            " ($maxBytes bytes, contentLength: $contentLength)"
        } else {
            " ($maxBytes bytes)"
        }
        return BodyReadResult.Exceeded("Response body exceeded maxResponseBytes limit$detail")
    }

    private fun readResponseBody(
        responseBody: ResponseBody,
        safeMaxBytes: Long?
    ): BodyReadResult {
        val contentLength = responseBody.contentLength()
        if (safeMaxBytes != null && contentLength > safeMaxBytes) {
            return exceededResult(safeMaxBytes, contentLength)
        }
        if (contentLength > Int.MAX_VALUE) {
            throw IOException("Response body length ($contentLength) exceeds maximum ByteArray size")
        }

        val source = responseBody.source()
        val chunks = ArrayList<ByteArray>(4)
        var totalCount = 0L
        var nextChunkSize = if (contentLength in 1L..MAX_DIRECT_ALLOC_BYTES) {
            contentLength.toInt()
        } else {
            INITIAL_READ_CHUNK_SIZE
        }

        while (true) {
            val remainingAllowed = if (safeMaxBytes != null) {
                val remaining = safeMaxBytes - totalCount
                if (remaining <= 0L) {
                    return if (!source.exhausted()) {
                        exceededResult(safeMaxBytes)
                    } else {
                        BodyReadResult.Success(mergeChunks(chunks, totalCount.toInt()))
                    }
                }
                remaining
            } else {
                Long.MAX_VALUE
            }

            val capacity = minOf(nextChunkSize.toLong(), remainingAllowed).toInt()
            val chunk = readChunk(source, capacity) ?: break

            totalCount += chunk.size.toLong()
            if (totalCount > Int.MAX_VALUE) {
                throw IOException("Response body exceeds maximum ByteArray size")
            }
            chunks.add(chunk)

            if (chunk.size < capacity) {
                break
            }
            nextChunkSize = if (nextChunkSize < MAX_READ_CHUNK_SIZE) {
                (nextChunkSize * 2).coerceAtMost(MAX_READ_CHUNK_SIZE)
            } else {
                MAX_READ_CHUNK_SIZE
            }
        }

        return BodyReadResult.Success(mergeChunks(chunks, totalCount.toInt()))
    }

    private fun readChunk(source: BufferedSource, capacity: Int): ByteArray? {
        if (source.exhausted()) {
            return null
        }
        val buffer = ByteArray(capacity)
        var filled = 0
        while (filled < capacity) {
            val bytesRead = source.read(buffer, filled, capacity - filled)
            if (bytesRead == -1) {
                break
            }
            filled += bytesRead
        }
        return when (filled) {
            0 -> null
            capacity -> buffer
            else -> buffer.copyOf(filled)
        }
    }

    private fun mergeChunks(chunks: List<ByteArray>, totalSize: Int): ByteArray {
        return when (chunks.size) {
            0 -> ByteArray(0)
            1 -> chunks[0]
            else -> {
                val merged = ByteArray(totalSize)
                var offset = 0
                for (chunk in chunks) {
                    System.arraycopy(chunk, 0, merged, offset, chunk.size)
                    offset += chunk.size
                }
                merged
            }
        }
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
        return when (e) {
            is SocketTimeoutException -> {
                val msg = e.message?.lowercase(Locale.ROOT) ?: ""
                when {
                    msg.contains("connect") -> "connectionTimeout"
                    isSendingBody -> "sendTimeout"
                    else -> "receiveTimeout"
                }
            }
            is InterruptedIOException -> {
                val msg = e.message?.lowercase(Locale.ROOT) ?: ""
                if (msg.contains("timeout")) {
                    if (isSendingBody) "sendTimeout" else "receiveTimeout"
                } else {
                    "cancel"
                }
            }
            is SSLException -> "badCertificate"
            is UnknownHostException,
            is ConnectException,
            is NoRouteToHostException,
            is SocketException,
            is ProtocolException,
            is UnknownServiceException -> "connectionError"
            else -> {
                if (e.cause is CertificateException || e.cause is SSLException) {
                    "badCertificate"
                } else {
                    "unknown"
                }
            }
        }
    }

    /**
     * Wraps a [RequestBody] to track whether the request is currently in the body-sending phase so
     * [classifyException] can distinguish `sendTimeout` from `receiveTimeout`. If an [IOException]
     * occurs during [writeTo], [isSendingBody] remains `true`.
     */
    private class TrackingRequestBody(
        private val delegate: RequestBody
    ) : RequestBody() {
        @Volatile
        var isSendingBody: Boolean = false

        override fun contentType(): MediaType? = delegate.contentType()

        override fun contentLength(): Long = delegate.contentLength()

        override fun writeTo(sink: BufferedSink) {
            isSendingBody = true
            delegate.writeTo(sink)
            sink.flush()
            isSendingBody = false
        }
    }

    companion object {
        private const val INITIAL_READ_CHUNK_SIZE = 8 * 1024
        private const val MAX_READ_CHUNK_SIZE = 64 * 1024
        private const val MAX_DIRECT_ALLOC_BYTES = 4L * 1024L * 1024L
    }
}
