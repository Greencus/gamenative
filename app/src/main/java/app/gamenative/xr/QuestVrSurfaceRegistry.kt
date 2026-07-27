package app.gamenative.xr

import android.view.Surface
import java.util.concurrent.CopyOnWriteArraySet

object QuestVrSurfaceRegistry {
    data class Target(
        val surface: Surface,
        val width: Int,
        val height: Int,
    )

    interface Listener {
        fun onXrSurfaceReady(target: Target)
        fun onXrSurfaceDestroyed()
    }

    private val listeners = CopyOnWriteArraySet<Listener>()
    private var target: Target? = null

    @Synchronized
    fun setSurface(surface: Surface, width: Int, height: Int) {
        target = Target(surface, width, height)
        listeners.forEach { it.onXrSurfaceReady(target!!) }
    }

    @Synchronized
    fun clearSurface() {
        target = null
        listeners.forEach { it.onXrSurfaceDestroyed() }
    }

    @Synchronized
    fun addListener(listener: Listener) {
        listeners.add(listener)
        target?.let(listener::onXrSurfaceReady)
    }

    @Synchronized
    fun removeListener(listener: Listener) {
        listeners.remove(listener)
    }
}
