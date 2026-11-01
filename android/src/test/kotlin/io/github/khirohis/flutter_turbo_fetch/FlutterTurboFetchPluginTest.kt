package io.github.khirohis.flutter_turbo_fetch

import java.util.concurrent.atomic.AtomicReference
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull

internal class FlutterTurboFetchPluginTest {
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
    fun executeRequest_andCancelRequest_delegateToExecutor() {
        val capturedRequest = AtomicReference<TurboHttpRequest?>()
        val capturedOnResult = AtomicReference<(TurboHttpResponse) -> Unit>()
        val canceledRequestId = AtomicReference<String?>()

        val fakeExecutor = object : TurboHttpExecutor() {
            override fun execute(
                request: TurboHttpRequest,
                onResult: (TurboHttpResponse) -> Unit
            ) {
                capturedRequest.set(request)
                capturedOnResult.set(onResult)
            }

            override fun cancel(requestId: String) {
                canceledRequestId.set(requestId)
            }
        }

        val plugin = FlutterTurboFetchPlugin(executor = fakeExecutor)
        val receivedResponse = AtomicReference<TurboHttpResponse?>()

        val request = createRequest(requestId = "req-cancel-test")
        plugin.executeRequest(request) { result ->
            receivedResponse.set(result.getOrNull())
        }

        assertEquals("req-cancel-test", capturedRequest.get()?.requestId)
        assertNull(receivedResponse.get())

        // cancelRequest delegates to executor.cancel(requestId)
        plugin.cancelRequest("req-cancel-test")
        assertEquals("req-cancel-test", canceledRequestId.get())

        // When executor finishes and invokes onResult, callback receives the response
        val expectedResponse = TurboHttpResponse(errorCode = "cancel", errorMessage = "Canceled")
        capturedOnResult.get().invoke(expectedResponse)
        assertEquals("cancel", receivedResponse.get()?.errorCode)
    }
}
