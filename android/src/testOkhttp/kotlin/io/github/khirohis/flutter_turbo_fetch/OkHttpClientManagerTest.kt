package io.github.khirohis.flutter_turbo_fetch

import okhttp3.OkHttpClient
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertNotSame
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

    @Test
    fun acquireClient_reusesCachedDerivedClientForIdenticalConfig() {
        val manager = OkHttpClientManager()
        val requestA = createRequest(
            connectTimeoutMs = 2000L,
            receiveTimeoutMs = 3000L,
            sendTimeoutMs = 4000L,
            followRedirects = true,
            allowBadCertificates = false
        )
        val requestB = createRequest(
            connectTimeoutMs = 2000L,
            receiveTimeoutMs = 3000L,
            sendTimeoutMs = 4000L,
            followRedirects = true,
            allowBadCertificates = false
        )

        val first = manager.acquireClient(requestA)
        val second = manager.acquireClient(requestB)

        assertSame(first, second)
    }

    @Test
    fun acquireClient_distinguishesDerivedClientsByConfig() {
        val manager = OkHttpClientManager()
        val baseReq = createRequest(
            connectTimeoutMs = 2000L,
            receiveTimeoutMs = 3000L,
            sendTimeoutMs = 4000L,
            followRedirects = true,
            allowBadCertificates = false
        )
        val diffTimeoutReq = createRequest(
            connectTimeoutMs = 2500L,
            receiveTimeoutMs = 3000L,
            sendTimeoutMs = 4000L,
            followRedirects = true,
            allowBadCertificates = false
        )
        val diffRedirectReq = createRequest(
            connectTimeoutMs = 2000L,
            receiveTimeoutMs = 3000L,
            sendTimeoutMs = 4000L,
            followRedirects = false,
            allowBadCertificates = false
        )
        val diffTlsReq = createRequest(
            connectTimeoutMs = 2000L,
            receiveTimeoutMs = 3000L,
            sendTimeoutMs = 4000L,
            followRedirects = true,
            allowBadCertificates = true
        )

        val baseClient = manager.acquireClient(baseReq)
        val diffTimeoutClient = manager.acquireClient(diffTimeoutReq)
        val diffRedirectClient = manager.acquireClient(diffRedirectReq)
        val diffTlsClient = manager.acquireClient(diffTlsReq)

        assertNotSame(baseClient, diffTimeoutClient)
        assertNotSame(baseClient, diffRedirectClient)
        assertNotSame(baseClient, diffTlsClient)
        assertEquals(2500, diffTimeoutClient.connectTimeoutMillis)
        assertFalse(diffRedirectClient.followRedirects)
        assertFalse(diffRedirectClient.followSslRedirects)
    }

    @Test
    fun acquireClient_clearsDerivedClientCacheOnShutdownAndRecovery() {
        val manager = OkHttpClientManager()
        val request = createRequest(connectTimeoutMs = 1234L)

        val beforeShutdown = manager.acquireClient(request)
        assertSame(beforeShutdown, manager.acquireClient(request))

        manager.shutdown()
        assertEquals(0, manager.derivedClientCacheSize)
        assertTrue(beforeShutdown.dispatcher.executorService.isShutdown)

        val afterRecovery = manager.acquireClient(request)
        assertNotSame(beforeShutdown, afterRecovery)
        assertFalse(afterRecovery.dispatcher.executorService.isShutdown)
        assertEquals(1234, afterRecovery.connectTimeoutMillis)
    }

    @Test
    fun acquireClient_boundsDerivedClientCacheSize() {
        val manager = OkHttpClientManager()

        val firstClient = manager.acquireClient(createRequest(connectTimeoutMs = 100L))
        val secondClient = manager.acquireClient(createRequest(connectTimeoutMs = 200L))
        for (i in 3L..16L) {
            val client = manager.acquireClient(createRequest(connectTimeoutMs = i * 100L))
            assertEquals((i * 100L).toInt(), client.connectTimeoutMillis)
        }
        assertEquals(16, manager.derivedClientCacheSize)

        // Inserting a 17th distinct config evicts the oldest (1st) entry in FIFO order.
        manager.acquireClient(createRequest(connectTimeoutMs = 1700L))
        assertEquals(16, manager.derivedClientCacheSize)
        assertSame(secondClient, manager.acquireClient(createRequest(connectTimeoutMs = 200L)))
        assertNotSame(firstClient, manager.acquireClient(createRequest(connectTimeoutMs = 100L)))
    }
}
