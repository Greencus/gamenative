package app.gamenative.xr

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class XrRuntimeManagerTest {
    @Test
    fun `runtime manifest is valid JSON with an escaped Windows library path`() {
        val manifest = XrRuntimeManager.runtimeManifest("gamenative_openxr64.dll")
        val parsed = Json.parseToJsonElement(manifest).jsonObject

        assertEquals("1.0.0", parsed.getValue("file_format_version").jsonPrimitive.content)
        assertEquals(
            "C:\\gamenative\\xr\\gamenative_openxr64.dll",
            parsed.getValue("runtime")
                .jsonObject
                .getValue("library_path")
                .jsonPrimitive
                .content,
        )
        assertTrue(manifest.contains("C:\\\\gamenative\\\\xr\\\\gamenative_openxr64.dll"))
    }

    @Test
    fun `32-bit manifest points only to the 32-bit runtime`() {
        val runtime = Json.parseToJsonElement(
            XrRuntimeManager.runtimeManifest("gamenative_openxr32.dll"),
        ).jsonObject.getValue("runtime").jsonObject

        assertEquals(
            "C:\\gamenative\\xr\\gamenative_openxr32.dll",
            runtime.getValue("library_path").jsonPrimitive.content,
        )
    }

    @Test
    fun `common manifest uses the WOW64 redirected system runtime path`() {
        val runtime = Json.parseToJsonElement(
            XrRuntimeManager.runtimeManifest(
                "C:\\windows\\system32\\gamenative_openxr.dll",
            ),
        ).jsonObject.getValue("runtime").jsonObject

        assertEquals(
            "C:\\windows\\system32\\gamenative_openxr.dll",
            runtime.getValue("library_path").jsonPrimitive.content,
        )
    }

    @Test
    fun `32-bit registry key uses Wine canonical casing`() {
        assertEquals(
            "Software\\Wow6432Node\\Khronos\\OpenXR\\1",
            XrRuntimeManager.OPENXR_REGISTRY_KEY_32,
        )
    }

    @Test
    fun `XR unix bridge is forced to Wine builtin mode`() {
        assertEquals(
            "gameoverlayrenderer=n;gameoverlayrenderer64=n;gamenative_xr_unixbridge=b",
            XrRuntimeManager.runtimeDllOverrides(""),
        )
    }

    @Test
    fun `XR unix bridge override preserves game-specific Wine overrides`() {
        assertEquals(
            "mf=n;mfmediaengine=n;gameoverlayrenderer=n;gameoverlayrenderer64=n;gamenative_xr_unixbridge=b",
            XrRuntimeManager.runtimeDllOverrides("mf=n;mfmediaengine=n;"),
        )
    }
}
