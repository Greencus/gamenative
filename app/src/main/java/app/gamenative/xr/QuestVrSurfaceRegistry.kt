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
    private var presentationSuspended = false

    @Synchronized
    fun setSurface(surface: Surface, width: Int, height: Int) {
        target = Target(surface, width, height)
        if (!presentationSuspended) {
            listeners.forEach { it.onXrSurfaceReady(target!!) }
        }
    }

    @Synchronized
    fun clearSurface() {
        target = null
        listeners.forEach { it.onXrSurfaceDestroyed() }
    }

    /**
     * Detaches the flat X-server compositor while retaining its SurfaceTexture target.
     *
     * True stereo uses the game's OpenXR eye buffers directly, so continuing to composite
     * the Wine desktop wastes GPU time and copies window content that can no longer be seen.
     * Keeping [target] allows the renderer to reattach without restarting Wine when stereo
     * transport disappears or the runtime falls back to theater presentation.
     */
    @Synchronized
    fun setPresentationSuspended(suspended: Boolean) {
        if (presentationSuspended == suspended) return
        presentationSuspended = suspended
        if (suspended) {
            listeners.forEach { it.onXrSurfaceDestroyed() }
        } else {
            target?.let { current -> listeners.forEach { it.onXrSurfaceReady(current) } }
        }
    }

    @Synchronized
    fun isPresentationSuspended(): Boolean = presentationSuspended

    @Synchronized
    fun addListener(listener: Listener) {
        listeners.add(listener)
        if (!presentationSuspended) target?.let(listener::onXrSurfaceReady)
    }

    @Synchronized
    fun removeListener(listener: Listener) {
        listeners.remove(listener)
    }
}
