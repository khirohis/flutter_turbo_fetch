package io.github.khirohis.flutter_turbo_fetch

import okhttp3.ConnectionPool
import okhttp3.Dispatcher
import okhttp3.OkHttpClient
import okhttp3.Protocol
import java.security.SecureRandom
import java.security.cert.X509Certificate
import java.util.concurrent.TimeUnit
import javax.net.ssl.SSLContext
import javax.net.ssl.TrustManager
import javax.net.ssl.X509TrustManager

/**
 * OkHttp client and connection pool manager responsible for thread-safe dispatcher scaling,
 * post-shutdown recovery, and per-request client derivation (timeouts, redirects, and TLS).
 */
internal open class TurboClientManager(
    private val clientFactory: (Dispatcher) -> OkHttpClient = ::createDefaultBaseClient
) {
    constructor(baseClient: OkHttpClient) : this({ baseClient })

    private val clientLock = Any()
    private var dispatcher: Dispatcher = createDispatcher()
    private var baseClient: OkHttpClient = clientFactory(dispatcher)

    open fun ensureReady() {
        synchronized(clientLock) {
            if (dispatcher.executorService.isShutdown) {
                dispatcher = createDispatcher()
                baseClient = clientFactory(dispatcher)
            }
        }
    }

    open fun shutdown() {
        synchronized(clientLock) {
            dispatcher.executorService.shutdown()
            baseClient.connectionPool.evictAll()
        }
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

    open fun acquireClient(request: TurboHttpRequest): OkHttpClient {
        val requestedPerHost = request.maxRequestsPerHost.toInt().coerceAtLeast(1)
        val sharedClient = ensureClient(requestedPerHost)

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

