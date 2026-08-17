package app.gamenative.xr

import android.content.Context
import androidx.test.core.app.ApplicationProvider
import app.gamenative.utils.ContainerUtils
import com.winlator.container.Container
import com.winlator.fexcore.FEXCorePreset
import com.winlator.xenvironment.ImageFs
import io.mockk.every
import io.mockk.mockkObject
import io.mockk.unmockkObject
import io.mockk.verify
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.json.JSONObject
import java.io.File

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class XrLaunchPreferencesPersistenceTest {
    private lateinit var context: Context
    private lateinit var rootDir: File
    private val appId = "STEAM_XR_STALE_SNAPSHOT_TEST"

    @Before
    fun setUp() {
        context = ApplicationProvider.getApplicationContext()
        rootDir = File(ImageFs.find(context).rootDir, "home/xuser-$appId")
        rootDir.deleteRecursively()
        rootDir.mkdirs()
    }

    @After
    fun tearDown() {
        unmockkObject(ContainerUtils)
        rootDir.deleteRecursively()
    }

    private fun loadCurrentContainer(): Container = Container(appId).apply {
        setRootDir(rootDir)
        loadData(JSONObject(File(rootDir, ".container").readText()))
    }

    @Test
    fun launchPreferenceSaves_reloadContainerAndPreserveNewerEmulationSettings() {
        // This is the screen-lifetime snapshot that existed before the container editor saved.
        val stale = Container(appId).apply {
            setRootDir(rootDir)
            graphicsDriver = "Wrapper"
            fexCoreVersion = "2512"
            setFEXCorePreset(FEXCorePreset.PERFORMANCE)
            // Bare Container defaults contain NUL controller bytes; real persisted containers
            // use an empty or populated mapping string.
            setControllerMapping("")
            saveData()
        }

        // Simulate the editor saving through a separately loaded Container instance.
        loadCurrentContainer().apply {
            graphicsDriver = "wrapper-gamenative"
            fexCoreVersion = "2505"
            setFEXCorePreset(FEXCorePreset.INTERMEDIATE)
            saveData()
        }

        mockkObject(ContainerUtils)
        every { ContainerUtils.getContainer(context, appId) } answers { loadCurrentContainer() }

        // The old implementation accepted `stale` and serialized its entire old snapshot here.
        XrLaunchPreferences.save(
            context = context,
            appId = stale.id,
            mode = XrLaunchPreferences.MODE_VR,
            steamLaunchIndex = 1,
            customArgs = "-openxr",
            promptEveryLaunch = true,
        )
        XrRuntimeManager.setOpenCompositeEnabled(context, stale.id, enabled = false)

        verify(exactly = 2) { ContainerUtils.getContainer(context, appId) }
        val reloaded = loadCurrentContainer()
        assertEquals("wrapper-gamenative", reloaded.graphicsDriver)
        assertEquals("2505", reloaded.fexCoreVersion)
        assertEquals(FEXCorePreset.INTERMEDIATE, reloaded.fexCorePreset)
        assertEquals(XrLaunchPreferences.MODE_VR, XrLaunchPreferences.mode(reloaded))
        assertEquals(1, XrLaunchPreferences.selectedSteamLaunchIndex(reloaded))
        assertEquals("-openxr", XrLaunchPreferences.customArgs(reloaded))
    }
}
