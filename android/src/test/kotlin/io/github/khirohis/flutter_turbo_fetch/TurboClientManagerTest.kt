package io.github.khirohis.flutter_turbo_fetch

import okhttp3.OkHttpClient
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertSame
import kotlin.test.assertTrue

internal class TurboClientManagerTest {
    private fun createRequest(
        connectTimeoutMs: Long? = 3000L,
        receiveTimeoutMs: Long? = 4000L,
        sendTimeoutMs: Long? = 5000L,
        followRedirects: Boolean = false,
        maxRequestsPerHost: Long = 32L,
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
            maxRequestsPerHost = maxRequestsPerHost,
            allowBadCertificates = allowBadCertificates,
            maxResponseBytes = null
        )
    }

    @Test
    fun ensureClient_scalesDispatcherLimitsDynamically() {
        val manager = TurboClientManager()

        // Default scale (requestedPerHost = 32 -> maxRequests = 64, maxRequestsPerHost = 32)
        val defaultClient = manager.ensureClient(32)
        assertEquals(64, defaultClient.dispatcher.maxRequests)
        assertEquals(32, defaultClient.dispatcher.maxRequestsPerHost)

        // Scale up above DEFAULT_MAX_REQUESTS (requestedPerHost = 100 -> maxRequests = 100, maxRequestsPerHost = 100)
        val scaledUpClient = manager.ensureClient(100)
        assertEquals(100, scaledUpClient.dispatcher.maxRequests)
        assertEquals(100, scaledUpClient.dispatcher.maxRequestsPerHost)

        // Scale back down when requestedPerHost <= 64 (requestedPerHost = 16 -> maxRequests = 64, maxRequestsPerHost = 16)
        val scaledDownClient = manager.ensureClient(16)
        assertEquals(64, scaledDownClient.dispatcher.maxRequests)
        assertEquals(16, scaledDownClient.dispatcher.maxRequestsPerHost)
    }

    @Test
    fun ensureClient_andEnsureReady_recoverAfterShutdown() {
        val manager = TurboClientManager()
        val initialClient = manager.ensureClient(32)
        manager.shutdown()
        assertTrue(initialClient.dispatcher.executorService.isShutdown)

        manager.ensureReady()
        val recoveredClient = manager.ensureClient(48)
        assertFalse(recoveredClient.dispatcher.executorService.isShutdown)
        assertEquals(48, recoveredClient.dispatcher.maxRequestsPerHost)
    }

    @Test
    fun acquireClient_appliesPerRequestTimeoutsRedirectsAndInsecureTls() {
        val manager = TurboClientManager()
        val request = createRequest(
            connectTimeoutMs = 1500L,
            receiveTimeoutMs = 2500L,
            sendTimeoutMs = 3500L,
            followRedirects = false,
            maxRequestsPerHost = 40L,
            allowBadCertificates = true
        )

        val derivedClient = manager.acquireClient(request)
        assertEquals(1500, derivedClient.connectTimeoutMillis)
        assertEquals(2500, derivedClient.readTimeoutMillis)
        assertEquals(3500, derivedClient.writeTimeoutMillis)
        assertFalse(derivedClient.followRedirects)
        assertFalse(derivedClient.followSslRedirects)
        assertEquals(40, derivedClient.dispatcher.maxRequestsPerHost)
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
        val manager = TurboClientManager(customBaseClient)

        val client = manager.ensureClient(48)
        assertEquals(48, client.dispatcher.maxRequestsPerHost)

        manager.shutdown()
        assertTrue(client.dispatcher.executorService.isShutdown)
    }
}

