package io.github.khirohis.flutter_turbo_fetch

import android.content.Context
import org.mockito.Mockito
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertNotSame
import kotlin.test.assertSame

internal class CronetClientManagerTest {
    @Test
    fun acquireEngine_reusesSingleSharedEngineInstance() {
        var factoryCount = 0
        val createdEngine = FakeCronetEngine()
        val manager = CronetClientManager(
            engineFactory = {
                factoryCount++
                createdEngine
            },
            shutdownRunner = { task -> task() }
        )

        val mockContext = Mockito.mock(Context::class.java)
        manager.ensureReady(mockContext)

        val handle1 = manager.acquireEngine()
        val handle2 = manager.acquireEngine()

        assertEquals(1, factoryCount)
        assertSame(createdEngine, handle1.engine)
        assertSame(createdEngine, handle2.engine)

        handle1.release()
        handle2.release()
        manager.shutdown()
    }

    @Test
    fun shutdown_withNoInFlightRequests_shutsDownEngineImmediatelyViaRunner() {
        val fakeEngine = FakeCronetEngine()
        val scheduledShutdowns = mutableListOf<() -> Unit>()
        val manager = CronetClientManager(
            engineFactory = { fakeEngine },
            shutdownRunner = { task -> scheduledShutdowns.add(task) }
        )

        manager.ensureReady(Mockito.mock(Context::class.java))
        val handle = manager.acquireEngine()
        handle.release()

        manager.shutdown()
        assertEquals(1, scheduledShutdowns.size)
        assertEquals(0, fakeEngine.shutdownCount)

        scheduledShutdowns.single().invoke()
        assertEquals(1, fakeEngine.shutdownCount)
    }

    @Test
    fun shutdown_withInFlightRequests_defersEngineShutdownUntilAllReleased() {
        val fakeEngine = FakeCronetEngine()
        val manager = CronetClientManager(
            engineFactory = { fakeEngine },
            shutdownRunner = { task -> task() }
        )

        manager.ensureReady(Mockito.mock(Context::class.java))
        val handle1 = manager.acquireEngine()
        val handle2 = manager.acquireEngine()

        // Shutdown requested while 2 handles are still active must defer engine.shutdown()
        manager.shutdown()
        assertEquals(0, fakeEngine.shutdownCount)

        // First release still has 1 active request remaining
        handle1.release()
        assertEquals(0, fakeEngine.shutdownCount)

        // Idempotent duplicate release on handle1 must not decrement twice or trigger shutdown early
        handle1.release()
        assertEquals(0, fakeEngine.shutdownCount)

        // Last release triggers deferred engine.shutdown() exactly once
        handle2.release()
        assertEquals(1, fakeEngine.shutdownCount)

        // Duplicate release on handle2 must remain idempotent
        handle2.release()
        assertEquals(1, fakeEngine.shutdownCount)
    }

    @Test
    fun acquireEngine_afterShutdown_createsFreshEngineInstance() {
        val engines = ArrayDeque(listOf(FakeCronetEngine(), FakeCronetEngine()))
        val manager = CronetClientManager(
            engineFactory = { engines.removeFirst() },
            shutdownRunner = { task -> task() }
        )

        val mockContext = Mockito.mock(Context::class.java)
        manager.ensureReady(mockContext)

        val firstHandle = manager.acquireEngine()
        val firstEngine = firstHandle.engine as FakeCronetEngine
        firstHandle.release()

        manager.shutdown()
        assertEquals(1, firstEngine.shutdownCount)

        val recoveredHandle = manager.acquireEngine()
        val recoveredEngine = recoveredHandle.engine as FakeCronetEngine
        assertNotSame(firstEngine, recoveredEngine)
        assertEquals(0, recoveredEngine.shutdownCount)

        recoveredHandle.release()
        manager.shutdown()
        assertEquals(1, recoveredEngine.shutdownCount)
    }

    @Test
    fun acquireEngine_withoutContextOrWhenFactoryFails_throwsException() {
        val uninitializedManager = CronetClientManager(
            engineFactory = { FakeCronetEngine() },
            shutdownRunner = { task -> task() }
        )
        assertFailsWith<IllegalStateException> {
            uninitializedManager.acquireEngine()
        }

        val failingManager = CronetClientManager(
            engineFactory = { throw UnsatisfiedLinkError("Cronet provider unavailable") },
            shutdownRunner = { task -> task() }
        )
        // ensureReady must not crash eagerly even if provider setup is broken
        failingManager.ensureReady(Mockito.mock(Context::class.java))
        assertFailsWith<UnsatisfiedLinkError> {
            failingManager.acquireEngine()
        }
    }
}
