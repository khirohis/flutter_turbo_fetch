package io.github.khirohis.flutter_turbo_fetch

import okhttp3.Call
import okhttp3.OkHttpClient
import okhttp3.Request
import okio.Timeout
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

internal class FlutterTurboFetchPluginTest {
    private class FakeCall : Call {
        private val canceled = AtomicBoolean(false)
        private val executed = AtomicBoolean(false)

        override fun cancel() {
            canceled.set(true)
        }

        override fun isCanceled(): Boolean = canceled.get()

        override fun isExecuted(): Boolean = executed.get()

        override fun request(): Request = Request.Builder().url("https://example.com").build()

        override fun timeout(): Timeout = Timeout.NONE

        override fun clone(): Call = FakeCall()

        override fun execute(): okhttp3.Response {
            throw UnsupportedOperationException("Not used in unit test")
        }

        override fun enqueue(responseCallback: okhttp3.Callback) {
            executed.set(true)
        }
    }

    private fun createRequest(
        requestId: String = "req-1",
        maxRequestsPerHost: Long = 32L
    ): TurboHttpRequest {
        return TurboHttpRequest(
            requestId = requestId,
            url = "https://example.com/data",
            method = "GET",
            headers = emptyMap(),
            body = null,
            connectTimeoutMs = 5000L,
            receiveTimeoutMs = 5000L,
            sendTimeoutMs = 5000L,
            followRedirects = true,
            maxRequestsPerHost = maxRequestsPerHost,
            allowBadCertificates = false,
            maxResponseBytes = null
        )
    }

    @Test
    fun pluginInstance_canBeCreated() {
        val plugin = FlutterTurboFetchPlugin()
        assertNotNull(plugin)
    }

    @Test
    fun ensureClient_scalesDispatcherLimitsDynamically() {
        val plugin = FlutterTurboFetchPlugin()

        // Default scale (requestedPerHost = 32 -> maxRequests = 64, maxRequestsPerHost = 32)
        val defaultClient = plugin.ensureClient(32)
        assertEquals(64, defaultClient.dispatcher.maxRequests)
        assertEquals(32, defaultClient.dispatcher.maxRequestsPerHost)

        // Scale up above DEFAULT_MAX_REQUESTS (requestedPerHost = 100 -> maxRequests = 100, maxRequestsPerHost = 100)
        val scaledUpClient = plugin.ensureClient(100)
        assertEquals(100, scaledUpClient.dispatcher.maxRequests)
        assertEquals(100, scaledUpClient.dispatcher.maxRequestsPerHost)

        // Scale back down when requestedPerHost <= 64 (requestedPerHost = 16 -> maxRequests = 64, maxRequestsPerHost = 16)
        val scaledDownClient = plugin.ensureClient(16)
        assertEquals(64, scaledDownClient.dispatcher.maxRequests)
        assertEquals(16, scaledDownClient.dispatcher.maxRequestsPerHost)
    }

    @Test
    fun ensureClient_recoversAfterDispatcherShutdown() {
        val plugin = FlutterTurboFetchPlugin()
        val initialClient = plugin.ensureClient(32)
        initialClient.dispatcher.executorService.shutdown()
        assertTrue(initialClient.dispatcher.executorService.isShutdown)

        val recoveredClient = plugin.ensureClient(48)
        assertFalse(recoveredClient.dispatcher.executorService.isShutdown)
        assertEquals(48, recoveredClient.dispatcher.maxRequestsPerHost)
    }

    @Test
    fun executeRequest_registersActiveCallAndSupportsCancelBeforeCompletion() {
        val fakeCall = FakeCall()
        val capturedOnResult = AtomicReference<(TurboHttpResponse) -> Unit>()

        val fakeExecutor = object : TurboHttpExecutor() {
            override fun execute(
                client: OkHttpClient,
                request: TurboHttpRequest,
                onCallCreated: (Call) -> Unit,
                onResult: (TurboHttpResponse) -> Unit
            ) {
                onCallCreated(fakeCall)
                capturedOnResult.set(onResult)
            }
        }

        val plugin = FlutterTurboFetchPlugin(handler = null, executor = fakeExecutor)
        val receivedResponse = AtomicReference<TurboHttpResponse?>()

        plugin.executeRequest(createRequest(requestId = "req-cancel-test")) { result ->
            receivedResponse.set(result.getOrNull())
        }

        // Before completion, cancelRequest should cancel the registered Call
        assertFalse(fakeCall.isCanceled())
        plugin.cancelRequest("req-cancel-test")
        assertTrue(fakeCall.isCanceled())
        assertNull(receivedResponse.get())

        // When executor finishes and invokes onResult, callback receives the response
        val expectedResponse = TurboHttpResponse(errorCode = "cancel", errorMessage = "Canceled")
        capturedOnResult.get().invoke(expectedResponse)
        assertEquals("cancel", receivedResponse.get()?.errorCode)
    }
}
