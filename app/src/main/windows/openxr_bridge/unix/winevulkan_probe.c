#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int has_extension(const VkExtensionProperties *properties, uint32_t count,
                         const char *name)
{
    for (uint32_t i = 0; i < count; ++i)
        if (!strcmp(properties[i].extensionName, name)) return 1;
    return 0;
}

int main(void)
{
    FILE *output = fopen("C:\\gamenative-winevulkan-probe.txt", "w");
    if (!output) output = stdout;
    VkApplicationInfo application = {
        VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "GameNative probe", 1,
        "GameNative", 1, VK_API_VERSION_1_1
    };
    VkInstanceCreateInfo instance_info = {
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &application,
        0, NULL, 0, NULL
    };
    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&instance_info, NULL, &instance);
    if (result != VK_SUCCESS) {
        fprintf(output, "vkCreateInstance failed: %d\n", result);
        if (output != stdout) fclose(output);
        return 2;
    }

    uint32_t physical_count = 0;
    result = vkEnumeratePhysicalDevices(instance, &physical_count, NULL);
    if (result != VK_SUCCESS || !physical_count) {
        fprintf(output, "vkEnumeratePhysicalDevices failed: %d count=%u\n",
                result, physical_count);
        vkDestroyInstance(instance, NULL);
        if (output != stdout) fclose(output);
        return 3;
    }
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    physical_count = 1;
    vkEnumeratePhysicalDevices(instance, &physical_count, &physical_device);

    uint32_t extension_count = 0;
    vkEnumerateDeviceExtensionProperties(
        physical_device, NULL, &extension_count, NULL);
    VkExtensionProperties *extensions =
        calloc(extension_count, sizeof(*extensions));
    if (!extensions) return 4;
    vkEnumerateDeviceExtensionProperties(
        physical_device, NULL, &extension_count, extensions);

    static const char *required[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
        "VK_KHR_external_memory_win32",
        "VK_KHR_external_fence_win32"
    };
    for (uint32_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i)
        fprintf(output, "%s=%d\n", required[i],
                has_extension(extensions, extension_count, required[i]));

    free(extensions);
    vkDestroyInstance(instance, NULL);
    if (output != stdout) fclose(output);
    return 0;
}
