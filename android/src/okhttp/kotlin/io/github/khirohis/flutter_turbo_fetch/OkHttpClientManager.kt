package io.github.khirohis.flutter_turbo_fetch

import okhttp3.ConnectionPool
import okhttp3.Dispatcher
import okhttp3.OkHttpClient
import okhttp3.Protocol
import java.security.SecureRandom
import java.security.cert.X509Certificate
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.TimeUnit
import javax.net.ssl.HostnameVerifier
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLSocketFactory
import javax.net.ssl.TrustManager
import javax.net.ssl.X509TrustManager

/**
 * OkHttp client and connection pool manager responsible for maintaining fixed HTTP/2 dispatcher limits,
 * post-shutdown recovery, and bounded caching of per-request derived clients (timeouts, redirects, and TLS).
 */
internal open class OkHttpClientManager(
    private val clientFactory: (Dispatcher) -> OkHttpClient = ::createDefaultBaseClient
) {
    constructor(baseClient: OkHttpClient) : this({ targetDispatcher ->
        baseClient.newBuilder().dispatcher(targetDispatcher).build()
    })

    private data class DerivedClientKey(
        val connectTimeoutMs: Long?,
        val receiveTimeoutMs: Long?,
        val sendTimeoutMs: Long?,
        val followRedirects: Boolean,
        val allowBadCertificates: Boolean
    )

    private class DerivedClientCache(
        private val maxEntries: Int
    ) {
        private val entries = ConcurrentHashMap<DerivedClientKey, OkHttpClient>()
        private val insertionOrder = ArrayDeque<DerivedClientKey>()

        val size: Int
            get() = entries.size

        inline fun getOrPut(
            key: DerivedClientKey,
            create: () -> OkHttpClient
        ): OkHttpClient {
            entries[key]?.let { return it }
            val created = create()
            synchronized(entries) {
                entries[key]?.let { return it }
                if (entries.size >= maxEntries) {
                    insertionOrder.removeFirstOrNull()?.let { entries.remove(it) }
                }
                entries[key] = created
                insertionOrder.addLast(key)
                return created
            }
        }

        fun clear() {
            synchronized(entries) {
                entries.clear()
                insertionOrder.clear()
            }
        }
    }

    private class ClientState(
        val baseClient: OkHttpClient,
        val derivedClients: DerivedClientCache = DerivedClientCache(MAX_DERIVED_CLIENTS)
    ) {
        val isShutdown: Boolean
            get() = baseClient.dispatcher.executorService.isShutdown
    }

    @Volatile
    private var state: ClientState = createState()

    internal val derivedClientCacheSize: Int
        get() = state.derivedClients.size

    private fun createState(): ClientState = ClientState(
        baseClient = clientFactory(createDispatcher())
    )

    private fun ensureActiveState(): ClientState {
        val current = state
        if (!current.isShutdown) {
            return current
        }
        synchronized(this) {
            val latest = state
            if (!latest.isShutdown) {
                return latest
            }
            val recreated = createState()
            state = recreated
            return recreated
        }
    }

    open fun ensureReady() {
        ensureActiveState()
    }

    @Synchronized
    open fun shutdown() {
        val current = state
        current.derivedClients.clear()
        current.baseClient.dispatcher.executorService.shutdown()
        current.baseClient.connectionPool.evictAll()
    }

    internal fun ensureClient(): OkHttpClient = ensureActiveState().baseClient

    open fun acquireClient(request: TurboHttpRequest): OkHttpClient {
        val currentState = ensureActiveState()
        val key = DerivedClientKey(
            connectTimeoutMs = request.connectTimeoutMs,
            receiveTimeoutMs = request.receiveTimeoutMs,
            sendTimeoutMs = request.sendTimeoutMs,
            followRedirects = request.followRedirects,
            allowBadCertificates = request.allowBadCertificates
        )
        return currentState.derivedClients.getOrPut(key) {
            buildDerivedClient(currentState.baseClient, request)
        }
    }

    private fun buildDerivedClient(
        sharedClient: OkHttpClient,
        request: TurboHttpRequest
    ): OkHttpClient {
        val clientBuilder = sharedClient.newBuilder()
            .followRedirects(request.followRedirects)
            .followSslRedirects(request.followRedirects)

        request.connectTimeoutMs?.let {
            clientBuilder.connectTimeout(it, TimeUnit.MILLISECONDS)
        }
        request.receiveTimeoutMs?.let {
            clientBuilder.readTimeout(it, TimeUnit.MILLISECONDS)
        }
        request.sendTimeoutMs?.let {
            clientBuilder.writeTimeout(it, TimeUnit.MILLISECONDS)
        }

        if (request.allowBadCertificates) {
            configureInsecureTls(clientBuilder)
        }

        return clientBuilder.build()
    }

    private fun configureInsecureTls(builder: OkHttpClient.Builder) {
        builder.sslSocketFactory(insecureSslSocketFactory, insecureTrustManager)
        builder.hostnameVerifier(insecureHostnameVerifier)
    }

    companion object {
        private const val DEFAULT_MAX_REQUESTS = 64
        private const val DEFAULT_MAX_REQUESTS_PER_HOST = 32
        private const val DEFAULT_MAX_IDLE_CONNECTIONS = 16
        private const val MAX_DERIVED_CLIENTS = 16

        private val insecureTrustManager: X509TrustManager by lazy {
            object : X509TrustManager {
                override fun checkClientTrusted(chain: Array<X509Certificate>, authType: String) {}
                override fun checkServerTrusted(chain: Array<X509Certificate>, authType: String) {}
                override fun getAcceptedIssuers(): Array<X509Certificate> = arrayOf()
            }
        }

        private val insecureSslSocketFactory: SSLSocketFactory by lazy {
            val sslContext = SSLContext.getInstance("TLS")
            sslContext.init(null, arrayOf<TrustManager>(insecureTrustManager), SecureRandom())
            sslContext.socketFactory
        }

        private val insecureHostnameVerifier: HostnameVerifier by lazy {
            HostnameVerifier { _, _ -> true }
        }

        private fun createDispatcher(): Dispatcher = Dispatcher().apply {
            maxRequests = DEFAULT_MAX_REQUESTS
            maxRequestsPerHost = DEFAULT_MAX_REQUESTS_PER_HOST
        }

        private fun createDefaultBaseClient(targetDispatcher: Dispatcher): OkHttpClient =
            OkHttpClient.Builder()
                .dispatcher(targetDispatcher)
                .connectionPool(ConnectionPool(DEFAULT_MAX_IDLE_CONNECTIONS, 5, TimeUnit.MINUTES))
                .protocols(listOf(Protocol.HTTP_2, Protocol.HTTP_1_1))
                .build()
    }
}
