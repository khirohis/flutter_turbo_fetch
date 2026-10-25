package io.github.khirohis.flutter_turbo_fetch

import android.os.Handler
import android.os.Looper
import io.flutter.embedding.engine.plugins.FlutterPlugin
import okhttp3.Call
import okhttp3.Callback
import okhttp3.ConnectionPool
import okhttp3.Dispatcher
import okhttp3.Headers
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Request
import okhttp3.RequestBody
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import okhttp3.internal.http.HttpMethod
import java.io.IOException
import java.io.InterruptedIOException
import java.net.ConnectException
import java.net.SocketTimeoutException
import java.net.UnknownHostException
import java.security.SecureRandom
import java.security.cert.X509Certificate
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.TimeUnit
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLException
import javax.net.ssl.TrustManager
import javax.net.ssl.X509TrustManager

/** FlutterTurboFetchPlugin */
class FlutterTurboFetchPlugin :
    FlutterPlugin,
    TurboFetchHostApi {

    private val mainHandler = Handler(Looper.getMainLooper())
    private val activeCalls = ConcurrentHashMap<String, Call>()

    private val dispatcher = Dispatcher().apply {
        maxRequests = DEFAULT_MAX_REQUESTS
        maxRequestsPerHost = DEFAULT_MAX_REQUESTS_PER_HOST
    }

    private val baseClient: OkHttpClient = OkHttpClient.Builder()
        .dispatcher(dispatcher)
        .connectionPool(ConnectionPool(5, 5, TimeUnit.MINUTES))
        .protocols(listOf(Protocol.HTTP_2, Protocol.HTTP_1_1))
        .build()

    override fun onAttachedToEngine(flutterPluginBinding: FlutterPlugin.FlutterPluginBinding) {
        TurboFetchHostApi.setUp(flutterPluginBinding.binaryMessenger, this)
    }

    override fun onDetachedFromEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        TurboFetchHostApi.setUp(binding.binaryMessenger, null)
        for (call in activeCalls.values) {
            call.cancel()
        }
        activeCalls.clear()
    }

    override fun executeRequest(
        request: TurboHttpRequest,
        callback: (Result<TurboHttpResponse>) -> Unit
    ) {
        val requestedPerHost = request.maxRequestsPerHost.toInt().coerceAtLeast(1)
        synchronized(dispatcher) {
            if (requestedPerHost > dispatcher.maxRequests) {
                dispatcher.maxRequests = requestedPerHost
            } else if (dispatcher.maxRequests > DEFAULT_MAX_REQUESTS && requestedPerHost <= DEFAULT_MAX_REQUESTS) {
                dispatcher.maxRequests = DEFAULT_MAX_REQUESTS
            }
            if (dispatcher.maxRequestsPerHost != requestedPerHost) {
                dispatcher.maxRequestsPerHost = requestedPerHost
            }
        }

        val clientBuilder = baseClient.newBuilder()
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

        val client = clientBuilder.build()

        val headersBuilder = Headers.Builder()
        for ((key, value) in request.headers) {
            if (key != null && value != null) {
                headersBuilder.add(key, value)
            }
        }

        val method = request.method.uppercase()
        val requestBody: RequestBody? = when {
            !HttpMethod.permitsRequestBody(method) -> null
            request.body != null -> request.body.toRequestBody(null)
            HttpMethod.requiresRequestBody(method) -> ByteArray(0).toRequestBody(null)
            else -> null
        }

        val okRequest = try {
            Request.Builder()
                .url(request.url)
                .headers(headersBuilder.build())
                .method(method, requestBody)
                .build()
        } catch (e: Exception) {
            val errorResponse = TurboHttpResponse(
                errorType = "connectionError",
                errorMessage = e.message ?: "Invalid request URL or parameters"
            )
            mainHandler.post {
                callback(Result.success(errorResponse))
            }
            return
        }

        val call = client.newCall(okRequest)
        activeCalls[request.requestId] = call

        call.enqueue(object : Callback {
            override fun onFailure(call: Call, e: IOException) {
                activeCalls.remove(request.requestId)
                val errorType = classifyException(call, e)
                val turboResponse = TurboHttpResponse(
                    errorType = errorType,
                    errorMessage = e.message ?: e.javaClass.simpleName
                )
                mainHandler.post {
                    callback(Result.success(turboResponse))
                }
            }

            override fun onResponse(call: Call, response: Response) {
                try {
                    response.use { res ->
                        // Native background thread: read & concatenate all HTTP/2 frames into a single ByteArray
                        val bodyBytes = res.body?.bytes() ?: ByteArray(0)
                        val rawMultimap = res.headers.toMultimap()
                        val headersMap = HashMap<String?, Any?>(rawMultimap.size)
                        for ((k, v) in rawMultimap) {
                            headersMap[k] = v
                        }

                        val turboResponse = TurboHttpResponse(
                            statusCode = res.code.toLong(),
                            statusMessage = res.message,
                            headers = headersMap,
                            body = bodyBytes
                        )
                        activeCalls.remove(request.requestId)
                        mainHandler.post {
                            callback(Result.success(turboResponse))
                        }
                    }
                } catch (e: IOException) {
                    activeCalls.remove(request.requestId)
                    val errorType = classifyException(call, e)
                    val turboResponse = TurboHttpResponse(
                        errorType = errorType,
                        errorMessage = e.message ?: e.javaClass.simpleName
                    )
                    mainHandler.post {
                        callback(Result.success(turboResponse))
                    }
                }
            }
        })
    }

    override fun cancelRequest(requestId: String) {
        activeCalls.remove(requestId)?.cancel()
    }

    private fun classifyException(call: Call, e: IOException): String {
        if (call.isCanceled()) {
            return "cancel"
        }
        return when (e) {
            is SocketTimeoutException -> {
                val msg = e.message?.lowercase() ?: ""
                if (msg.contains("connect")) "connectionTimeout" else "receiveTimeout"
            }
            is InterruptedIOException -> "cancel"
            is SSLException -> "badCertificate"
            is UnknownHostException, is ConnectException -> "connectionError"
            else -> "connectionError"
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

    companion object {
        private const val DEFAULT_MAX_REQUESTS = 64
        private const val DEFAULT_MAX_REQUESTS_PER_HOST = 32
    }
}
