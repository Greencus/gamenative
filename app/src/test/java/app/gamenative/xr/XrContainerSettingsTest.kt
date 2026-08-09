package app.gamenative.xr

import android.content.Context
import androidx.test.core.app.ApplicationProvider
import com.winlator.container.Container
import com.winlator.container.ContainerData
import java.io.File
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.annotation.Config
import org.robolectric.RobolectricTestRunner

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class XrContainerSettingsTest {
    private lateinit var context: Context

    @Before
    fun clearPersistentSettings() {
        context = ApplicationProvider.getApplicationContext()
        context.getSharedPreferences(XrContainerSettings.PREFERENCES_NAME, Context.MODE_PRIVATE)
            .edit()
            .clear()
            .commit()
    }

    @Test
    fun `render scale accepts supported values and defaults invalid values`() {
        assertEquals(50, XrContainerSettings.sanitizeRenderScale(50))
        assertEquals(120, XrContainerSettings.sanitizeRenderScale(120))
        assertEquals(100, XrContainerSettings.sanitizeRenderScale(75))
        assertEquals(100, XrContainerSettings.sanitizeRenderScale(null))
    }

    @Test
    fun `temporary configuration merge preserves saved VR settings`() {
        val saved = ContainerData(
            xrRenderScale = 60,
            xrOpenCompositeEnabled = false,
            xrTheaterScreenEnabled = false,
            xrClockEnabled = false,
        )
        val temporaryCandidate = ContainerData(
            name = "temporary launch config",
            xrRenderScale = 150,
            xrOpenCompositeEnabled = true,
            xrTheaterScreenEnabled = true,
            xrClockEnabled = true,
        )

        val merged = XrContainerSettings.preserveFrom(saved, temporaryCandidate)

        assertEquals("temporary launch config", merged.name)
        assertEquals(60, merged.xrRenderScale)
        assertEquals(false, merged.xrOpenCompositeEnabled)
        assertEquals(false, merged.xrTheaterScreenEnabled)
        assertEquals(false, merged.xrClockEnabled)
    }

    @Test
    fun `container writer round trips every VR setting`() {
        val root = File.createTempFile("xr_settings_", null).also {
            it.delete()
            it.mkdirs()
        }
        val container = Container("STEAM_620980").apply { setRootDir(root) }
        val requested = XrContainerSettings.Values(
            renderScale = 60,
            openCompositeEnabled = false,
            theaterScreenEnabled = false,
            clockEnabled = false,
        )

        XrContainerSettings.write(container, requested)
        container.saveData()
        val reloaded = Container("STEAM_620980").apply {
            setRootDir(root)
            loadData(JSONObject(File(root, ".container").readText()))
        }

        assertEquals(requested, XrContainerSettings.read(reloaded))
        root.deleteRecursively()
    }

    @Test
    fun `durable settings survive a later container rewrite`() {
        val container = Container("STEAM_620980")
        val requested = XrContainerSettings.Values(
            renderScale = 60,
            openCompositeEnabled = false,
            theaterScreenEnabled = false,
            clockEnabled = false,
        )
        assertEquals(true, XrContainerSettings.persist(context, container.id, requested))

        // Simulate unrelated launch/config code rewriting the container extras with defaults.
        XrContainerSettings.write(container, XrContainerSettings.Values())

        assertEquals(requested, XrContainerSettings.read(context, container.id, container))
        assertEquals(XrContainerSettings.Values(), XrContainerSettings.read(container))
    }

    @Test
    fun `durable settings are isolated per container id`() {
        val beatSaber = Container("STEAM_620980")
        val otherGame = Container("STEAM_1234")
        val beatSaberSettings = XrContainerSettings.Values(renderScale = 70, clockEnabled = false)
        val otherSettings = XrContainerSettings.Values(renderScale = 120, theaterScreenEnabled = false)

        XrContainerSettings.persist(context, beatSaber.id, beatSaberSettings)
        XrContainerSettings.persist(context, otherGame.id, otherSettings)

        assertEquals(beatSaberSettings, XrContainerSettings.read(context, beatSaber.id, beatSaber))
        assertEquals(otherSettings, XrContainerSettings.read(context, otherGame.id, otherGame))
    }
}
