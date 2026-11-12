package io.github.khirohis.flutter_turbo_fetch

import io.flutter.embedding.engine.plugins.FlutterPlugin
import io.flutter.plugin.common.BinaryMessenger
import java.nio.ByteBuffer
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

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
    fun executeRequest_andCancelRequest_delegateToExecutor_andInvokeCallbackExactlyOnce() {
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
        val callbackCount = AtomicInteger(0)

        val request = createRequest(requestId = "req-cancel-test")
        plugin.executeRequest(request) { result ->
            callbackCount.incrementAndGet()
            receivedResponse.set(result.getOrNull())
        }

        assertEquals("req-cancel-test", capturedRequest.get()?.requestId)
        assertNull(receivedResponse.get())
        assertEquals(0, callbackCount.get())

        // cancelRequest delegates to executor.cancel(requestId)
        plugin.cancelRequest("req-cancel-test")
        assertEquals("req-cancel-test", canceledRequestId.get())

        // When executor finishes and invokes onResult, callback receives the response exactly once
        val expectedResponse = TurboHttpResponse(errorCode = "cancel", errorMessage = "Canceled")
        capturedOnResult.get().invoke(expectedResponse)
        assertEquals("cancel", receivedResponse.get()?.errorCode)
        assertEquals(1, callbackCount.get(), "Plugin callback must be invoked exactly once")
    }

    @Test
    fun engineLifecycle_attachedAndDetached_delegatesToExecutorLifecycle() {
        var ensureReadyCalled = false
        var shutdownCalled = false

        val fakeExecutor = object : TurboHttpExecutor() {
            override fun ensureReady() {
                ensureReadyCalled = true
            }

            override fun shutdown() {
                shutdownCalled = true
            }
        }

        val fakeMessenger = object : BinaryMessenger {
            val handlers = mutableMapOf<String, BinaryMessenger.BinaryMessageHandler?>()

            override fun send(channel: String, message: ByteBuffer?) {}
            override fun send(channel: String, message: ByteBuffer?, callback: BinaryMessenger.BinaryReply?) {}
            override fun setMessageHandler(channel: String, handler: BinaryMessenger.BinaryMessageHandler?) {
                handlers[channel] = handler
            }
            override fun makeBackgroundTaskQueue(options: BinaryMessenger.TaskQueueOptions?): BinaryMessenger.TaskQueue? = null
        }

        val constructor = FlutterPlugin.FlutterPluginBinding::class.java.declaredConstructors[0]
        constructor.isAccessible = true
        val args = arrayOfNulls<Any>(constructor.parameterCount)
        val messengerIndex = constructor.parameterTypes.indexOfFirst { it == BinaryMessenger::class.java }
        if (messengerIndex != -1) {
            args[messengerIndex] = fakeMessenger
        }
        val binding = constructor.newInstance(*args) as FlutterPlugin.FlutterPluginBinding

        val plugin = FlutterTurboFetchPlugin(executor = fakeExecutor)

        // onAttachedToEngine ensures executor is ready and sets up Pigeon channel
        plugin.onAttachedToEngine(binding)
        assertTrue(ensureReadyCalled, "ensureReady must be invoked on engine attach")
        assertTrue(fakeMessenger.handlers.containsKey("dev.flutter.pigeon.flutter_turbo_fetch.TurboFetchHostApi.executeRequest"))
        assertNotNull(fakeMessenger.handlers["dev.flutter.pigeon.flutter_turbo_fetch.TurboFetchHostApi.executeRequest"])

        // onDetachedFromEngine shuts down executor and cleans up Pigeon channel
        plugin.onDetachedFromEngine(binding)
        assertTrue(shutdownCalled, "shutdown must be invoked on engine detach")
        assertNull(fakeMessenger.handlers["dev.flutter.pigeon.flutter_turbo_fetch.TurboFetchHostApi.executeRequest"])
    }
}
