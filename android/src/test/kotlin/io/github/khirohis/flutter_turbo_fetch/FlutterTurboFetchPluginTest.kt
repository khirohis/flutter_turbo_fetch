package io.github.khirohis.flutter_turbo_fetch

import kotlin.test.Test
import kotlin.test.assertNotNull

internal class FlutterTurboFetchPluginTest {
    @Test
    fun pluginInstance_canBeCreated() {
        val plugin = FlutterTurboFetchPlugin()
        assertNotNull(plugin)
    }
}
