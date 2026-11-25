package io.github.khirohis.flutter_turbo_fetch

import org.chromium.net.CronetException
import org.chromium.net.NetworkException
import org.chromium.net.UploadDataProvider
import org.chromium.net.UploadDataSink
import org.chromium.net.UrlRequest
import org.chromium.net.UrlResponseInfo
import java.nio.ByteBuffer
import java.util.Locale
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

/**
 * Per-request Cronet execution state machine managing phase timers, watchdog idle timeout,
 * direct [ByteBuffer] lease, upload streaming, and one-shot response body aggregation.
 */
internal class ActiveCronetCall(
    val request: TurboHttpRequest,
    private val bufferPool: CronetDirectByteBufferPool,
    private val timerScheduler: CronetTimerScheduler,
    private val onComplete: (ActiveCronetCall, TurboHttpResponse) -> Unit
) {
    private val completed = AtomicBoolean(false)
    private val urlRequestRef = AtomicReference<UrlRequest?>()
    private val engineHandleRef = AtomicReference<CronetClientManager.EngineHandle?>()
    private val leasedBufferRef = AtomicReference<ByteBuffer?>()
    private val timerLock = Any()
    private var currentTimer: CronetTimerHandle? = null

    @Volatile
    private var phase: RequestPhase = RequestPhase.CONNECTING

    @Volatile
    private var lastReceiveActivityMs: Long = 0L

    private val bodyAccumulator = ResponseBodyAccumulator()

    val isCompleted: Boolean
        get() = completed.get()

    fun attachEngineHandle(handle: CronetClientManager.EngineHandle) {
        engineHandleRef.set(handle)
    }

    fun attachUrlRequest(urlRequest: UrlRequest) {
        urlRequestRef.set(urlRequest)
    }

    fun releaseResources() {
        leasedBufferRef.getAndSet(null)?.let { bufferPool.release(it) }
        engineHandleRef.getAndSet(null)?.release()
    }

    fun createUploadProvider(bodyBytes: ByteArray): UploadDataProvider =
        TrackingUploadDataProvider(bodyBytes)

    private fun replaceTimer(delayMs: Long?, onTimeout: () -> Unit) {
        synchronized(timerLock) {
            currentTimer?.cancel()
            currentTimer = null
            if (!completed.get() && delayMs != null && delayMs > 0L) {
                currentTimer = timerScheduler.schedule(delayMs, onTimeout)
            }
        }
    }

    fun cancelTimer() {
        replaceTimer(null) {}
    }

    fun schedulePhaseTimer(delayMs: Long?, errorCode: String, message: String) {
        replaceTimer(delayMs) {
            fail(errorCode, message)
            urlRequestRef.get()?.cancel()
        }
    }

    fun startReceiveWatchdog() {
        val timeoutMs = request.receiveTimeoutMs
        if (timeoutMs == null || timeoutMs <= 0L) {
            cancelTimer()
            return
        }
        lastReceiveActivityMs = timerScheduler.nowMs()
        scheduleReceiveWatchdog(timeoutMs, timeoutMs)
    }

    private fun scheduleReceiveWatchdog(timeoutMs: Long, delayMs: Long) {
        replaceTimer(delayMs) {
            if (completed.get()) {
                return@replaceTimer
            }
            val elapsedMs = (timerScheduler.nowMs() - lastReceiveActivityMs).coerceAtLeast(0L)
            val remainingMs = timeoutMs - elapsedMs
            if (remainingMs > 0L) {
                scheduleReceiveWatchdog(timeoutMs, remainingMs)
            } else {
                fail("receiveTimeout", "Response receive timed out after ${timeoutMs}ms.")
                urlRequestRef.get()?.cancel()
            }
        }
    }

    fun cancel() {
        cancelTimer()
        val urlRequest = urlRequestRef.get()
        if (urlRequest != null) {
            urlRequest.cancel()
        } else {
            failAndRelease("cancel", "Request was cancelled.")
        }
    }

    fun finish(response: TurboHttpResponse) {
        if (completed.compareAndSet(false, true)) {
            cancelTimer()
            onComplete(this, response)
        }
    }

    fun fail(errorCode: String, errorMessage: String) {
        finish(TurboHttpResponse(errorCode = errorCode, errorMessage = errorMessage))
    }

    fun failAndRelease(errorCode: String, errorMessage: String) {
        releaseResources()
        fail(errorCode, errorMessage)
    }

    private fun failAndCancel(urlRequest: UrlRequest, errorCode: String, errorMessage: String) {
        fail(errorCode, errorMessage)
        urlRequest.cancel()
    }

    val callback: UrlRequest.Callback = object : UrlRequest.Callback() {
        override fun onRedirectReceived(
            urlRequest: UrlRequest,
            info: UrlResponseInfo,
            newLocationUrl: String
        ) {
            if (completed.get()) {
                urlRequest.cancel()
                return
            }
            if (request.followRedirects) {
                urlRequest.followRedirect()
            } else {
                finish(
                    TurboHttpResponse(
                        statusCode = info.httpStatusCode.toLong(),
                        statusMessage = info.httpStatusText,
                        headers = normalizeHeaders(info),
                        body = ByteArray(0)
                    )
                )
                urlRequest.cancel()
            }
        }

        override fun onResponseStarted(urlRequest: UrlRequest, info: UrlResponseInfo) {
            if (completed.get()) {
                urlRequest.cancel()
                return
            }
            phase = RequestPhase.RECEIVING

            val maxBytes = request.maxResponseBytes?.coerceAtLeast(0L)
            val contentLength = extractContentLength(info)
            if (maxBytes != null && contentLength != null && contentLength > maxBytes) {
                failAndCancel(
                    urlRequest,
                    "maxResponseBytesExceeded",
                    "Response body exceeded maxResponseBytes limit ($maxBytes bytes, contentLength: $contentLength)"
                )
                return
            }

            startReceiveWatchdog()

            val readBuffer = bufferPool.acquire()
            leasedBufferRef.set(readBuffer)
            urlRequest.read(readBuffer)
        }

        override fun onReadCompleted(
            urlRequest: UrlRequest,
            info: UrlResponseInfo,
            byteBuffer: ByteBuffer
        ) {
            if (completed.get()) {
                urlRequest.cancel()
                return
            }

            byteBuffer.flip()
            val bytesRead = byteBuffer.remaining()
            val maxBytes = request.maxResponseBytes?.coerceAtLeast(0L)
            if (maxBytes != null && bytesRead.toLong() > maxBytes - bodyAccumulator.totalBytes) {
                failAndCancel(
                    urlRequest,
                    "maxResponseBytesExceeded",
                    "Response body exceeded maxResponseBytes limit ($maxBytes bytes)"
                )
                return
            }

            if (bytesRead > 0 && !bodyAccumulator.append(byteBuffer, bytesRead)) {
                failAndCancel(urlRequest, "unknown", "Response body exceeds maximum ByteArray size")
                return
            }
            byteBuffer.clear()

            // Record activity timestamp without canceling/re-scheduling timer on every chunk.
            lastReceiveActivityMs = timerScheduler.nowMs()

            urlRequest.read(byteBuffer)
        }

        override fun onSucceeded(urlRequest: UrlRequest, info: UrlResponseInfo) {
            val bodyBytes = bodyAccumulator.toByteArray()
            releaseResources()
            finish(
                TurboHttpResponse(
                    statusCode = info.httpStatusCode.toLong(),
                    statusMessage = info.httpStatusText,
                    headers = normalizeHeaders(info),
                    body = bodyBytes
                )
            )
        }

        override fun onFailed(
            urlRequest: UrlRequest,
            info: UrlResponseInfo?,
            error: CronetException?
        ) {
            val errorCode = if (error is NetworkException) {
                CronetErrorClassifier.classify(error, phase)
            } else {
                "unknown"
            }
            failAndRelease(errorCode, error?.message ?: "Cronet request failed.")
        }

        override fun onCanceled(urlRequest: UrlRequest, info: UrlResponseInfo?) {
            failAndRelease("cancel", "Request was cancelled.")
        }
    }

    private inner class TrackingUploadDataProvider(
        private val bytes: ByteArray
    ) : UploadDataProvider() {
        private var offset = 0
        private var uploadStarted = false

        override fun getLength(): Long = bytes.size.toLong()

        override fun read(uploadDataSink: UploadDataSink, byteBuffer: ByteBuffer) {
            val remainingBytes = bytes.size - offset
            val toWrite = minOf(byteBuffer.remaining(), remainingBytes)
            if (toWrite > 0) {
                byteBuffer.put(bytes, offset, toWrite)
                offset += toWrite
            }

            if (offset >= bytes.size) {
                uploadStarted = true
                phase = RequestPhase.RECEIVING
                startReceiveWatchdog()
            } else if (!uploadStarted) {
                uploadStarted = true
                phase = RequestPhase.SENDING_BODY
                schedulePhaseTimer(
                    delayMs = request.sendTimeoutMs,
                    errorCode = "sendTimeout",
                    message = "Request send timed out after ${request.sendTimeoutMs}ms."
                )
            }

            uploadDataSink.onReadSucceeded(false)
        }

        override fun rewind(uploadDataSink: UploadDataSink) {
            offset = 0
            uploadStarted = false
            uploadDataSink.onRewindSucceeded()
        }
    }

    private class ResponseBodyAccumulator {
        private val chunks = ArrayList<ByteArray>(4)

        var totalBytes: Long = 0L
            private set

        fun append(byteBuffer: ByteBuffer, bytesRead: Int): Boolean {
            val newTotal = totalBytes + bytesRead.toLong()
            if (newTotal > Int.MAX_VALUE) {
                return false
            }
            totalBytes = newTotal
            val chunk = ByteArray(bytesRead)
            byteBuffer.get(chunk)
            chunks.add(chunk)
            return true
        }

        fun toByteArray(): ByteArray = when (chunks.size) {
            0 -> ByteArray(0)
            1 -> chunks[0]
            else -> {
                val merged = ByteArray(totalBytes.toInt())
                var offset = 0
                for (chunk in chunks) {
                    System.arraycopy(chunk, 0, merged, offset, chunk.size)
                    offset += chunk.size
                }
                merged
            }
        }
    }

    companion object {
        private fun normalizeHeaders(info: UrlResponseInfo): Map<String?, Any?> {
            val list = info.allHeadersAsList
            val grouped = HashMap<String?, MutableList<String>>(list.size)
            for (entry in list) {
                val key = entry.key.lowercase(Locale.ROOT)
                grouped.getOrPut(key) { ArrayList(1) }.add(entry.value)
            }
            return grouped
        }

        private fun extractContentLength(info: UrlResponseInfo): Long? =
            info.allHeadersAsList
                .firstOrNull { it.key.equals("content-length", ignoreCase = true) }
                ?.value
                ?.trim()
                ?.toLongOrNull()
    }
}
