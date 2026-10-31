package io.github.khirohis.flutter_turbo_fetch

import okhttp3.Call
import okhttp3.Interceptor
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.ResponseBody
import okhttp3.ResponseBody.Companion.toResponseBody
import okio.Buffer
import okio.BufferedSource
import java.io.IOException
import java.io.InterruptedIOException
import java.net.ConnectException
import java.net.NoRouteToHostException
import java.net.ProtocolException
import java.net.SocketException
import java.net.SocketTimeoutException
import java.net.UnknownHostException
import java.net.UnknownServiceException
import java.security.cert.CertificateException
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference
import javax.net.ssl.SSLException
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

internal class TurboHttpExecutorTest {
    private fun createDummyCall(canceled: Boolean = false): Call {
        return java.lang.reflect.Proxy.newProxyInstance(
            Call::class.java.classLoader,
            arrayOf(Call::class.java)
        ) { _, method, _ ->
            if (method.name == "isCanceled") canceled
            else null
        } as Call
    }

    private fun createRequest(
        url: String = "https://example.com/api",
        method: String = "GET",
        headers: Map<String?, String?> = emptyMap(),
        body: ByteArray? = null,
        maxResponseBytes: Long? = null
    ): TurboHttpRequest {
        return TurboHttpRequest(
            requestId = "test-req",
            url = url,
            method = method,
            headers = headers,
            body = body,
            connectTimeoutMs = 5000L,
            receiveTimeoutMs = 5000L,
            sendTimeoutMs = 5000L,
            followRedirects = true,
            maxRequestsPerHost = 32L,
            allowBadCertificates = false,
            maxResponseBytes = maxResponseBytes
        )
    }

    private fun createInterceptedClient(interceptor: Interceptor): OkHttpClient {
        return OkHttpClient.Builder()
            .addInterceptor(interceptor)
            .build()
    }

    private fun executeSync(
        executor: TurboHttpExecutor,
        client: OkHttpClient,
        request: TurboHttpRequest,
        onCallCreatedCallback: ((Call) -> Unit)? = null
    ): TurboHttpResponse {
        val latch = CountDownLatch(1)
        val resultRef = AtomicReference<TurboHttpResponse>()
        executor.execute(
            client = client,
            request = request,
            onCallCreated = { call ->
                onCallCreatedCallback?.invoke(call)
            },
            onResult = { response ->
                resultRef.set(response)
                latch.countDown()
            }
        )
        assertTrue(latch.await(5, TimeUnit.SECONDS), "Timed out waiting for TurboHttpExecutor.execute")
        return assertNotNull(resultRef.get())
    }

    @Test
    fun classifyException_classifiesCanceledCall() {
        val executor = TurboHttpExecutor()
        val call = createDummyCall(canceled = true)
        val result = executor.classifyException(call, IOException("Generic"), isSendingBody = false)
        assertEquals("cancel", result)
    }

    @Test
    fun classifyException_classifiesTimeoutsCorrectly() {
        val executor = TurboHttpExecutor()
        val call = createDummyCall(canceled = false)

        // Connect timeout
        val connectTimeout = SocketTimeoutException("failed to connect")
        assertEquals("connectionTimeout", executor.classifyException(call, connectTimeout, isSendingBody = false))

        // Send timeout
        val sendTimeout = SocketTimeoutException("write timed out")
        assertEquals("sendTimeout", executor.classifyException(call, sendTimeout, isSendingBody = true))

        // Receive timeout
        val receiveTimeout = SocketTimeoutException("timeout reading from source")
        assertEquals("receiveTimeout", executor.classifyException(call, receiveTimeout, isSendingBody = false))

        // InterruptedIOException timeout
        val interruptedTimeout = InterruptedIOException("timeout")
        assertEquals("sendTimeout", executor.classifyException(call, interruptedTimeout, isSendingBody = true))
        assertEquals("receiveTimeout", executor.classifyException(call, interruptedTimeout, isSendingBody = false))

        // InterruptedIOException cancel
        val interruptedCancel = InterruptedIOException("thread interrupted")
        assertEquals("cancel", executor.classifyException(call, interruptedCancel, isSendingBody = false))
    }

    @Test
    fun classifyException_classifiesCertificateAndConnectionErrors() {
        val executor = TurboHttpExecutor()
        val call = createDummyCall(canceled = false)

        val sslException = SSLException("Handshake failed")
        assertEquals("badCertificate", executor.classifyException(call, sslException, isSendingBody = false))

        val certCauseException = IOException("Cert failed", CertificateException("Untrusted"))
        assertEquals("badCertificate", executor.classifyException(call, certCauseException, isSendingBody = false))

        assertEquals("connectionError", executor.classifyException(call, UnknownHostException("example.invalid"), false))
        assertEquals("connectionError", executor.classifyException(call, ConnectException("Connection refused"), false))
        assertEquals("connectionError", executor.classifyException(call, NoRouteToHostException("No route"), false))
        assertEquals("connectionError", executor.classifyException(call, SocketException("Socket closed"), false))
        assertEquals("connectionError", executor.classifyException(call, ProtocolException("Invalid HTTP"), false))
        assertEquals("connectionError", executor.classifyException(call, UnknownServiceException("Unknown service"), false))

        val genericIo = IOException("Unexpected EOF")
        assertEquals("unknown", executor.classifyException(call, genericIo, isSendingBody = false))
    }

    @Test
    fun execute_invalidUrl_returnsConnectionErrorWithoutCreatingCall() {
        val executor = TurboHttpExecutor()
        val callCreated = AtomicBoolean(false)
        val response = executeSync(
            executor = executor,
            client = OkHttpClient(),
            request = createRequest(url = "http://[::1"),
            onCallCreatedCallback = { callCreated.set(true) }
        )

        assertFalse(callCreated.get(), "onCallCreated must not be called when request building fails")
        assertEquals("connectionError", response.errorCode)
        assertNotNull(response.errorMessage)
    }

    @Test
    fun execute_invalidHeaderCharacters_returnsConnectionErrorSafelyWithoutCrash() {
        val executor = TurboHttpExecutor()
        val callCreated = AtomicBoolean(false)
        val invalidHeaders: Map<String?, String?> = mapOf("X-Invalid\r\nHeader" to "value\r\ninjection")

        val response = executeSync(
            executor = executor,
            client = OkHttpClient(),
            request = createRequest(headers = invalidHeaders),
            onCallCreatedCallback = { callCreated.set(true) }
        )

        assertFalse(callCreated.get(), "onCallCreated must not be called when headers are invalid")
        assertEquals("connectionError", response.errorCode)
    }

    @Test
    fun execute_invokesOnCallCreatedBeforeOnResult() {
        val executor = TurboHttpExecutor()
        val callCreatedBeforeResult = AtomicBoolean(false)
        val callCreated = AtomicBoolean(false)

        val client = createInterceptedClient { chain ->
            Response.Builder()
                .request(chain.request())
                .protocol(Protocol.HTTP_2)
                .code(200)
                .message("OK")
                .body("hello".toByteArray().toResponseBody("text/plain".toMediaType()))
                .build()
        }

        val latch = CountDownLatch(1)
        executor.execute(
            client = client,
            request = createRequest(),
            onCallCreated = {
                callCreated.set(true)
            },
            onResult = {
                callCreatedBeforeResult.set(callCreated.get())
                latch.countDown()
            }
        )

        assertTrue(latch.await(5, TimeUnit.SECONDS))
        assertTrue(callCreatedBeforeResult.get(), "onCallCreated must be executed before onResult")
    }

    @Test
    fun execute_maxResponseBytesNull_returnsFullBodyAndHeaders() {
        val executor = TurboHttpExecutor()
        val expectedBytes = "turbo-fetch-payload".encodeToByteArray()

        val client = createInterceptedClient { chain ->
            Response.Builder()
                .request(chain.request())
                .protocol(Protocol.HTTP_2)
                .code(201)
                .message("Created")
                .addHeader("X-Custom", "v1")
                .addHeader("X-Custom", "v2")
                .body(expectedBytes.toResponseBody("application/octet-stream".toMediaType()))
                .build()
        }

        val response = executeSync(executor, client, createRequest(maxResponseBytes = null))
        assertNull(response.errorCode)
        assertEquals(201L, response.statusCode)
        assertEquals("Created", response.statusMessage)
        assertContentEquals(expectedBytes, response.body)
        assertEquals(listOf("v1", "v2"), response.headers?.get("x-custom"))
    }

    @Test
    fun execute_maxResponseBytesExceededByContentLength_returnsExceededError() {
        val executor = TurboHttpExecutor()
        val payload = ByteArray(100) { 1 }

        val client = createInterceptedClient { chain ->
            Response.Builder()
                .request(chain.request())
                .protocol(Protocol.HTTP_2)
                .code(200)
                .message("OK")
                .body(payload.toResponseBody("application/octet-stream".toMediaType()))
                .build()
        }

        val response = executeSync(executor, client, createRequest(maxResponseBytes = 50L))
        assertEquals("maxResponseBytesExceeded", response.errorCode)
        assertNull(response.body)
    }

    @Test
    fun execute_maxResponseBytesExceededWhenContentLengthUnknown_returnsExceededError() {
        val executor = TurboHttpExecutor()
        val payload = ByteArray(64) { 2 }

        // Simulate streaming response with unknown contentLength (-1L)
        val unknownLengthBody = object : ResponseBody() {
            override fun contentType() = "application/octet-stream".toMediaType()
            override fun contentLength(): Long = -1L
            override fun source(): BufferedSource = Buffer().write(payload)
        }

        val client = createInterceptedClient { chain ->
            Response.Builder()
                .request(chain.request())
                .protocol(Protocol.HTTP_2)
                .code(200)
                .message("OK")
                .body(unknownLengthBody)
                .build()
        }

        val response = executeSync(executor, client, createRequest(maxResponseBytes = 32L))
        assertEquals("maxResponseBytesExceeded", response.errorCode)
        assertNull(response.body)
    }

    @Test
    fun execute_maxResponseBytesExactBoundary_succeeds() {
        val executor = TurboHttpExecutor()
        val payload = ByteArray(32) { 3 }

        val client = createInterceptedClient { chain ->
            Response.Builder()
                .request(chain.request())
                .protocol(Protocol.HTTP_2)
                .code(200)
                .message("OK")
                .body(payload.toResponseBody("application/octet-stream".toMediaType()))
                .build()
        }

        val response = executeSync(executor, client, createRequest(maxResponseBytes = 32L))
        assertNull(response.errorCode)
        assertContentEquals(payload, response.body)
    }

    @Test
    fun execute_maxResponseBytesLongMaxValue_returnsFullBodyWithoutTruncation() {
        val executor = TurboHttpExecutor()
        val payload = "full-body-at-long-max-value".encodeToByteArray()

        // Use unknown contentLength (-1L) so source reading path is fully exercised
        val unknownLengthBody = object : ResponseBody() {
            override fun contentType() = "text/plain".toMediaType()
            override fun contentLength(): Long = -1L
            override fun source(): BufferedSource = Buffer().write(payload)
        }

        val client = createInterceptedClient { chain ->
            Response.Builder()
                .request(chain.request())
                .protocol(Protocol.HTTP_2)
                .code(200)
                .message("OK")
                .body(unknownLengthBody)
                .build()
        }

        val response = executeSync(executor, client, createRequest(maxResponseBytes = Long.MAX_VALUE))
        assertNull(response.errorCode)
        assertContentEquals(payload, response.body)
    }

    @Test
    fun execute_unexpectedRuntimeExceptionDuringResponseRead_returnsUnknownErrorSafely() {
        val executor = TurboHttpExecutor()

        val failingBody = object : ResponseBody() {
            override fun contentType() = "text/plain".toMediaType()
            override fun contentLength(): Long = 10L
            override fun source(): BufferedSource {
                throw IllegalStateException("Corrupted internal buffer state")
            }
        }

        val client = createInterceptedClient { chain ->
            Response.Builder()
                .request(chain.request())
                .protocol(Protocol.HTTP_2)
                .code(200)
                .message("OK")
                .body(failingBody)
                .build()
        }

        val response = executeSync(executor, client, createRequest())
        assertEquals("unknown", response.errorCode)
        assertNotNull(response.errorMessage)
    }
}

