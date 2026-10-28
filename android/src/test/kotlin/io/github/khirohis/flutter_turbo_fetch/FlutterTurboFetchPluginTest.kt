package io.github.khirohis.flutter_turbo_fetch

import okhttp3.Call
import java.io.IOException
import java.io.InterruptedIOException
import java.net.ConnectException
import java.net.SocketTimeoutException
import java.net.UnknownHostException
import javax.net.ssl.SSLException
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull

internal class FlutterTurboFetchPluginTest {
    private fun createDummyCall(canceled: Boolean = false): Call {
        return java.lang.reflect.Proxy.newProxyInstance(
            Call::class.java.classLoader,
            arrayOf(Call::class.java)
        ) { _, method, _ ->
            if (method.name == "isCanceled") canceled
            else null
        } as Call
    }

    @Test
    fun pluginInstance_canBeCreated() {
        val plugin = FlutterTurboFetchPlugin()
        assertNotNull(plugin)
    }

    @Test
    fun classifyException_classifiesCanceledCall() {
        val plugin = FlutterTurboFetchPlugin()
        val call = createDummyCall(canceled = true)
        val result = plugin.classifyException(call, IOException("Generic"), isSendingBody = false)
        assertEquals("cancel", result)
    }

    @Test
    fun classifyException_classifiesTimeoutsCorrectly() {
        val plugin = FlutterTurboFetchPlugin()
        val call = createDummyCall(canceled = false)

        // Connect timeout
        val connectTimeout = SocketTimeoutException("failed to connect")
        assertEquals("connectionTimeout", plugin.classifyException(call, connectTimeout, isSendingBody = false))

        // Send timeout
        val sendTimeout = SocketTimeoutException("write timed out")
        assertEquals("sendTimeout", plugin.classifyException(call, sendTimeout, isSendingBody = true))

        // Receive timeout
        val receiveTimeout = SocketTimeoutException("timeout reading from source")
        assertEquals("receiveTimeout", plugin.classifyException(call, receiveTimeout, isSendingBody = false))

        // InterruptedIOException timeout
        val interruptedTimeout = InterruptedIOException("timeout")
        assertEquals("sendTimeout", plugin.classifyException(call, interruptedTimeout, isSendingBody = true))
        assertEquals("receiveTimeout", plugin.classifyException(call, interruptedTimeout, isSendingBody = false))

        // InterruptedIOException cancel
        val interruptedCancel = InterruptedIOException("thread interrupted")
        assertEquals("cancel", plugin.classifyException(call, interruptedCancel, isSendingBody = false))
    }

    @Test
    fun classifyException_classifiesCertificateAndConnectionErrors() {
        val plugin = FlutterTurboFetchPlugin()
        val call = createDummyCall(canceled = false)

        val sslException = SSLException("Handshake failed")
        assertEquals("badCertificate", plugin.classifyException(call, sslException, isSendingBody = false))

        val unknownHost = UnknownHostException("example.invalid")
        assertEquals("connectionError", plugin.classifyException(call, unknownHost, isSendingBody = false))

        val connectException = ConnectException("Connection refused")
        assertEquals("connectionError", plugin.classifyException(call, connectException, isSendingBody = false))

        val genericIo = IOException("Unexpected EOF")
        assertEquals("unknown", plugin.classifyException(call, genericIo, isSendingBody = false))
    }
}
