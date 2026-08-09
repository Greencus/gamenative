package app.gamenative.xr

import android.content.Context
import com.winlator.container.Container
import com.winlator.container.ContainerData

/** Per-container settings consumed by the Android OpenXR host. */
object XrContainerSettings {
    internal const val PREFERENCES_NAME = "gamenative_xr_container_settings"
    private const val STORED_SUFFIX = "stored"
    private const val RENDER_SCALE_SUFFIX = "renderScale"
    private const val OPENCOMPOSITE_SUFFIX = "openComposite"
    private const val THEATER_SCREEN_SUFFIX = "theaterScreen"
    private const val CLOCK_SUFFIX = "clock"

    const val RENDER_SCALE_EXTRA = "xrRenderScale"
    const val OPENCOMPOSITE_EXTRA = "xrOpenCompositeEnabled"
    const val THEATER_SCREEN_EXTRA = "xrTheaterScreenEnabled"
    const val CLOCK_EXTRA = "xrClockEnabled"

    const val DEFAULT_RENDER_SCALE = 100

    val renderScaleOptions = listOf(50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150)

    data class Values(
        val renderScale: Int = DEFAULT_RENDER_SCALE,
        val openCompositeEnabled: Boolean = true,
        val theaterScreenEnabled: Boolean = true,
        val clockEnabled: Boolean = true,
    )

    fun read(container: Container): Values = Values(
        renderScale = sanitizeRenderScale(
            container.getExtra(RENDER_SCALE_EXTRA, DEFAULT_RENDER_SCALE.toString()).toIntOrNull(),
        ),
        openCompositeEnabled = readBoolean(container, OPENCOMPOSITE_EXTRA, true),
        theaterScreenEnabled = readBoolean(container, THEATER_SCREEN_EXTRA, true),
        clockEnabled = readBoolean(container, CLOCK_EXTRA, true),
    )

    /**
     * Read the durable per-app settings used by the XR activity. Existing installs are migrated
     * once from the container JSON, after which container rewrites cannot reset VR preferences.
     */
    fun read(context: Context, appId: String, container: Container): Values {
        val preferences = context.getSharedPreferences(PREFERENCES_NAME, Context.MODE_PRIVATE)
        if (!preferences.getBoolean(key(appId, STORED_SUFFIX), false)) {
            return read(container).also { migrated ->
                persist(context, appId, migrated)
            }
        }

        return Values(
            renderScale = sanitizeRenderScale(
                preferences.getInt(key(appId, RENDER_SCALE_SUFFIX), DEFAULT_RENDER_SCALE),
            ),
            openCompositeEnabled = preferences.getBoolean(key(appId, OPENCOMPOSITE_SUFFIX), true),
            theaterScreenEnabled = preferences.getBoolean(key(appId, THEATER_SCREEN_SUFFIX), true),
            clockEnabled = preferences.getBoolean(key(appId, CLOCK_SUFFIX), true),
        )
    }

    fun from(data: ContainerData): Values = Values(
        renderScale = sanitizeRenderScale(data.xrRenderScale),
        openCompositeEnabled = data.xrOpenCompositeEnabled,
        theaterScreenEnabled = data.xrTheaterScreenEnabled,
        clockEnabled = data.xrClockEnabled,
    )

    fun write(container: Container, values: Values) {
        container.putExtra(RENDER_SCALE_EXTRA, sanitizeRenderScale(values.renderScale).toString())
        container.putExtra(OPENCOMPOSITE_EXTRA, values.openCompositeEnabled.toString())
        container.putExtra(THEATER_SCREEN_EXTRA, values.theaterScreenEnabled.toString())
        container.putExtra(CLOCK_EXTRA, values.clockEnabled.toString())
    }

    /** Synchronously persist a complete settings snapshot before a launch can begin. */
    fun persist(context: Context, appId: String, values: Values): Boolean {
        val sanitized = values.copy(renderScale = sanitizeRenderScale(values.renderScale))
        return context.getSharedPreferences(PREFERENCES_NAME, Context.MODE_PRIVATE)
            .edit()
            .putInt(key(appId, RENDER_SCALE_SUFFIX), sanitized.renderScale)
            .putBoolean(key(appId, OPENCOMPOSITE_SUFFIX), sanitized.openCompositeEnabled)
            .putBoolean(key(appId, THEATER_SCREEN_SUFFIX), sanitized.theaterScreenEnabled)
            .putBoolean(key(appId, CLOCK_SUFFIX), sanitized.clockEnabled)
            .putBoolean(key(appId, STORED_SUFFIX), true)
            .commit()
    }

    fun sanitizeRenderScale(value: Int?): Int =
        value?.takeIf(renderScaleOptions::contains) ?: DEFAULT_RENDER_SCALE

    /**
     * VR settings are persistent container preferences, not launch-intent overrides. Preserve
     * them whenever a temporary launch configuration is merged with the saved container data.
     */
    fun preserveFrom(base: ContainerData, candidate: ContainerData): ContainerData = candidate.copy(
        xrRenderScale = base.xrRenderScale,
        xrOpenCompositeEnabled = base.xrOpenCompositeEnabled,
        xrTheaterScreenEnabled = base.xrTheaterScreenEnabled,
        xrClockEnabled = base.xrClockEnabled,
    )

    private fun readBoolean(container: Container, key: String, default: Boolean): Boolean =
        container.getExtra(key, default.toString()).toBooleanStrictOrNull() ?: default

    private fun key(appId: String, suffix: String): String = "$appId.$suffix"
}
