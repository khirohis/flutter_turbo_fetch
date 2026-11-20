package io.github.khirohis.flutter_turbo_fetch

import android.content.Context
import org.chromium.net.NetworkException
import org.mockito.Mockito
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertSame
import kotlin.test.assertTrue

internal class CronetTurboHttpExecutorTest {
    private data class Harness(
        val engine: FakeCronetEngine,
        val clientManager: CronetClientManager,
        val timerScheduler: ManualTimerScheduler,
        val executor: CronetTurboHttpExecutor
    )

    private fun createHarness(
        engineFactory: (Context) -> FakeCronetEngine = { FakeCronetEngine() }
    ): Harness {
        val engine = engineFactory(Mockito.mock(Context::class.java))
        val clientManager = CronetClientManager(
            engineFactory = { engine },
            shutdownRunner = { task -> task() }
        )
        val timerScheduler = ManualTimerScheduler()
        val executor = CronetTurboHttpExecutor(
            clientManager = clientManager,
            callbackExecutor = DirectExecutor(),
            timerScheduler = timerScheduler
        )
        executor.ensureReady(Mockito.mock(Context::class.java))
        return Harness(engine, clientManager, timerScheduler, executor)
    }

    private fun runAndAssertOnce(
        executor: CronetTurboHttpExecutor,
        request: TurboHttpRequest,
        driver: () -> Unit = {}
    ): TurboHttpResponse {
        val callCount = AtomicInteger(0)
        val responseRef = AtomicReference<TurboHttpResponse?>()

        executor.execute(request) { response ->
            callCount.incrementAndGet()
            responseRef.set(response)
        }
        driver()

        assertEquals(1, callCount.get(), "Callback must be invoked exactly once")
        assertEquals(0, executor.activeCallCount, "activeCalls must be empty after completion")
        assertFalse(executor.hasActiveCall(request.requestId))
        return assertNotNull(responseRef.get())
    }

    @Test
    fun execute_getMultiChunkResponse_concatenatesUsingReusable64KiBDirectBufferAndNormalizesHeaders() {
        val harness = createHarness()
        val chunk1 = "hello-".encodeToByteArray()
        val chunk2 = "cronet-world".encodeToByteArray()
        val expected = chunk1 + chunk2

        val responseInfo = FakeUrlResponseInfo(
            statusCode = 201,
            statusText = "Created",
            headersList = listOf(
                "X-Custom" to "v1",
                "x-custom" to "v2",
                "Content-Type" to "text/plain"
            )
        )

        var readStep = 0
        harness.engine.onBuildRequest = { req ->
            req.onStart = {
                req.callback.onResponseStarted(req, responseInfo)
            }
            req.onRead = { _, buffer ->
                when (readStep++) {
                    0 -> {
                        buffer.put(chunk1)
                        req.callback.onReadCompleted(req, responseInfo, buffer)
                    }
                    1 -> {
                        buffer.put(chunk2)
                        req.callback.onReadCompleted(req, responseInfo, buffer)
                    }
                    else -> {
                        req.done = true
                        req.callback.onSucceeded(req, responseInfo)
                    }
                }
            }
        }

        val response = runAndAssertOnce(harness.executor, createCronetRequest())

        val sentReq = harness.engine.createdRequests.single()
        assertEquals(3, sentReq.readBuffers.size)
        val buffer = sentReq.readBuffers.first()
        assertTrue(buffer.isDirect, "Cronet read buffer must be a direct ByteBuffer")
        assertEquals(64 * 1024, buffer.capacity(), "Cronet read buffer must be fixed 64KiB")
        assertSame(buffer, sentReq.readBuffers[1], "Direct ByteBuffer must be reused across reads")
        assertSame(buffer, sentReq.readBuffers[2], "Direct ByteBuffer must be reused across reads")

        assertNull(response.errorCode)
        assertEquals(201L, response.statusCode)
        assertEquals("Created", response.statusMessage)
        assertContentEquals(expected, response.body)
        assertEquals(listOf("v1", "v2"), response.headers?.get("x-custom"))
        assertEquals(0, harness.timerScheduler.pendingCount)
    }

    @Test
    fun execute_emptyResponseBody_returnsEmptyByteArraySafely() {
        val harness = createHarness()
        val responseInfo = FakeUrlResponseInfo(statusCode = 200, statusText = "OK")

        harness.engine.onBuildRequest = { req ->
            req.onStart = { req.callback.onResponseStarted(req, responseInfo) }
            req.onRead = { _, _ ->
                req.done = true
                req.callback.onSucceeded(req, responseInfo)
            }
        }

        val response = runAndAssertOnce(harness.executor, createCronetRequest())
        assertNull(response.errorCode)
        assertEquals(200L, response.statusCode)
        assertNotNull(response.body)
        assertEquals(0, response.body.size)
    }

    @Test
    fun execute_postWithBodyAndContentType_uploadsPayloadCorrectly() {
        val harness = createHarness()
        val requestPayload = "turbo-cronet-post-payload".encodeToByteArray()
        val responseInfo = FakeUrlResponseInfo(statusCode = 200, statusText = "OK")
        val uploadedRef = AtomicReference<ByteArray>()

        harness.engine.onBuildRequest = { req ->
            req.onStart = {
                val provider = assertNotNull(req.uploadDataProvider)
                uploadedRef.set(FakeUploadDataSink(provider, chunkBufferSize = 8).readAll())
                req.callback.onResponseStarted(req, responseInfo)
            }
            req.onRead = { _, _ ->
                req.done = true
                req.callback.onSucceeded(req, responseInfo)
            }
        }

        val response = runAndAssertOnce(
            harness.executor,
            createCronetRequest(
                method = "POST",
                headers = mapOf("content-type" to "application/json"),
                body = requestPayload
            )
        )

        assertNull(response.errorCode)
        assertEquals(200L, response.statusCode)
        assertContentEquals(requestPayload, uploadedRef.get())
    }

    @Test
    fun execute_postWithNonEmptyBodyWithoutContentType_returnsConnectionErrorWithoutStartingRequest() {
        val harness = createHarness()

        val response = runAndAssertOnce(
            harness.executor,
            createCronetRequest(
                method = "POST",
                headers = emptyMap(),
                body = "raw-body".encodeToByteArray()
            )
        )

        assertEquals("connectionError", response.errorCode)
        assertTrue(response.errorMessage?.contains("Content-Type", ignoreCase = true) == true)
        assertEquals(0, harness.engine.createdRequests.size, "Must fail before building/starting UrlRequest")
    }

    @Test
    fun execute_postWithNullOrEmptyBodyWithoutContentType_succeedsWithoutUploadProvider() {
        val harness = createHarness()
        val responseInfo = FakeUrlResponseInfo(statusCode = 204, statusText = "No Content")

        harness.engine.onBuildRequest = { req ->
            req.onStart = { req.callback.onResponseStarted(req, responseInfo) }
            req.onRead = { _, _ ->
                req.done = true
                req.callback.onSucceeded(req, responseInfo)
            }
        }

        val nullBodyResponse = runAndAssertOnce(
            harness.executor,
            createCronetRequest(requestId = "post-null", method = "POST", body = null)
        )
        val emptyBodyResponse = runAndAssertOnce(
            harness.executor,
            createCronetRequest(requestId = "post-empty", method = "POST", body = ByteArray(0))
        )

        assertNull(nullBodyResponse.errorCode)
        assertEquals(204L, nullBodyResponse.statusCode)
        assertNull(emptyBodyResponse.errorCode)
        assertEquals(204L, emptyBodyResponse.statusCode)
        assertTrue(harness.engine.createdRequests.all { it.uploadDataProvider == null })
    }

    @Test
    fun execute_allowBadCertificatesTrue_returnsUnknownErrorImmediatelyWithoutStartingRequest() {
        val harness = createHarness()

        val response = runAndAssertOnce(
            harness.executor,
            createCronetRequest(allowBadCertificates = true)
        )

        assertEquals("unknown", response.errorCode)
        assertTrue(
            response.errorMessage?.contains("Network Security Config", ignoreCase = true) == true,
            "Error message should mention Network Security Config guidance"
        )
        assertEquals(0, harness.engine.createdRequests.size)
    }

    @Test
    fun execute_whenEngineInitializationFails_returnsUnknownErrorSafely() {
        val failingManager = CronetClientManager(
            engineFactory = { throw IllegalStateException("No Cronet provider enabled") },
            shutdownRunner = { task -> task() }
        )
        val timerScheduler = ManualTimerScheduler()
        val executor = CronetTurboHttpExecutor(
            clientManager = failingManager,
            callbackExecutor = DirectExecutor(),
            timerScheduler = timerScheduler
        )
        executor.ensureReady(Mockito.mock(Context::class.java))

        val response = runAndAssertOnce(executor, createCronetRequest())

        assertEquals("unknown", response.errorCode)
        assertNotNull(response.errorMessage)
        assertEquals(0, timerScheduler.pendingCount)
    }

    @Test
    fun execute_followRedirectsTrue_followsRedirectAndReturnsFinalResponse() {
        val harness = createHarness()
        val redirectInfo = FakeUrlResponseInfo(
            statusCode = 302,
            statusText = "Found",
            headersList = listOf("Location" to "https://example.com/final")
        )
        val finalInfo = FakeUrlResponseInfo(statusCode = 200, statusText = "OK")
        val finalBody = "redirected-ok".encodeToByteArray()

        var readDone = false
        harness.engine.onBuildRequest = { req ->
            req.onStart = {
                req.callback.onRedirectReceived(req, redirectInfo, "https://example.com/final")
            }
            req.onFollowRedirect = {
                req.callback.onResponseStarted(req, finalInfo)
            }
            req.onRead = { _, buffer ->
                if (!readDone) {
                    readDone = true
                    buffer.put(finalBody)
                    req.callback.onReadCompleted(req, finalInfo, buffer)
                } else {
                    req.done = true
                    req.callback.onSucceeded(req, finalInfo)
                }
            }
        }

        val response = runAndAssertOnce(
            harness.executor,
            createCronetRequest(followRedirects = true)
        )

        val sentReq = harness.engine.createdRequests.single()
        assertEquals(1, sentReq.followRedirectCount)
        assertEquals(0, sentReq.cancelCount)
        assertNull(response.errorCode)
        assertEquals(200L, response.statusCode)
        assertContentEquals(finalBody, response.body)
    }

    @Test
    fun execute_followRedirectsFalse_returns3xxStatusAndHeadersWithEmptyBodyAndCancelsRequest() {
        val harness = createHarness()
        val redirectInfo = FakeUrlResponseInfo(
            statusCode = 302,
            statusText = "Found",
            headersList = listOf("Location" to "https://example.com/next")
        )

        harness.engine.onBuildRequest = { req ->
            req.onStart = {
                req.callback.onRedirectReceived(req, redirectInfo, "https://example.com/next")
            }
            req.onCancel = {
                // Simulate real Cronet firing onCanceled after cancel(); executor must not double-callback
                req.callback.onCanceled(req, redirectInfo)
            }
        }

        val response = runAndAssertOnce(
            harness.executor,
            createCronetRequest(followRedirects = false)
        )

        val sentReq = harness.engine.createdRequests.single()
        assertEquals(0, sentReq.followRedirectCount)
        assertEquals(1, sentReq.cancelCount)
        assertNull(response.errorCode)
        assertEquals(302L, response.statusCode)
        assertEquals("Found", response.statusMessage)
        assertEquals(listOf("https://example.com/next"), response.headers?.get("location"))
        assertNotNull(response.body)
        assertEquals(0, response.body.size)
        assertEquals(0, harness.timerScheduler.pendingCount)
    }

    @Test
    fun execute_maxResponseBytesExceededByContentLength_cancelsBeforeReadingAndReturnsError() {
        val harness = createHarness()
        val responseInfo = FakeUrlResponseInfo(
            statusCode = 200,
            statusText = "OK",
            headersList = listOf("Content-Length" to "100")
        )

        harness.engine.onBuildRequest = { req ->
            req.onStart = { req.callback.onResponseStarted(req, responseInfo) }
            req.onCancel = { req.callback.onCanceled(req, responseInfo) }
        }

        val response = runAndAssertOnce(
            harness.executor,
            createCronetRequest(maxResponseBytes = 50L)
        )

        val sentReq = harness.engine.createdRequests.single()
        assertEquals(0, sentReq.readBuffers.size, "Must cancel before invoking read() when Content-Length exceeds limit")
        assertEquals(1, sentReq.cancelCount)
        assertEquals("maxResponseBytesExceeded", response.errorCode)
        assertNull(response.body)
    }

    @Test
    fun execute_maxResponseBytesExceededDuringChunkRead_cancelsImmediatelyWithoutTruncation() {
        val harness = createHarness()
        val responseInfo = FakeUrlResponseInfo(statusCode = 200, statusText = "OK")
        val chunk1 = ByteArray(20) { 1 }
        val chunk2 = ByteArray(20) { 2 }

        var step = 0
        harness.engine.onBuildRequest = { req ->
            req.onStart = { req.callback.onResponseStarted(req, responseInfo) }
            req.onRead = { _, buffer ->
                val chunk = if (step++ == 0) chunk1 else chunk2
                buffer.put(chunk)
                req.callback.onReadCompleted(req, responseInfo, buffer)
            }
            req.onCancel = { req.callback.onCanceled(req, responseInfo) }
        }

        val response = runAndAssertOnce(
            harness.executor,
            createCronetRequest(maxResponseBytes = 32L)
        )

        val sentReq = harness.engine.createdRequests.single()
        assertEquals(1, sentReq.cancelCount)
        assertEquals("maxResponseBytesExceeded", response.errorCode)
        assertNull(response.body)
    }

    @Test
    fun execute_maxResponseBytesExactBoundary_succeeds() {
        val harness = createHarness()
        val payload = ByteArray(32) { 3 }
        val responseInfo = FakeUrlResponseInfo(
            statusCode = 200,
            statusText = "OK",
            headersList = listOf("Content-Length" to "32")
        )

        var readDone = false
        harness.engine.onBuildRequest = { req ->
            req.onStart = { req.callback.onResponseStarted(req, responseInfo) }
            req.onRead = { _, buffer ->
                if (!readDone) {
                    readDone = true
                    buffer.put(payload)
                    req.callback.onReadCompleted(req, responseInfo, buffer)
                } else {
                    req.done = true
                    req.callback.onSucceeded(req, responseInfo)
                }
            }
        }

        val response = runAndAssertOnce(
            harness.executor,
            createCronetRequest(maxResponseBytes = 32L)
        )

        assertNull(response.errorCode)
        assertContentEquals(payload, response.body)
    }

    @Test
    fun execute_maxResponseBytesNull_returnsFullBody() {
        val harness = createHarness()
        val payload = "unbounded-payload".encodeToByteArray()
        val responseInfo = FakeUrlResponseInfo(statusCode = 200, statusText = "OK")

        var readDone = false
        harness.engine.onBuildRequest = { req ->
            req.onStart = { req.callback.onResponseStarted(req, responseInfo) }
            req.onRead = { _, buffer ->
                if (!readDone) {
                    readDone = true
                    buffer.put(payload)
                    req.callback.onReadCompleted(req, responseInfo, buffer)
                } else {
                    req.done = true
                    req.callback.onSucceeded(req, responseInfo)
                }
            }
        }

        val response = runAndAssertOnce(
            harness.executor,
            createCronetRequest(maxResponseBytes = null)
        )

        assertNull(response.errorCode)
        assertContentEquals(payload, response.body)
    }

    @Test
    fun execute_invalidUrlOrInvalidHeaderCharacters_returnsConnectionErrorWithoutActiveRegistration() {
        val harness = createHarness()

        val badUrlResponse = runAndAssertOnce(
            harness.executor,
            createCronetRequest(requestId = "bad-url", url = "ftp://example.com/file")
        )
        assertEquals("connectionError", badUrlResponse.errorCode)
        assertEquals(0, harness.engine.createdRequests.size)

        val malformedUrlResponse = runAndAssertOnce(
            harness.executor,
            createCronetRequest(requestId = "malformed-url", url = "http://[::1")
        )
        assertEquals("connectionError", malformedUrlResponse.errorCode)
        assertEquals(0, harness.engine.createdRequests.size)

        val badHeaderResponse = runAndAssertOnce(
            harness.executor,
            createCronetRequest(
                requestId = "bad-header",
                headers = mapOf("X-Bad\r\nName" to "value\r\ninjection")
            )
        )
        assertEquals("connectionError", badHeaderResponse.errorCode)
        assertEquals(0, harness.engine.createdRequests.size)
    }

    @Test
    fun cancel_inFlightRequest_cancelsUrlRequestAndInvokesCallbackOnceWithCancel() {
        val harness = createHarness()

        harness.engine.onBuildRequest = { req ->
            req.onCancel = {
                req.callback.onCanceled(req, null)
            }
        }

        val response = runAndAssertOnce(
            harness.executor,
            createCronetRequest(requestId = "cancel-target")
        ) {
            assertTrue(harness.executor.hasActiveCall("cancel-target"))
            harness.executor.cancel("cancel-target")
        }

        assertEquals("cancel", response.errorCode)
        assertEquals(1, harness.engine.createdRequests.single().cancelCount)
        assertEquals(0, harness.timerScheduler.pendingCount)
    }

    @Test
    fun shutdown_cancelsAllInFlightRequestsAndDefersEngineShutdown() {
        val harness = createHarness()

        harness.engine.onBuildRequest = { req ->
            req.onCancel = {
                req.callback.onCanceled(req, null)
            }
        }

        val results = mutableListOf<TurboHttpResponse>()
        harness.executor.execute(createCronetRequest(requestId = "req-1")) { results.add(it) }
        harness.executor.execute(createCronetRequest(requestId = "req-2")) { results.add(it) }

        assertEquals(2, harness.executor.activeCallCount)
        assertEquals(0, harness.engine.shutdownCount)

        harness.executor.shutdown()

        assertEquals(2, results.size)
        assertTrue(results.all { it.errorCode == "cancel" })
        assertEquals(0, harness.executor.activeCallCount)
        assertEquals(1, harness.engine.shutdownCount)
    }

    @Test
    fun execute_phaseTimers_fireConnectSendAndReceiveTimeoutsDeterministically() {
        // 1. Connect timeout
        run {
            val harness = createHarness()
            harness.engine.onBuildRequest = { req ->
                req.onCancel = { req.callback.onCanceled(req, null) }
            }
            val response = runAndAssertOnce(
                harness.executor,
                createCronetRequest(
                    requestId = "conn-to",
                    connectTimeoutMs = 1500L,
                    sendTimeoutMs = null,
                    receiveTimeoutMs = null
                )
            ) {
                assertEquals(1, harness.timerScheduler.pendingCount)
                assertTrue(harness.timerScheduler.fireNextActive())
            }
            assertEquals("connectionTimeout", response.errorCode)
            assertEquals(1, harness.engine.createdRequests.single().cancelCount)
        }

        // 2. Send timeout (simulated during body upload phase)
        run {
            val harness = createHarness()
            harness.engine.onBuildRequest = { req ->
                req.onStart = {
                    val provider = assertNotNull(req.uploadDataProvider)
                    // Read a partial chunk to transition into SENDING_BODY without completing upload
                    FakeUploadDataSink(provider, chunkBufferSize = 4).readOneChunk()
                }
                req.onCancel = { req.callback.onCanceled(req, null) }
            }
            val response = runAndAssertOnce(
                harness.executor,
                createCronetRequest(
                    requestId = "send-to",
                    method = "POST",
                    headers = mapOf("Content-Type" to "text/plain"),
                    body = "longer-body-payload".encodeToByteArray(),
                    connectTimeoutMs = 1000L,
                    sendTimeoutMs = 2000L,
                    receiveTimeoutMs = null
                )
            ) {
                // Connect timer should have been canceled when upload started; active timer is now send (2000ms)
                val active = harness.timerScheduler.activeHandles.single()
                assertEquals(2000L, active.delayMs)
                active.fire()
            }
            assertEquals("sendTimeout", response.errorCode)
        }

        // 3. Receive timeout (after response headers arrive)
        run {
            val harness = createHarness()
            val info = FakeUrlResponseInfo(statusCode = 200, statusText = "OK")
            harness.engine.onBuildRequest = { req ->
                req.onStart = { req.callback.onResponseStarted(req, info) }
                req.onCancel = { req.callback.onCanceled(req, info) }
            }
            val response = runAndAssertOnce(
                harness.executor,
                createCronetRequest(
                    requestId = "recv-to",
                    connectTimeoutMs = 1000L,
                    sendTimeoutMs = null,
                    receiveTimeoutMs = 3000L
                )
            ) {
                val active = harness.timerScheduler.activeHandles.single()
                assertEquals(3000L, active.delayMs)
                active.fire()
            }
            assertEquals("receiveTimeout", response.errorCode)
        }
    }

    @Test
    fun execute_receiveTimer_resetsOnEachReadCompletedChunk() {
        val harness = createHarness()
        val info = FakeUrlResponseInfo(statusCode = 200, statusText = "OK")
        val chunk1 = "part1-".encodeToByteArray()
        val chunk2 = "part2".encodeToByteArray()

        val activeCountsDuringReads = mutableListOf<Int>()
        var step = 0

        harness.engine.onBuildRequest = { req ->
            req.onStart = {
                req.callback.onResponseStarted(req, info)
            }
            req.onRead = { _, buffer ->
                activeCountsDuringReads.add(harness.timerScheduler.pendingCount)
                when (step++) {
                    0 -> {
                        val firstReceiveHandle = harness.timerScheduler.activeHandles.single()
                        buffer.put(chunk1)
                        req.callback.onReadCompleted(req, info, buffer)
                        assertTrue(firstReceiveHandle.canceled, "Previous receive timer must be canceled on onReadCompleted")
                    }
                    1 -> {
                        val secondReceiveHandle = harness.timerScheduler.activeHandles.single()
                        buffer.put(chunk2)
                        req.callback.onReadCompleted(req, info, buffer)
                        assertTrue(secondReceiveHandle.canceled, "Receive timer must reset on subsequent onReadCompleted")
                    }
                    else -> {
                        req.done = true
                        req.callback.onSucceeded(req, info)
                    }
                }
            }
        }

        val response = runAndAssertOnce(
            harness.executor,
            createCronetRequest(
                connectTimeoutMs = null,
                sendTimeoutMs = null,
                receiveTimeoutMs = 2500L
            )
        )

        assertNull(response.errorCode)
        assertContentEquals(chunk1 + chunk2, response.body)
        assertEquals(listOf(1, 1, 1), activeCountsDuringReads)
        assertEquals(3, harness.timerScheduler.handles.size)
        assertTrue(harness.timerScheduler.handles.all { it.canceled && !it.fired && it.delayMs == 2500L })
        assertEquals(0, harness.timerScheduler.pendingCount)
    }

    @Test
    fun execute_nonNetworkCronetException_returnsUnknownError() {
        val harness = createHarness()
        harness.engine.onBuildRequest = { req ->
            req.onStart = {
                req.callback.onFailed(req, null, FakeCronetException("Internal Cronet failure"))
            }
        }

        val response = runAndAssertOnce(harness.executor, createCronetRequest())
        assertEquals("unknown", response.errorCode)
        assertEquals("Internal Cronet failure", response.errorMessage)
    }

    @Test
    fun classifyNetworkException_mapsTimeoutsCertificatesConnectionAndUnknownErrorsCorrectly() {
        val harness = createHarness()
        val executor = harness.executor

        // Connect timeout code
        assertEquals(
            "connectionTimeout",
            executor.classifyNetworkException(
                FakeNetworkException("Connect timed out", NetworkException.ERROR_CONNECTION_TIMED_OUT),
                RequestPhase.CONNECTING
            )
        )

        // General timed out code maps by active phase
        assertEquals(
            "connectionTimeout",
            executor.classifyNetworkException(
                FakeNetworkException("Timed out", NetworkException.ERROR_TIMED_OUT),
                RequestPhase.CONNECTING
            )
        )
        assertEquals(
            "sendTimeout",
            executor.classifyNetworkException(
                FakeNetworkException("Timed out", NetworkException.ERROR_TIMED_OUT),
                RequestPhase.SENDING_BODY
            )
        )
        assertEquals(
            "receiveTimeout",
            executor.classifyNetworkException(
                FakeNetworkException("Timed out", NetworkException.ERROR_TIMED_OUT),
                RequestPhase.RECEIVING
            )
        )

        // Certificate errors (-200..-299) and SSL handshake internal error codes
        for (internalCode in listOf(-200, -202, -299, -107, -113, -150)) {
            assertEquals(
                "badCertificate",
                executor.classifyNetworkException(
                    FakeNetworkException("Cert/TLS error", NetworkException.ERROR_OTHER, internalCode),
                    RequestPhase.CONNECTING
                )
            )
        }

        // Standard network / connection errors
        for (code in listOf(
            NetworkException.ERROR_HOSTNAME_NOT_RESOLVED,
            NetworkException.ERROR_INTERNET_DISCONNECTED,
            NetworkException.ERROR_NETWORK_CHANGED,
            NetworkException.ERROR_CONNECTION_CLOSED,
            NetworkException.ERROR_CONNECTION_REFUSED,
            NetworkException.ERROR_CONNECTION_RESET,
            NetworkException.ERROR_ADDRESS_UNREACHABLE,
            NetworkException.ERROR_QUIC_PROTOCOL_FAILED
        )) {
            assertEquals(
                "connectionError",
                executor.classifyNetworkException(
                    FakeNetworkException("Network error", code),
                    RequestPhase.CONNECTING
                )
            )
        }

        // Non-certificate ERROR_OTHER falls back to connectionError
        assertEquals(
            "connectionError",
            executor.classifyNetworkException(
                FakeNetworkException("Other network failure", NetworkException.ERROR_OTHER, -105),
                RequestPhase.RECEIVING
            )
        )
    }
}
