#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan.h>
#include "../gamenative_openxr_unix.h"

#include <stdio.h>
#include <string.h>

typedef int (__cdecl *GnWineUnixCall)(unsigned int code, void *args);

static int fail(const char *message, int value)
{
    fprintf(stderr, "%s: %d\n", message, value);
    return value ? value : 1;
}

int main(void)
{
    HMODULE helper = LoadLibraryA("gamenative_xr_unixbridge.dll");
    if (!helper) return fail("LoadLibrary", (int)GetLastError());
    FARPROC raw_unix_call =
        GetProcAddress(helper, "gnWineUnixCall");
    GnWineUnixCall unix_call = NULL;
    memcpy(&unix_call, &raw_unix_call, sizeof(unix_call));
    if (!unix_call) return fail("GetProcAddress", (int)GetLastError());

    struct gn_unix_init_args init = {
        GN_UNIX_ABI_VERSION, GN_UNIX_ERROR_UNAVAILABLE
    };
    if (unix_call(GN_UNIX_INIT, &init) || init.result != GN_UNIX_SUCCESS)
        return fail("GN_UNIX_INIT", init.result);

    VkApplicationInfo application = {
        VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "GameNative unixlib smoke", 1,
        "GameNative", 1, VK_API_VERSION_1_1
    };
    VkInstanceCreateInfo instance_info = {
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &application,
        0, NULL, 0, NULL
    };
    VkInstance instance = VK_NULL_HANDLE;
    VkResult vk_result = vkCreateInstance(&instance_info, NULL, &instance);
    if (vk_result != VK_SUCCESS) return fail("vkCreateInstance", vk_result);

    uint32_t physical_count = 1;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    vk_result = vkEnumeratePhysicalDevices(
        instance, &physical_count, &physical_device);
    if (vk_result != VK_SUCCESS || !physical_count)
        return fail("vkEnumeratePhysicalDevices", vk_result);

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(
        physical_device, &family_count, NULL);
    VkQueueFamilyProperties families[32];
    if (!family_count || family_count > 32) return fail("queue families", 10);
    vkGetPhysicalDeviceQueueFamilyProperties(
        physical_device, &family_count, families);
    uint32_t family = family_count;
    for (uint32_t i = 0; i < family_count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            family = i;
            break;
        }
    }
    if (family == family_count) return fail("graphics queue", 11);

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0,
        family, 1, &priority
    };
    VkDeviceCreateInfo device_info = {
        VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &queue_info,
        0, NULL, 0, NULL, NULL
    };
    VkDevice device = VK_NULL_HANDLE;
    vk_result = vkCreateDevice(
        physical_device, &device_info, NULL, &device);
    if (vk_result != VK_SUCCESS) return fail("vkCreateDevice", vk_result);
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, family, 0, &queue);

    struct gn_unix_vulkan_context_args context;
    memset(&context, 0, sizeof(context));
    context.client_physical_device = (gn_u64)(uintptr_t)physical_device;
    context.client_device = (gn_u64)(uintptr_t)device;
    context.client_queue = (gn_u64)(uintptr_t)queue;
    context.queue_family_index = family;
    context.handles_are_host = 0;
    context.result = GN_UNIX_ERROR_UNAVAILABLE;
    if (unix_call(GN_UNIX_SET_VULKAN_CONTEXT, &context) ||
        context.result != GN_UNIX_SUCCESS)
        return fail("GN_UNIX_SET_VULKAN_CONTEXT", context.result);

    struct gn_unix_create_swapchain_args create;
    memset(&create, 0, sizeof(create));
    create.width = 64;
    create.height = 64;
    create.array_size = 2;
    create.mip_count = 1;
    create.sample_count = 1;
    create.format = VK_FORMAT_R8G8B8A8_UNORM;
    create.result = GN_UNIX_ERROR_UNAVAILABLE;
    if (unix_call(GN_UNIX_CREATE_SWAPCHAIN, &create) ||
        create.result != GN_UNIX_SUCCESS || create.image_count < 2) {
        int result = create.result;
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        FreeLibrary(helper);
        if (result == GN_UNIX_ERROR_VULKAN) {
            fprintf(stderr,
                    "Wine Vulkan host interop passed; driver cannot allocate "
                    "an exportable dma-buf image (skip)\n");
            return 77;
        }
        return fail("GN_UNIX_CREATE_SWAPCHAIN", result);
    }

    struct gn_unix_destroy_swapchain_args destroy = {
        0, GN_UNIX_ERROR_UNAVAILABLE
    };
    if (unix_call(GN_UNIX_DESTROY_SWAPCHAIN, &destroy) ||
        destroy.result != GN_UNIX_SUCCESS)
        return fail("GN_UNIX_DESTROY_SWAPCHAIN", destroy.result);

    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    FreeLibrary(helper);
    puts("Wine Vulkan unixlib swapchain smoke passed");
    return 0;
}
