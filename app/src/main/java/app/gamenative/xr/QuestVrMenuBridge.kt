package app.gamenative.xr

/**
 * Keeps the reusable flat-game quick menu decoupled from the Quest activity.
 * The menu only publishes visibility changes, allowing the XR renderer to keep
 * its panel resources dormant while the menu is hidden.
 */
object QuestVrMenuBridge {
    @Volatile
    private var visibilityListener: ((Boolean) -> Unit)? = null

    fun setVisibilityListener(listener: ((Boolean) -> Unit)?) {
        visibilityListener = listener
    }

    fun publishVisibility(visible: Boolean) {
        visibilityListener?.invoke(visible)
    }
}
