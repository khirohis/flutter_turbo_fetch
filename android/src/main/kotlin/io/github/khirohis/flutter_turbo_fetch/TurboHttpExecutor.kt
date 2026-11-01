package io.github.khirohis.flutter_turbo_fetch

import okhttp3.Call
import okhttp3.Callback
import okhttp3.ConnectionPool
import okhttp3.Dispatcher
import okhttp3.Headers
import okhttp3.MediaType
import okhttp3.OkHttpClient
import okhttp3.Protocol
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
import java.security.SecureRandom
import java.security.cert.CertificateException
import java.security.cert.X509Certificate
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.TimeUnit
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLException
import javax.net.ssl.TrustManager
import javax.net.ssl.X509TrustManager

/**
 * OkHttp communication and resource management layer responsible for shared client pooling,
 * in-flight call tracking, defensive request building, one-shot response body buffering,
 * and error classification.
 */
internal open class TurboHttpExecutor(
    private val clientFactory: (Dispatcher) -> OkHttpClient = ::createDefaultBaseClient
) {
    constructor(baseClient: OkHttpClient) : this({ baseClient })

    private val activeCalls = ConcurrentHashMap<String, Call>()
    private val clientLock = Any()

    private var dispatcher: Dispatcher = createDispatcher()
    private var baseClient: OkHttpClient = clientFactory(dispatcher)

    // -------------------------------------------------------------------------
    // Lifecycle & Client Management
    // -------------------------------------------------------------------------

    open fun ensureReady() {
        synchronized(clientLock) {
            if (dispatcher.executorService.isShutdown) {
                dispatcher = createDispatcher()
                baseClient = clientFactory(dispatcher)
            }
        }
    }

    open fun shutdown() {
        for (call in activeCalls.values) {
            call.cancel()
        }
        activeCalls.clear()
        synchronized(clientLock) {
            dispatcher.executorService.shutdown()
            baseClient.connectionPool.evictAll()
        }
    }

    open fun cancel(requestId: String) {
        activeCalls.remove(requestId)?.cancel()
    }

    internal fun ensureClient(requestedPerHost: Int): OkHttpClient {
        synchronized(clientLock) {
            if (dispatcher.executorService.isShutdown) {
                dispatcher = createDispatcher()
                baseClient = clientFactory(dispatcher)
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

    // -------------------------------------------------------------------------
    // Request Execution Pipeline
    // -------------------------------------------------------------------------

    open fun execute(
        request: TurboHttpRequest,
        onResult: (TurboHttpResponse) -> Unit
    ) {
        val requestedPerHost = request.maxRequestsPerHost.toInt().coerceAtLeast(1)
        val sharedClient = ensureClient(requestedPerHost)
        val requestClient = buildRequestClient(sharedClient, request)
        val sendPhaseTracker = SendPhaseTracker()

        val okRequest = try {
            buildOkRequest(request, sendPhaseTracker)
        } catch (e: Exception) {
            val errorResponse = TurboHttpResponse(
                errorCode = "connectionError",
                errorMessage = e.message ?: "Invalid request URL or parameters"
            )
            onResult(errorResponse)
            return
        }

        val call = requestClient.newCall(okRequest)
        activeCalls[request.requestId] = call

        call.enqueue(
            createCallback(
                request = request,
                sendPhaseTracker = sendPhaseTracker,
                onResult = { response ->
                    activeCalls.remove(request.requestId)
                    onResult(response)
                }
            )
        )
    }

    private fun buildRequestClient(
        sharedClient: OkHttpClient,
        request: TurboHttpRequest
    ): OkHttpClient {
        val clientBuilder = sharedClient.newBuilder()
            .followRedirects(request.followRedirects)
            .followSslRedirects(request.followRedirects)

        request.connectTimeoutMs?.let {
            clientBuilder.connectTimeout(it.coerceAtLeast(0L), TimeUnit.MILLISECONDS)
        }
        request.receiveTimeoutMs?.let {
            clientBuilder.readTimeout(it.coerceAtLeast(0L), TimeUnit.MILLISECONDS)
        }
        request.sendTimeoutMs?.let {
            clientBuilder.writeTimeout(it.coerceAtLeast(0L), TimeUnit.MILLISECONDS)
        }

        if (request.allowBadCertificates) {
            configureInsecureTls(clientBuilder)
        }

        return clientBuilder.build()
    }

    private fun buildOkRequest(
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
        override fun onFailure(call: Call, e: IOException) {
            val errorCode = classifyException(call, e, sendPhaseTracker.isSendingBody)
            val turboResponse = TurboHttpResponse(
                errorCode = errorCode,
                errorMessage = e.message ?: e.javaClass.simpleName
            )
            onResult(turboResponse)
        }

        override fun onResponse(call: Call, response: Response) {
            try {
                response.use { res ->
                    val turboResponse = readAndBuildResponse(res, request.maxResponseBytes)
                    onResult(turboResponse)
                }
            } catch (e: IOException) {
                val errorCode = classifyException(call, e, sendPhaseTracker.isSendingBody)
                val turboResponse = TurboHttpResponse(
                    errorCode = errorCode,
                    errorMessage = e.message ?: e.javaClass.simpleName
                )
                onResult(turboResponse)
            } catch (e: Throwable) {
                val errorCode = if (call.isCanceled()) "cancel" else "unknown"
                val turboResponse = TurboHttpResponse(
                    errorCode = errorCode,
                    errorMessage = e.message ?: e.javaClass.simpleName
                )
                onResult(turboResponse)
            }
        }
    }

    private fun readAndBuildResponse(
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
    // Error Classification & TLS Helpers
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

    private fun configureInsecureTls(builder: OkHttpClient.Builder) {
        val trustAllCerts = arrayOf<TrustManager>(
            object : X509TrustManager {
                override fun checkClientTrusted(chain: Array<X509Certificate>, authType: String) {}
                override fun checkServerTrusted(chain: Array<X509Certificate>, authType: String) {}
                override fun getAcceptedIssuers(): Array<X509Certificate> = arrayOf()
            }
        )
        val sslContext = SSLContext.getInstance("TLS")
        sslContext.init(null, trustAllCerts, SecureRandom())
        val trustManager = trustAllCerts[0] as X509TrustManager
        builder.sslSocketFactory(sslContext.socketFactory, trustManager)
        builder.hostnameVerifier { _, _ -> true }
    }

    private class SendPhaseTracker {
        @Volatile
        var isSendingBody: Boolean = false
    }

    private class TrackingRequestBody(
        private val delegate: RequestBody,
        private val tracker: SendPhaseTracker
    ) : RequestBody() {
        override fun contentType(): MediaType? = delegate.contentType()

        override fun contentLength(): Long = delegate.contentLength()

        override fun writeTo(sink: BufferedSink) {
            tracker.isSendingBody = true
            try {
                delegate.writeTo(sink)
                sink.flush()
                tracker.isSendingBody = false
            } catch (e: IOException) {
                throw e
            }
        }
    }

    companion object {
        private const val DEFAULT_MAX_REQUESTS = 64
        private const val DEFAULT_MAX_REQUESTS_PER_HOST = 32

        private fun createDispatcher(): Dispatcher = Dispatcher().apply {
            maxRequests = DEFAULT_MAX_REQUESTS
            maxRequestsPerHost = DEFAULT_MAX_REQUESTS_PER_HOST
        }

        private fun createDefaultBaseClient(targetDispatcher: Dispatcher): OkHttpClient =
            OkHttpClient.Builder()
                .dispatcher(targetDispatcher)
                .connectionPool(ConnectionPool(5, 5, TimeUnit.MINUTES))
                .protocols(listOf(Protocol.HTTP_2, Protocol.HTTP_1_1))
                .build()
    }
}
