package io.github.khirohis.flutter_turbo_fetch

import okhttp3.OkHttpClient
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertSame
import kotlin.test.assertTrue

internal class OkHttpClientManagerTest {
    private fun createRequest(
        connectTimeoutMs: Long? = 3000L,
        receiveTimeoutMs: Long? = 4000L,
        sendTimeoutMs: Long? = 5000L,
        followRedirects: Boolean = false,
        allowBadCertificates: Boolean = false
    ): TurboHttpRequest {
        return TurboHttpRequest(
            requestId = "client-test-req",
            url = "https://example.com/api",
            method = "GET",
            headers = emptyMap(),
            body = null,
            connectTimeoutMs = connectTimeoutMs,
            receiveTimeoutMs = receiveTimeoutMs,
            sendTimeoutMs = sendTimeoutMs,
            followRedirects = followRedirects,
            allowBadCertificates = allowBadCertificates,
            maxResponseBytes = null
        )
    }

    @Test
    fun ensureClient_reusesSharedClientWithFixedDispatcherLimits() {
        val manager = OkHttpClientManager()

        val firstClient = manager.ensureClient()
        assertEquals(64, firstClient.dispatcher.maxRequests)
        assertEquals(32, firstClient.dispatcher.maxRequestsPerHost)

        val secondClient = manager.ensureClient()
        assertSame(firstClient, secondClient)
        assertEquals(64, secondClient.dispatcher.maxRequests)
        assertEquals(32, secondClient.dispatcher.maxRequestsPerHost)
    }

    @Test
    fun ensureClient_andEnsureReady_recoverAfterShutdown() {
        val manager = OkHttpClientManager()
        val initialClient = manager.ensureClient()
        manager.shutdown()
        assertTrue(initialClient.dispatcher.executorService.isShutdown)

        manager.ensureReady()
        val recoveredClient = manager.ensureClient()
        assertFalse(recoveredClient.dispatcher.executorService.isShutdown)
        assertEquals(64, recoveredClient.dispatcher.maxRequests)
        assertEquals(32, recoveredClient.dispatcher.maxRequestsPerHost)
    }

    @Test
    fun acquireClient_appliesPerRequestTimeoutsRedirectsAndInsecureTls() {
        val manager = OkHttpClientManager()
        val request = createRequest(
            connectTimeoutMs = 1500L,
            receiveTimeoutMs = 2500L,
            sendTimeoutMs = 3500L,
            followRedirects = false,
            allowBadCertificates = true
        )

        val derivedClient = manager.acquireClient(request)
        assertEquals(1500, derivedClient.connectTimeoutMillis)
        assertEquals(2500, derivedClient.readTimeoutMillis)
        assertEquals(3500, derivedClient.writeTimeoutMillis)
        assertFalse(derivedClient.followRedirects)
        assertFalse(derivedClient.followSslRedirects)
        assertNotNull(derivedClient.x509TrustManager)

        // Verify insecure TLS instances are shared across requests to preserve ConnectionPool reuse
        val secondDerivedClient = manager.acquireClient(request)
        assertSame(derivedClient.sslSocketFactory, secondDerivedClient.sslSocketFactory)
        assertSame(derivedClient.x509TrustManager, secondDerivedClient.x509TrustManager)
        assertSame(derivedClient.hostnameVerifier, secondDerivedClient.hostnameVerifier)
    }

    @Test
    fun secondaryConstructor_withBaseClient_sharesManagedDispatcher() {
        val customBaseClient = OkHttpClient.Builder().build()
        val manager = OkHttpClientManager(customBaseClient)

        val client = manager.ensureClient()
        assertFalse(client.dispatcher.executorService.isShutdown)

        manager.shutdown()
        assertTrue(client.dispatcher.executorService.isShutdown)
    }

    @Test
    fun acquireClient_withNullTimeouts_preservesOkHttpDefaults() {
        val manager = OkHttpClientManager()
        val request = createRequest(
            connectTimeoutMs = null,
            receiveTimeoutMs = null,
            sendTimeoutMs = null
        )

        val client = manager.acquireClient(request)
        // OkHttp default timeouts are 10,000 ms (10 seconds)
        assertEquals(10_000, client.connectTimeoutMillis)
        assertEquals(10_000, client.readTimeoutMillis)
        assertEquals(10_000, client.writeTimeoutMillis)
    }
}
