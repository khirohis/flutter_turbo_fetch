package io.github.khirohis.flutter_turbo_fetch

import org.chromium.net.CronetEngine
import org.chromium.net.CronetException
import org.chromium.net.NetworkException
import org.chromium.net.UploadDataProvider
import org.chromium.net.UploadDataSink
import org.chromium.net.UrlRequest
import org.chromium.net.UrlResponseInfo
import java.io.ByteArrayOutputStream
import java.net.URL
import java.net.URLConnection
import java.net.URLStreamHandlerFactory
import java.nio.ByteBuffer
import java.util.AbstractMap
import java.util.TreeMap
import java.util.concurrent.Executor

internal class DirectExecutor : Executor {
    override fun execute(command: Runnable) {
        command.run()
    }
}

internal fun createCronetRequest(
    requestId: String = "test-req",
    url: String = "https://example.com/api",
    method: String = "GET",
    headers: Map<String?, String?> = emptyMap(),
    body: ByteArray? = null,
    connectTimeoutMs: Long? = 5000L,
    receiveTimeoutMs: Long? = 5000L,
    sendTimeoutMs: Long? = 5000L,
    followRedirects: Boolean = true,
    allowBadCertificates: Boolean = false,
    maxResponseBytes: Long? = null
): TurboHttpRequest {
    return TurboHttpRequest(
        requestId = requestId,
        url = url,
        method = method,
        headers = headers,
        body = body,
        connectTimeoutMs = connectTimeoutMs,
        receiveTimeoutMs = receiveTimeoutMs,
        sendTimeoutMs = sendTimeoutMs,
        followRedirects = followRedirects,
        allowBadCertificates = allowBadCertificates,
        maxResponseBytes = maxResponseBytes
    )
}

internal class FakeCronetEngine : CronetEngine() {
    var shutdownCount: Int = 0
        private set

    val createdRequests = mutableListOf<FakeUrlRequest>()
    var onBuildRequest: ((FakeUrlRequest) -> Unit)? = null

    override fun getVersionString(): String = "FakeCronet/1.0"

    override fun shutdown() {
        shutdownCount++
    }

    override fun startNetLogToFile(fileName: String, logAll: Boolean) {}

    override fun stopNetLog() {}

    override fun getGlobalMetricsDeltas(): ByteArray = ByteArray(0)

    override fun openConnection(url: URL): URLConnection {
        throw UnsupportedOperationException("Not used in unit tests")
    }

    override fun createURLStreamHandlerFactory(): URLStreamHandlerFactory {
        throw UnsupportedOperationException("Not used in unit tests")
    }

    override fun newUrlRequestBuilder(
        url: String,
        callback: UrlRequest.Callback,
        executor: Executor
    ): UrlRequest.Builder {
        return FakeUrlRequestBuilder(this, url, callback, executor)
    }
}

internal class FakeUrlRequestBuilder(
    private val engine: FakeCronetEngine,
    val url: String,
    val callback: UrlRequest.Callback,
    val executor: Executor
) : UrlRequest.Builder() {
    var httpMethod: String? = null
    val headers = mutableListOf<Pair<String, String>>()
    var uploadDataProvider: UploadDataProvider? = null
    var uploadExecutor: Executor? = null
    var cacheDisabled: Boolean = false
    var directExecutorAllowed: Boolean = false
    var priority: Int = REQUEST_PRIORITY_MEDIUM

    override fun setHttpMethod(method: String): UrlRequest.Builder {
        httpMethod = method
        return this
    }

    override fun addHeader(header: String, value: String): UrlRequest.Builder {
        headers.add(header to value)
        return this
    }

    override fun disableCache(): UrlRequest.Builder {
        cacheDisabled = true
        return this
    }

    override fun setPriority(priority: Int): UrlRequest.Builder {
        this.priority = priority
        return this
    }

    override fun setUploadDataProvider(
        uploadDataProvider: UploadDataProvider,
        executor: Executor
    ): UrlRequest.Builder {
        this.uploadDataProvider = uploadDataProvider
        this.uploadExecutor = executor
        return this
    }

    override fun allowDirectExecutor(): UrlRequest.Builder {
        directExecutorAllowed = true
        return this
    }

    override fun build(): UrlRequest {
        val request = FakeUrlRequest(
            url = url,
            callback = callback,
            callbackExecutor = executor,
            httpMethod = httpMethod ?: "GET",
            headers = headers.toList(),
            uploadDataProvider = uploadDataProvider,
            uploadExecutor = uploadExecutor
        )
        engine.createdRequests.add(request)
        engine.onBuildRequest?.invoke(request)
        return request
    }
}

internal class FakeUrlRequest(
    val url: String,
    val callback: UrlRequest.Callback,
    val callbackExecutor: Executor,
    val httpMethod: String,
    val headers: List<Pair<String, String>>,
    val uploadDataProvider: UploadDataProvider?,
    val uploadExecutor: Executor?
) : UrlRequest() {
    var startCount: Int = 0
        private set
    var cancelCount: Int = 0
        private set
    var followRedirectCount: Int = 0
        private set
    val readBuffers = mutableListOf<ByteBuffer>()
    var done: Boolean = false

    var onStart: ((FakeUrlRequest) -> Unit)? = null
    var onRead: ((FakeUrlRequest, ByteBuffer) -> Unit)? = null
    var onFollowRedirect: ((FakeUrlRequest) -> Unit)? = null
    var onCancel: ((FakeUrlRequest) -> Unit)? = null

    override fun start() {
        startCount++
        onStart?.invoke(this)
    }

    override fun followRedirect() {
        followRedirectCount++
        onFollowRedirect?.invoke(this)
    }

    override fun read(buffer: ByteBuffer) {
        readBuffers.add(buffer)
        onRead?.invoke(this, buffer)
    }

    override fun cancel() {
        cancelCount++
        done = true
        onCancel?.invoke(this)
    }

    override fun isDone(): Boolean = done

    override fun getStatus(listener: UrlRequest.StatusListener) {
        listener.onStatus(UrlRequest.Status.IDLE)
    }
}

internal class FakeUrlResponseInfo(
    private val urlValue: String = "https://example.com/api",
    private val statusCode: Int = 200,
    private val statusText: String = "OK",
    private val headersList: List<Pair<String, String>> = emptyList(),
    private val urlChainValue: List<String> = listOf(urlValue),
    private val negotiatedProtocolValue: String = "h2",
    private val receivedByteCountValue: Long = 0L
) : UrlResponseInfo() {
    override fun getUrl(): String = urlValue

    override fun getUrlChain(): List<String> = urlChainValue

    override fun getHttpStatusCode(): Int = statusCode

    override fun getHttpStatusText(): String = statusText

    override fun getAllHeadersAsList(): List<Map.Entry<String, String>> =
        headersList.map { AbstractMap.SimpleImmutableEntry(it.first, it.second) }

    override fun getAllHeaders(): Map<String, List<String>> {
        val map = TreeMap<String, MutableList<String>>(String.CASE_INSENSITIVE_ORDER)
        for ((k, v) in headersList) {
            map.getOrPut(k) { mutableListOf() }.add(v)
        }
        return map
    }

    override fun wasCached(): Boolean = false

    override fun getNegotiatedProtocol(): String = negotiatedProtocolValue

    override fun getProxyServer(): String = ":0"

    override fun getReceivedByteCount(): Long = receivedByteCountValue
}

internal class FakeNetworkException(
    message: String,
    private val code: Int,
    private val internalCode: Int = 0,
    private val retryable: Boolean = false
) : NetworkException(message, null) {
    override fun getErrorCode(): Int = code

    override fun getCronetInternalErrorCode(): Int = internalCode

    override fun immediatelyRetryable(): Boolean = retryable
}

internal class FakeCronetException(
    message: String,
    cause: Throwable? = null
) : CronetException(message, cause)

internal class FakeUploadDataSink(
    private val provider: UploadDataProvider,
    private val chunkBufferSize: Int = 64
) : UploadDataSink() {
    private val output = ByteArrayOutputStream()
    private val buffer = ByteBuffer.allocate(chunkBufferSize)
    private var finished = false
    var readFailure: Exception? = null
        private set

    fun readOneChunk() {
        buffer.clear()
        provider.read(this, buffer)
    }

    fun readAll(): ByteArray {
        val expectedLength = provider.length
        while (!finished && readFailure == null) {
            buffer.clear()
            provider.read(this, buffer)
            if (expectedLength == 0L && output.size() == 0) {
                break
            }
        }
        return output.toByteArray()
    }

    override fun onReadSucceeded(finalChunk: Boolean) {
        buffer.flip()
        val bytes = ByteArray(buffer.remaining())
        buffer.get(bytes)
        output.write(bytes)
        if (finalChunk || output.size().toLong() >= provider.length) {
            finished = true
        }
    }

    override fun onReadError(exception: Exception) {
        readFailure = exception
    }

    override fun onRewindSucceeded() {}

    override fun onRewindError(exception: Exception) {}
}

internal class ManualTimerHandle(
    val delayMs: Long,
    private val task: () -> Unit
) : CronetTimerHandle {
    var canceled: Boolean = false
        private set
    var fired: Boolean = false
        private set

    val isActive: Boolean
        get() = !canceled && !fired

    override fun cancel() {
        canceled = true
    }

    fun fire() {
        if (!canceled && !fired) {
            fired = true
            task()
        }
    }
}

internal class ManualTimerScheduler : CronetTimerScheduler {
    val handles = mutableListOf<ManualTimerHandle>()
    var shutdownCalled: Boolean = false
        private set

    val activeHandles: List<ManualTimerHandle>
        get() = handles.filter { it.isActive }

    val pendingCount: Int
        get() = activeHandles.size

    override fun schedule(delayMs: Long, task: () -> Unit): CronetTimerHandle {
        val handle = ManualTimerHandle(delayMs, task)
        handles.add(handle)
        return handle
    }

    fun fireNextActive(): Boolean {
        val next = handles.firstOrNull { it.isActive } ?: return false
        next.fire()
        return true
    }

    fun fireLastActive(): Boolean {
        val last = handles.lastOrNull { it.isActive } ?: return false
        last.fire()
        return true
    }

    override fun shutdown() {
        shutdownCalled = true
        handles.forEach { it.cancel() }
    }
}
