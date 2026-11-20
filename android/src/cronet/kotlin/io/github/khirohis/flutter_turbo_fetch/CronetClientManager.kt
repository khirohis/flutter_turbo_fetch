package io.github.khirohis.flutter_turbo_fetch

import android.content.Context
import org.chromium.net.CronetEngine
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Manages the shared [CronetEngine] lifecycle with reference-counted deferred shutdown.
 *
 * `CronetEngine.shutdown()` throws [IllegalStateException] if called while requests are active or
 * from a Cronet network thread. Wrapping each active engine in [ManagedCronetEngine] ensures
 * shutdown is deferred to a background daemon thread once all in-flight handles are released, and a
 * new engine instance is created lazily on subsequent requests after detachment/re-attachment.
 */
internal class CronetClientManager(
    private val engineFactory: (Context) -> CronetEngine = ::createDefaultEngine,
    private val shutdownRunner: (() -> Unit) -> Unit = ::runOnDaemonThread
) {
    /**
     * Scoped lease for the currently active [CronetEngine]. Callers must invoke [release] once
     * the associated Cronet request reaches a terminal state (`onSucceeded`, `onFailed`,
     * `onCanceled`, or synchronous startup failure).
     */
    class EngineHandle(
        val engine: CronetEngine,
        private val onRelease: () -> Unit
    ) {
        private val released = AtomicBoolean(false)

        fun release() {
            if (released.compareAndSet(false, true)) {
                onRelease()
            }
        }
    }

    private class ManagedCronetEngine(
        val engine: CronetEngine,
        private val shutdownRunner: (() -> Unit) -> Unit
    ) {
        private var inFlightCount = 0
        private var shutdownRequested = false
        private var shutdownDispatched = false

        @Synchronized
        fun tryAcquire(): EngineHandle? {
            if (shutdownRequested) {
                return null
            }
            inFlightCount++
            return EngineHandle(engine) { releaseOne() }
        }

        private fun releaseOne() {
            var shouldDispatchShutdown = false
            synchronized(this) {
                if (inFlightCount > 0) {
                    inFlightCount--
                }
                if (inFlightCount == 0 && shutdownRequested && !shutdownDispatched) {
                    shutdownDispatched = true
                    shouldDispatchShutdown = true
                }
            }
            if (shouldDispatchShutdown) {
                dispatchShutdown()
            }
        }

        fun requestShutdown() {
            var shouldDispatchShutdown = false
            synchronized(this) {
                shutdownRequested = true
                if (inFlightCount == 0 && !shutdownDispatched) {
                    shutdownDispatched = true
                    shouldDispatchShutdown = true
                }
            }
            if (shouldDispatchShutdown) {
                dispatchShutdown()
            }
        }

        private fun dispatchShutdown() {
            shutdownRunner {
                try {
                    engine.shutdown()
                } catch (_: IllegalStateException) {
                    // Defensive guard if engine was already shut down concurrently.
                }
            }
        }
    }

    @Volatile
    private var appContext: Context? = null

    private var currentManaged: ManagedCronetEngine? = null

    @Synchronized
    fun ensureReady(context: Context?) {
        if (context != null) {
            appContext = context.applicationContext ?: context
        }
    }

    @Synchronized
    fun acquireEngine(): EngineHandle {
        currentManaged?.tryAcquire()?.let { return it }

        val ctx = appContext
            ?: throw IllegalStateException("CronetClientManager is not initialized with a Context.")

        val created = ManagedCronetEngine(engineFactory(ctx), shutdownRunner)
        currentManaged = created
        return checkNotNull(created.tryAcquire())
    }

    fun shutdown() {
        val target: ManagedCronetEngine?
        synchronized(this) {
            target = currentManaged
            currentManaged = null
        }
        target?.requestShutdown()
    }

    companion object {
        private fun createDefaultEngine(context: Context): CronetEngine =
            CronetEngine.Builder(context)
                .enableHttp2(true)
                .enableQuic(false)
                .build()

        private fun runOnDaemonThread(task: () -> Unit) {
            Thread(task, "turbo-fetch-cronet-shutdown").apply {
                isDaemon = true
                start()
            }
        }
    }
}
