package app.gamenative.xr

import android.view.Surface
import io.mockk.mockk
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

class QuestVrSurfaceRegistryTest {
    private class RecordingListener : QuestVrSurfaceRegistry.Listener {
        val readyTargets = mutableListOf<QuestVrSurfaceRegistry.Target>()
        var destroyedCount = 0

        override fun onXrSurfaceReady(target: QuestVrSurfaceRegistry.Target) {
            readyTargets += target
        }

        override fun onXrSurfaceDestroyed() {
            destroyedCount++
        }
    }

    @Before
    fun resetRegistry() {
        QuestVrSurfaceRegistry.clearSurface()
        QuestVrSurfaceRegistry.setPresentationSuspended(false)
    }

    @After
    fun cleanupRegistry() {
        QuestVrSurfaceRegistry.clearSurface()
        QuestVrSurfaceRegistry.setPresentationSuspended(false)
    }

    @Test
    fun `suspension detaches the renderer and resume reuses the retained target`() {
        val listener = RecordingListener()
        val surface = mockk<Surface>(relaxed = true)
        QuestVrSurfaceRegistry.addListener(listener)

        QuestVrSurfaceRegistry.setSurface(surface, 1920, 1080)
        QuestVrSurfaceRegistry.setPresentationSuspended(true)
        QuestVrSurfaceRegistry.setPresentationSuspended(false)

        assertFalse(QuestVrSurfaceRegistry.isPresentationSuspended())
        assertEquals(2, listener.readyTargets.size)
        assertEquals(1, listener.destroyedCount)
        assertTrue(listener.readyTargets.all { it.surface === surface })
        QuestVrSurfaceRegistry.removeListener(listener)
    }

    @Test
    fun `surface created while suspended is attached only after resume`() {
        val listener = RecordingListener()
        val surface = mockk<Surface>(relaxed = true)
        QuestVrSurfaceRegistry.addListener(listener)

        QuestVrSurfaceRegistry.setPresentationSuspended(true)
        QuestVrSurfaceRegistry.setSurface(surface, 1280, 720)
        assertTrue(listener.readyTargets.isEmpty())

        QuestVrSurfaceRegistry.setPresentationSuspended(false)
        assertEquals(1, listener.readyTargets.size)
        assertTrue(listener.readyTargets.single().surface === surface)
        QuestVrSurfaceRegistry.removeListener(listener)
    }
}
