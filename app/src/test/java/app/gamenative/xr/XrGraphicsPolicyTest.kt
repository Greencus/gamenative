package app.gamenative.xr

import org.junit.Assert.assertEquals
import org.junit.Test

class XrGraphicsPolicyTest {
    @Test
    fun `VR replaces WineD3D with DXVK`() {
        assertEquals(
            "dxvk",
            XrGraphicsPolicy.effectiveDxWrapper("wined3d", xrActive = true),
        )
    }

    @Test
    fun `VR preserves compatible DXVK and VKD3D selections`() {
        assertEquals(
            "dxvk-2.6.1-gplasync",
            XrGraphicsPolicy.effectiveDxWrapper("dxvk-2.6.1-gplasync", xrActive = true),
        )
        assertEquals(
            "vkd3d-2.14.1",
            XrGraphicsPolicy.effectiveDxWrapper("vkd3d-2.14.1", xrActive = true),
        )
    }

    @Test
    fun `VR replaces non-Vulkan graphics driver`() {
        assertEquals(
            "vortek",
            XrGraphicsPolicy.effectiveGraphicsDriver("virgl", xrActive = true),
        )
    }

    @Test
    fun `flat launch preserves requested graphics configuration`() {
        assertEquals(
            "wined3d",
            XrGraphicsPolicy.effectiveDxWrapper("wined3d", xrActive = false),
        )
        assertEquals(
            "virgl",
            XrGraphicsPolicy.effectiveGraphicsDriver("virgl", xrActive = false),
        )
    }
}
