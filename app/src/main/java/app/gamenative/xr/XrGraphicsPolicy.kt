package app.gamenative.xr

object XrGraphicsPolicy {
    private const val XR_DX_WRAPPER = "dxvk"
    private const val XR_GRAPHICS_DRIVER = "vortek"

    fun effectiveDxWrapper(requested: String, xrActive: Boolean): String {
        if (!xrActive) return requested
        val family = requested.substringBefore('-').lowercase()
        return if (family == "dxvk" || family == "vkd3d") requested else XR_DX_WRAPPER
    }

    fun effectiveGraphicsDriver(requested: String, xrActive: Boolean): String {
        if (!xrActive) return requested
        return if (requested.lowercase() in NON_VULKAN_GRAPHICS_DRIVERS) {
            XR_GRAPHICS_DRIVER
        } else {
            requested
        }
    }

    private val NON_VULKAN_GRAPHICS_DRIVERS = setOf(
        "virgl",
        "llvmpipe",
    )
}
