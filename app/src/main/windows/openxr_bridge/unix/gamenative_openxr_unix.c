#define _GNU_SOURCE
#include "../gamenative_openxr_unix.h"

#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define GN_FOURCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
     ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define DRM_FORMAT_ABGR8888 GN_FOURCC('A', 'B', '2', '4')
#define DRM_FORMAT_ARGB8888 GN_FOURCC('A', 'R', '2', '4')

typedef int32_t (*unixlib_entry_t)(void *);

struct wine_client_object {
    uint64_t loader_magic;
    uint64_t unix_handle;
};

struct gn_image {
    VkImage image;
    VkDeviceMemory memory;
    int dma_buf_fd;
    uint32_t plane_count;
    uint32_t strides[4];
    uint32_t offsets[4];
    uint64_t modifier;
    uint8_t registered_eye_mask;
    uint32_t registered_array_index[2];
    uint8_t submitted;
};

struct gn_swapchain {
    uint8_t allocated;
    uint32_t width;
    uint32_t height;
    VkFormat format;
    uint32_t array_size;
    uint32_t image_count;
    struct gn_image images[GN_UNIX_MAX_IMAGES];
};

static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t socket_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct gn_swapchain swapchains[GN_UNIX_MAX_SWAPCHAINS];
static VkPhysicalDevice physical_device;
static VkDevice device;
static VkQueue queue;
static void *vulkan_so;
static int transport_fd = -1;

static PFN_vkGetDeviceProcAddr p_vkGetDeviceProcAddr;
static PFN_vkGetPhysicalDeviceMemoryProperties p_vkGetPhysicalDeviceMemoryProperties;
static PFN_vkGetPhysicalDeviceFormatProperties2 p_vkGetPhysicalDeviceFormatProperties2;
static PFN_vkCreateImage p_vkCreateImage;
static PFN_vkDestroyImage p_vkDestroyImage;
static PFN_vkGetImageMemoryRequirements p_vkGetImageMemoryRequirements;
static PFN_vkAllocateMemory p_vkAllocateMemory;
static PFN_vkFreeMemory p_vkFreeMemory;
static PFN_vkBindImageMemory p_vkBindImageMemory;
static PFN_vkGetImageSubresourceLayout p_vkGetImageSubresourceLayout;
static PFN_vkGetImageDrmFormatModifierPropertiesEXT p_vkGetImageDrmFormatModifierPropertiesEXT;
static PFN_vkGetMemoryFdKHR p_vkGetMemoryFdKHR;
static PFN_vkCreateFence p_vkCreateFence;
static PFN_vkDestroyFence p_vkDestroyFence;
static PFN_vkGetFenceFdKHR p_vkGetFenceFdKHR;
static PFN_vkQueueSubmit p_vkQueueSubmit;
static PFN_vkQueueWaitIdle p_vkQueueWaitIdle;

static void log_line(const char *line)
{
    FILE *f = fopen("/tmp/gamenative-xr-unix.log", "a");
    if (!f) return;
    fprintf(f, "%s\n", line);
    fclose(f);
}

static void log_vulkan_context_state(void)
{
    char line[384];
    snprintf(line, sizeof(line),
             "Vulkan context phys=%p device=%p queue=%p gpa=%p createImage=%p "
             "getMemoryFd=%p createFence=%p getFenceFd=%p queueSubmit=%p",
             (void *)physical_device, (void *)device, (void *)queue,
             (void *)p_vkGetDeviceProcAddr, (void *)p_vkCreateImage,
             (void *)p_vkGetMemoryFdKHR, (void *)p_vkCreateFence,
             (void *)p_vkGetFenceFdKHR, (void *)p_vkQueueSubmit);
    log_line(line);
}

static uintptr_t readable_mapping_end(uintptr_t address)
{
    FILE *maps = fopen("/proc/self/maps", "r");
    char line[256];
    if (!maps) return address;
    while (fgets(line, sizeof(line), maps)) {
        unsigned long long start, end;
        char permissions[5] = {0};
        if (sscanf(line, "%llx-%llx %4s", &start, &end, permissions) == 3 &&
            address >= (uintptr_t)start && address < (uintptr_t)end &&
            permissions[0] == 'r') {
            fclose(maps);
            return (uintptr_t)end;
        }
    }
    fclose(maps);
    return address;
}

static uint64_t unwrap_dispatchable(uint64_t client_handle)
{
    char trace[224];
    const struct wine_client_object *client =
        (const struct wine_client_object *)(uintptr_t)client_handle;
    if (!client || !client->unix_handle) {
        snprintf(trace, sizeof(trace), "unwrap client=0x%llx has no unix object",
                 (unsigned long long)client_handle);
        log_line(trace);
        return 0;
    }
    const uint64_t *object =
        (const uint64_t *)(uintptr_t)client->unix_handle;
    if (object[1] == client->unix_handle) {
        snprintf(trace, sizeof(trace),
                 "unwrap client=0x%llx object=0x%llx Wine10 host=0x%llx",
                 (unsigned long long)client_handle,
                 (unsigned long long)client->unix_handle,
                 (unsigned long long)object[0]);
        log_line(trace);
        return object[0];
    }

    /*
     * Wine 10+ starts dispatchable Unix objects with VULKAN_OBJECT_HEADER
     * (host handle, client handle). Wine 9.x instead stores the client handle
     * immediately before the host handle; VkDevice places both after its
     * generated function table. Locate that adjacent pair inside the readable
     * heap mapping so the same unixlib works with GameNative's Wine 9.2 and
     * newer user-installed Wine builds.
     */
    uintptr_t end = readable_mapping_end((uintptr_t)object);
    size_t words = end > (uintptr_t)object ?
        (end - (uintptr_t)object) / sizeof(*object) : 0;
    if (words > 8192) words = 8192;
    for (size_t i = 0; i + 1 < words; ++i) {
        if (object[i] == client_handle) {
            snprintf(trace, sizeof(trace),
                     "unwrap client=0x%llx object=0x%llx Wine9 index=%zu host=0x%llx",
                     (unsigned long long)client_handle,
                     (unsigned long long)client->unix_handle, i,
                     (unsigned long long)object[i + 1]);
            log_line(trace);
            return object[i + 1];
        }
    }
    /*
     * Wine 9.2's Vulkan loader exposes a loader-owned VkDevice instead of the
     * ICD VkDevice stored in wine_device.handle. Its generated device function
     * table is 4088 bytes, followed by phys_dev, handle and host_device. The
     * client handle therefore cannot be found by value, but unix_handle still
     * points at the right wine_device object. This exact fallback is paired
     * with the version-pinned Wine 9.2 module shipped by GameNative.
     */
    const size_t wine_92_device_host_index = (4088 / sizeof(*object)) + 1;
    if (words > wine_92_device_host_index &&
        object[wine_92_device_host_index - 2] &&
        object[wine_92_device_host_index]) {
        snprintf(trace, sizeof(trace),
                 "unwrap client=0x%llx object=0x%llx Wine9.2 device host=0x%llx",
                 (unsigned long long)client_handle,
                 (unsigned long long)client->unix_handle,
                 (unsigned long long)object[wine_92_device_host_index]);
        log_line(trace);
        return object[wine_92_device_host_index];
    }
    snprintf(trace, sizeof(trace),
             "unwrap client=0x%llx object=0x%llx no match words=%zu "
             "w510=0x%llx w511=0x%llx w512=0x%llx w513=0x%llx",
             (unsigned long long)client_handle,
             (unsigned long long)client->unix_handle, words,
             (unsigned long long)(words > 510 ? object[510] : 0),
             (unsigned long long)(words > 511 ? object[511] : 0),
             (unsigned long long)(words > 512 ? object[512] : 0),
             (unsigned long long)(words > 513 ? object[513] : 0));
    log_line(trace);
    return words ? object[0] : 0;
}

static int write_all(int fd, const void *data, size_t len)
{
    const char *p = data;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        p += n;
        len -= (size_t)n;
    }
    return 1;
}

static int read_line(int fd, char *line, size_t capacity)
{
    size_t used = 0;
    if (!capacity) return 0;
    while (used + 1 < capacity) {
        char ch;
        ssize_t n = recv(fd, &ch, 1, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        if (ch == '\n') {
            line[used] = 0;
            return 1;
        }
        line[used++] = ch;
    }
    line[capacity - 1] = 0;
    return 0;
}

static int send_fd(int socket_fd, int fd)
{
    char payload = 'F';
    struct iovec iov = {&payload, 1};
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
    ssize_t n;
    do n = sendmsg(socket_fd, &msg, MSG_NOSIGNAL); while (n < 0 && errno == EINTR);
    return n == 1;
}

static int recv_fd(int socket_fd)
{
    char payload;
    struct iovec iov = {&payload, 1};
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    ssize_t n;
    do n = recvmsg(socket_fd, &msg, 0); while (n < 0 && errno == EINTR);
    if (n <= 0) return -1;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
            cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
            int fd;
            memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
            return fd;
        }
    }
    return -1;
}

static void close_transport(void)
{
    if (transport_fd >= 0) close(transport_fd);
    transport_fd = -1;
}

static int ensure_transport(void)
{
    if (transport_fd >= 0) return 1;
    const char *path = getenv("GAMENATIVE_XR_SOCKET");
    if (!path || !*path) path = "@gamenative-xr";

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 0;
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    socklen_t length;
    if (path[0] == '@') {
        size_t size = strlen(path + 1);
        if (size > sizeof(address.sun_path) - 2) size = sizeof(address.sun_path) - 2;
        address.sun_path[0] = 0;
        memcpy(address.sun_path + 1, path + 1, size);
        length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + size);
    } else {
        strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);
        length = sizeof(address);
    }
    if (connect(fd, (struct sockaddr *)&address, length) < 0) {
        close(fd);
        return 0;
    }
    if (!write_all(fd, "HELLO producer=wine-unixlib version=2\n", 38)) {
        close(fd);
        return 0;
    }
    char response[64];
    if (!read_line(fd, response, sizeof(response)) || strncmp(response, "OK", 2)) {
        close(fd);
        return 0;
    }
    transport_fd = fd;
    return 1;
}

static int transact_line(const char *line, char *response, size_t response_size)
{
    if (!ensure_transport() || !write_all(transport_fd, line, strlen(line)) ||
        !read_line(transport_fd, response, response_size)) {
        close_transport();
        return 0;
    }
    return 1;
}

static void load_vulkan_functions(void)
{
    if (!vulkan_so) vulkan_so = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!vulkan_so) return;
    p_vkGetDeviceProcAddr = dlsym(vulkan_so, "vkGetDeviceProcAddr");
    p_vkGetPhysicalDeviceMemoryProperties =
        dlsym(vulkan_so, "vkGetPhysicalDeviceMemoryProperties");
    p_vkGetPhysicalDeviceFormatProperties2 =
        dlsym(vulkan_so, "vkGetPhysicalDeviceFormatProperties2");
    if (!p_vkGetDeviceProcAddr || !device) return;
#define LOAD_DEVICE(name) p_##name = (PFN_##name)p_vkGetDeviceProcAddr(device, #name)
    LOAD_DEVICE(vkCreateImage);
    LOAD_DEVICE(vkDestroyImage);
    LOAD_DEVICE(vkGetImageMemoryRequirements);
    LOAD_DEVICE(vkAllocateMemory);
    LOAD_DEVICE(vkFreeMemory);
    LOAD_DEVICE(vkBindImageMemory);
    LOAD_DEVICE(vkGetImageSubresourceLayout);
    LOAD_DEVICE(vkGetImageDrmFormatModifierPropertiesEXT);
    LOAD_DEVICE(vkGetMemoryFdKHR);
    LOAD_DEVICE(vkCreateFence);
    LOAD_DEVICE(vkDestroyFence);
    LOAD_DEVICE(vkGetFenceFdKHR);
    LOAD_DEVICE(vkQueueSubmit);
      LOAD_DEVICE(vkQueueWaitIdle);
#undef LOAD_DEVICE
    /*
     * winevulkan intentionally hides Linux handle extensions from the Windows
     * extension list, so vkGetDeviceProcAddr returns NULL for their entry
     * points even when the host driver has them. The Vulkan loader's exported
     * trampolines still dispatch against the host VkDevice; use those for the
     * Unix-only transport calls. This keeps Linux extension names out of the
     * Windows application's VkDeviceCreateInfo.
     */
#define LOAD_HOST_TRAMPOLINE(name) \
    if (!p_##name) p_##name = (PFN_##name)dlsym(vulkan_so, #name)
    LOAD_HOST_TRAMPOLINE(vkGetImageDrmFormatModifierPropertiesEXT);
    LOAD_HOST_TRAMPOLINE(vkGetMemoryFdKHR);
    LOAD_HOST_TRAMPOLINE(vkGetFenceFdKHR);
#undef LOAD_HOST_TRAMPOLINE
}

static int find_memory_type(uint32_t bits, VkMemoryPropertyFlags desired)
{
    VkPhysicalDeviceMemoryProperties props;
    p_vkGetPhysicalDeviceMemoryProperties(physical_device, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & desired) == desired) return (int)i;
    }
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if (bits & (1u << i)) return (int)i;
    return -1;
}

static uint32_t format_to_fourcc(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        return DRM_FORMAT_ABGR8888;
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return DRM_FORMAT_ARGB8888;
    default:
        return 0;
    }
}

static void log_vk_result(const char *operation, VkResult result)
{
    char line[160];
    snprintf(line, sizeof(line), "%s failed with VkResult %d", operation, result);
    log_line(line);
}

static int create_image(struct gn_swapchain *swapchain, struct gn_image *out,
                        uint32_t array_size, uint32_t mip_count, uint32_t sample_count)
{
    VkExternalMemoryImageCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
    };
    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = swapchain->format,
        .extent = {swapchain->width, swapchain->height, 1},
        .mipLevels = mip_count ? mip_count : 1,
        .arrayLayers = array_size ? array_size : 1,
        .samples = sample_count == 2 ? VK_SAMPLE_COUNT_2_BIT :
                   sample_count == 4 ? VK_SAMPLE_COUNT_4_BIT :
                   sample_count == 8 ? VK_SAMPLE_COUNT_8_BIT : VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };

    VkDrmFormatModifierPropertiesListEXT modifier_list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT
    };
    VkFormatProperties2 format_props = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &modifier_list
    };
    VkDrmFormatModifierPropertiesEXT *modifiers = NULL;
    VkImageDrmFormatModifierListCreateInfoEXT modifier_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT
    };
    uint64_t chosen_modifier = 0;
    uint32_t chosen_plane_count = 1;
    int has_chosen_modifier = 0;
    if (p_vkGetPhysicalDeviceFormatProperties2) {
        p_vkGetPhysicalDeviceFormatProperties2(physical_device, swapchain->format, &format_props);
        if (modifier_list.drmFormatModifierCount) {
            modifiers = calloc(modifier_list.drmFormatModifierCount, sizeof(*modifiers));
            modifier_list.pDrmFormatModifierProperties = modifiers;
            p_vkGetPhysicalDeviceFormatProperties2(physical_device, swapchain->format, &format_props);
            for (uint32_t i = 0; i < modifier_list.drmFormatModifierCount; ++i) {
                VkFormatFeatureFlags features = modifiers[i].drmFormatModifierTilingFeatures;
                if ((features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) &&
                    (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
                    modifiers[i].drmFormatModifierPlaneCount >= 1 &&
                    modifiers[i].drmFormatModifierPlaneCount <= 4) {
                    chosen_modifier = modifiers[i].drmFormatModifier;
                    chosen_plane_count =
                        modifiers[i].drmFormatModifierPlaneCount;
                    has_chosen_modifier = 1;
                    break;
                }
            }
        }
    }
    if (has_chosen_modifier) {
        modifier_info.drmFormatModifierCount = 1;
        modifier_info.pDrmFormatModifiers = &chosen_modifier;
        external.pNext = &modifier_info;
        info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    }

    VkResult result = p_vkCreateImage(device, &info, NULL, &out->image);
    free(modifiers);
    if (result != VK_SUCCESS && has_chosen_modifier) {
        chosen_modifier = 0;
        chosen_plane_count = 1;
        has_chosen_modifier = 0;
        external.pNext = NULL;
        info.tiling = VK_IMAGE_TILING_LINEAR;
        result = p_vkCreateImage(device, &info, NULL, &out->image);
    }
    if (result != VK_SUCCESS) {
        log_vk_result("vkCreateImage(dma-buf)", result);
        goto fail;
    }

    VkMemoryRequirements requirements;
    p_vkGetImageMemoryRequirements(device, out->image, &requirements);
    int memory_type = find_memory_type(requirements.memoryTypeBits, 0);
    if (memory_type < 0) {
        log_line("No compatible memory type for dma-buf image");
        goto fail;
    }

    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = out->image
    };
    VkExportMemoryAllocateInfo export_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
    };
    VkMemoryAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &export_info,
        .allocationSize = requirements.size,
        .memoryTypeIndex = (uint32_t)memory_type
    };
    result = p_vkAllocateMemory(device, &allocate_info, NULL, &out->memory);
    if (result != VK_SUCCESS) {
        log_vk_result("vkAllocateMemory(dma-buf)", result);
        goto fail;
    }
    result = p_vkBindImageMemory(device, out->image, out->memory, 0);
    if (result != VK_SUCCESS) {
        log_vk_result("vkBindImageMemory(dma-buf)", result);
        goto fail;
    }

    VkMemoryGetFdInfoKHR fd_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = out->memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
    };
    if (!p_vkGetMemoryFdKHR) {
        log_line("vkGetMemoryFdKHR is unavailable");
        goto fail;
    }
    result = p_vkGetMemoryFdKHR(device, &fd_info, &out->dma_buf_fd);
    if (result != VK_SUCCESS) {
        log_vk_result("vkGetMemoryFdKHR(dma-buf)", result);
        goto fail;
    }

    out->plane_count = chosen_plane_count;
    for (uint32_t plane = 0; plane < out->plane_count; ++plane) {
        VkImageSubresource subresource = {
            .aspectMask = out->plane_count == 1
                ? VK_IMAGE_ASPECT_COLOR_BIT
                : (VkImageAspectFlags)
                    (VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << plane),
            .mipLevel = 0,
            .arrayLayer = 0
        };
        VkSubresourceLayout layout;
        p_vkGetImageSubresourceLayout(
            device, out->image, &subresource, &layout);
        out->strides[plane] = (uint32_t)layout.rowPitch;
        out->offsets[plane] = (uint32_t)layout.offset;
    }
    out->modifier = has_chosen_modifier ? chosen_modifier : 0;
    if (has_chosen_modifier && p_vkGetImageDrmFormatModifierPropertiesEXT) {
        VkImageDrmFormatModifierPropertiesEXT props = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT
        };
        if (p_vkGetImageDrmFormatModifierPropertiesEXT(device, out->image, &props) == VK_SUCCESS)
            out->modifier = props.drmFormatModifier;
    }
    return 1;

fail:
    if (out->dma_buf_fd >= 0) close(out->dma_buf_fd);
    out->dma_buf_fd = -1;
    if (out->memory && p_vkFreeMemory) p_vkFreeMemory(device, out->memory, NULL);
    out->memory = VK_NULL_HANDLE;
    if (out->image && p_vkDestroyImage) p_vkDestroyImage(device, out->image, NULL);
    out->image = VK_NULL_HANDLE;
    return 0;
}

static void destroy_image(struct gn_image *image)
{
    if (image->dma_buf_fd >= 0) close(image->dma_buf_fd);
    if (image->image && p_vkDestroyImage) p_vkDestroyImage(device, image->image, NULL);
    if (image->memory && p_vkFreeMemory) p_vkFreeMemory(device, image->memory, NULL);
    memset(image, 0, sizeof(*image));
    image->dma_buf_fd = -1;
}

static int register_image(uint32_t slot, uint32_t image_index, uint32_t eye,
                          uint32_t array_index)
{
    struct gn_swapchain *swapchain = &swapchains[slot];
    struct gn_image *image = &swapchain->images[image_index];
    const uint8_t bit = (uint8_t)(1u << eye);
    if (array_index >= swapchain->array_size) return 0;
    if ((image->registered_eye_mask & bit) &&
        image->registered_array_index[eye] == array_index) return 1;

    VkSubresourceLayout layouts[4];
    for (uint32_t plane = 0; plane < image->plane_count; ++plane) {
        VkImageSubresource subresource = {
            .aspectMask = image->plane_count == 1
                ? VK_IMAGE_ASPECT_COLOR_BIT
                : (VkImageAspectFlags)
                    (VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << plane),
            .mipLevel = 0,
            .arrayLayer = array_index
        };
        p_vkGetImageSubresourceLayout(
            device, image->image, &subresource, &layouts[plane]);
    }

    char line[512], response[64];
    const uint32_t fourcc = format_to_fourcc(swapchain->format);
    if (!fourcc) return 0;
    const uint32_t transport_index = slot * GN_UNIX_MAX_IMAGES + image_index;
    int length = snprintf(
        line, sizeof(line),
        "DMABUF eye=%u index=%u w=%u h=%u planes=%u fourcc=0x%08x "
        "modifier=0x%llx",
        eye, transport_index, swapchain->width, swapchain->height,
        image->plane_count, fourcc, (unsigned long long)image->modifier);
    for (uint32_t plane = 0;
         plane < image->plane_count && length > 0 &&
         (size_t)length < sizeof(line); ++plane) {
        length += snprintf(
            line + length, sizeof(line) - (size_t)length,
            " stride%u=%u offset%u=%u",
            plane, (uint32_t)layouts[plane].rowPitch,
            plane, (uint32_t)layouts[plane].offset);
    }
    if (length <= 0 || (size_t)length + 2 > sizeof(line)) return 0;
    line[length++] = '\n';
    line[length] = 0;
    int sent_planes = 1;
    if (!transact_line(line, response, sizeof(response)) ||
        strncmp(response, "OK", 2)) {
        sent_planes = 0;
    }
    for (uint32_t plane = 0;
         sent_planes && plane < image->plane_count; ++plane) {
        if (!send_fd(transport_fd, image->dma_buf_fd)) sent_planes = 0;
    }
    if (!sent_planes ||
        !read_line(transport_fd, response, sizeof(response)) || strncmp(response, "OK", 2)) {
        close_transport();
        return 0;
    }
    image->registered_eye_mask |= bit;
    image->registered_array_index[eye] = array_index;
    return 1;
}

static int make_acquire_fence_fd(void)
{
    if (!p_vkCreateFence || !p_vkQueueSubmit || !p_vkGetFenceFdKHR) {
        if (p_vkQueueWaitIdle) p_vkQueueWaitIdle(queue);
        return -1;
    }
    VkExportFenceCreateInfo export_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT
    };
    VkFenceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = &export_info
    };
    VkFence fence;
    if (p_vkCreateFence(device, &create_info, NULL, &fence) != VK_SUCCESS) {
        if (p_vkQueueWaitIdle) p_vkQueueWaitIdle(queue);
        return -1;
    }
    if (p_vkQueueSubmit(queue, 0, NULL, fence) != VK_SUCCESS) {
        p_vkDestroyFence(device, fence, NULL);
        if (p_vkQueueWaitIdle) p_vkQueueWaitIdle(queue);
        return -1;
    }
    VkFenceGetFdInfoKHR fd_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR,
        .fence = fence,
        .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT
    };
    int fd = -1;
    if (p_vkGetFenceFdKHR(device, &fd_info, &fd) != VK_SUCCESS) fd = -1;
    p_vkDestroyFence(device, fence, NULL);
    if (fd < 0 && p_vkQueueWaitIdle) p_vkQueueWaitIdle(queue);
    return fd;
}

static int32_t unix_init(void *opaque)
{
    struct gn_unix_init_args *args = opaque;
    args->result = args->abi_version == GN_UNIX_ABI_VERSION ?
        GN_UNIX_SUCCESS : GN_UNIX_ERROR_ARGUMENT;
    log_line("unixlib initialized");
    return 0;
}

static int32_t unix_set_vulkan_context(void *opaque)
{
    struct gn_unix_vulkan_context_args *args = opaque;
    pthread_mutex_lock(&state_mutex);
    if (args->handles_are_host) {
        physical_device = (VkPhysicalDevice)(uintptr_t)args->client_physical_device;
        device = (VkDevice)(uintptr_t)args->client_device;
        queue = (VkQueue)(uintptr_t)args->client_queue;
    } else {
        physical_device = (VkPhysicalDevice)(uintptr_t)unwrap_dispatchable(args->client_physical_device);
        device = (VkDevice)(uintptr_t)unwrap_dispatchable(args->client_device);
        queue = (VkQueue)(uintptr_t)unwrap_dispatchable(args->client_queue);
    }
    load_vulkan_functions();
    log_vulkan_context_state();
    args->result = physical_device && device && queue && p_vkCreateImage &&
                   p_vkGetMemoryFdKHR ? GN_UNIX_SUCCESS : GN_UNIX_ERROR_UNAVAILABLE;
    pthread_mutex_unlock(&state_mutex);
    log_line(args->result ? "Vulkan context unavailable" : "Vulkan context ready");
    return 0;
}

static int32_t unix_create_swapchain(void *opaque)
{
    struct gn_unix_create_swapchain_args *args = opaque;
    if (args->slot >= GN_UNIX_MAX_SWAPCHAINS || !args->width || !args->height ||
        !device || !format_to_fourcc((VkFormat)args->format)) {
        args->result = GN_UNIX_ERROR_ARGUMENT;
        return 0;
    }
    pthread_mutex_lock(&state_mutex);
    struct gn_swapchain *swapchain = &swapchains[args->slot];
    if (swapchain->allocated) {
        args->result = GN_UNIX_ERROR_ARGUMENT;
        pthread_mutex_unlock(&state_mutex);
        return 0;
    }
    memset(swapchain, 0, sizeof(*swapchain));
    swapchain->width = args->width;
    swapchain->height = args->height;
    swapchain->format = (VkFormat)args->format;
    swapchain->array_size = args->array_size ? args->array_size : 1;
    swapchain->image_count = 3; /* Triple buffering covers the requested double-buffer minimum. */
    for (uint32_t i = 0; i < swapchain->image_count; ++i) {
        swapchain->images[i].dma_buf_fd = -1;
        if (!create_image(swapchain, &swapchain->images[i], args->array_size,
                          args->mip_count, args->sample_count)) {
            for (uint32_t j = 0; j <= i; ++j) destroy_image(&swapchain->images[j]);
            memset(swapchain, 0, sizeof(*swapchain));
            args->result = GN_UNIX_ERROR_VULKAN;
            pthread_mutex_unlock(&state_mutex);
            return 0;
        }
        args->images[i] = (gn_u64)(uintptr_t)swapchain->images[i].image;
    }
    swapchain->allocated = 1;
    args->image_count = swapchain->image_count;
    args->result = GN_UNIX_SUCCESS;
    pthread_mutex_unlock(&state_mutex);
    log_line("swapchain created");
    return 0;
}

static int32_t unix_destroy_swapchain(void *opaque)
{
    struct gn_unix_destroy_swapchain_args *args = opaque;
    if (args->slot >= GN_UNIX_MAX_SWAPCHAINS) {
        args->result = GN_UNIX_ERROR_ARGUMENT;
        return 0;
    }
    pthread_mutex_lock(&state_mutex);
    struct gn_swapchain *swapchain = &swapchains[args->slot];
    for (uint32_t i = 0; i < swapchain->image_count; ++i)
        destroy_image(&swapchain->images[i]);
    memset(swapchain, 0, sizeof(*swapchain));
    args->result = GN_UNIX_SUCCESS;
    pthread_mutex_unlock(&state_mutex);
    return 0;
}

static int32_t unix_acquire_image(void *opaque)
{
    struct gn_unix_acquire_image_args *args = opaque;
    if (args->slot >= GN_UNIX_MAX_SWAPCHAINS ||
        args->image_index >= GN_UNIX_MAX_IMAGES) {
        args->result = GN_UNIX_ERROR_ARGUMENT;
        return 0;
    }
    pthread_mutex_lock(&socket_mutex);
    char line[512], response[64];
    snprintf(line, sizeof(line), "ACQUIRE eye=0 index=%u\n", args->image_index);
    /* A swapchain image can be submitted to either eye. Ask every eye on which it
       was registered; each consumer slot has an independent release fence. */
    struct gn_image *image = &swapchains[args->slot].images[args->image_index];
    int timeout_ms;
    if (args->timeout_ns < 0 || args->timeout_ns > 2147483647000000LL)
        timeout_ms = -1;
    else
        timeout_ms = (int)((args->timeout_ns + 999999) / 1000000);
    for (uint32_t eye = 0; eye < 2; ++eye) {
        if (!(image->registered_eye_mask & (1u << eye))) continue;
        const uint32_t transport_index =
            args->slot * GN_UNIX_MAX_IMAGES + args->image_index;
        snprintf(line, sizeof(line), "ACQUIRE eye=%u index=%u timeout=%d\n",
                 eye, transport_index, timeout_ms);
        if (!transact_line(line, response, sizeof(response))) {
            args->result = GN_UNIX_ERROR_TRANSPORT;
            pthread_mutex_unlock(&socket_mutex);
            return 0;
        }
        if (!strcmp(response, "ERR timeout")) {
            args->result = GN_UNIX_ERROR_TIMEOUT;
            pthread_mutex_unlock(&socket_mutex);
            return 0;
        }
        if (strncmp(response, "OK", 2)) {
            args->result = GN_UNIX_ERROR_TRANSPORT;
            pthread_mutex_unlock(&socket_mutex);
            return 0;
        }
        if (strstr(response, "fence=1")) {
            int fd = recv_fd(transport_fd);
            if (fd < 0) {
                close_transport();
                args->result = GN_UNIX_ERROR_TRANSPORT;
                pthread_mutex_unlock(&socket_mutex);
                return 0;
            }
            struct pollfd pfd = {fd, POLLIN, 0};
            int waited;
            do waited = poll(&pfd, 1, timeout_ms); while (waited < 0 && errno == EINTR);
            close(fd);
            if (waited <= 0) {
                args->result = waited == 0 ? GN_UNIX_ERROR_TIMEOUT : GN_UNIX_ERROR_TRANSPORT;
                pthread_mutex_unlock(&socket_mutex);
                return 0;
            }
        }
    }
    image->submitted = 0;
    args->result = GN_UNIX_SUCCESS;
    pthread_mutex_unlock(&socket_mutex);
    return 0;
}

static int32_t unix_submit_image(void *opaque)
{
    struct gn_unix_submit_image_args *args = opaque;
    if (args->slot >= GN_UNIX_MAX_SWAPCHAINS || args->eye >= 2 ||
        args->image_index >= swapchains[args->slot].image_count) {
        args->result = GN_UNIX_ERROR_ARGUMENT;
        return 0;
    }
    pthread_mutex_lock(&socket_mutex);
    if (!register_image(args->slot, args->image_index, args->eye,
                        args->array_index)) {
        args->result = GN_UNIX_ERROR_TRANSPORT;
        pthread_mutex_unlock(&socket_mutex);
        return 0;
    }
    int fence_fd = make_acquire_fence_fd();
    char line[512], response[64];
    const uint32_t transport_index =
        args->slot * GN_UNIX_MAX_IMAGES + args->image_index;
    snprintf(line, sizeof(line),
             "FRAME eye=%u index=%u fence=%u x=%d y=%d w=%u h=%u "
             "projection=1 qx=%lld qy=%lld qz=%lld qw=%lld "
             "px=%lld py=%lld pz=%lld fl=%lld fr=%lld fu=%lld fd=%lld\n",
             args->eye, transport_index, fence_fd >= 0 ? 1u : 0u,
             args->rect_x, args->rect_y, args->rect_width, args->rect_height,
             (long long)args->orientation_micro[0],
             (long long)args->orientation_micro[1],
             (long long)args->orientation_micro[2],
             (long long)args->orientation_micro[3],
             (long long)args->position_micro[0],
             (long long)args->position_micro[1],
             (long long)args->position_micro[2],
             (long long)args->fov_micro[0],
             (long long)args->fov_micro[1],
             (long long)args->fov_micro[2],
             (long long)args->fov_micro[3]);
    int ok = 0;
    if (fence_fd >= 0) {
        ok = transact_line(line, response, sizeof(response)) &&
             !strncmp(response, "OK", 2) &&
             send_fd(transport_fd, fence_fd) &&
             read_line(transport_fd, response, sizeof(response)) &&
             !strncmp(response, "OK", 2);
        close(fence_fd);
    } else {
        ok = transact_line(line, response, sizeof(response)) &&
             !strncmp(response, "OK", 2);
    }
    if (!ok) close_transport();
    swapchains[args->slot].images[args->image_index].submitted = ok ? 1 : 0;
    args->result = ok ? GN_UNIX_SUCCESS : GN_UNIX_ERROR_TRANSPORT;
    pthread_mutex_unlock(&socket_mutex);
    return 0;
}

__attribute__((visibility("default")))
const unixlib_entry_t __wine_unix_call_funcs[GN_UNIX_CALL_COUNT] = {
    unix_init,
    unix_set_vulkan_context,
    unix_create_swapchain,
    unix_destroy_swapchain,
    unix_acquire_image,
    unix_submit_image
};

/* Wine selects this table for a 32-bit PE running under a 64-bit wineserver.
   The ABI contains fixed-width scalars only, so the implementations are shared. */
__attribute__((visibility("default")))
const unixlib_entry_t __wine_unix_call_wow64_funcs[GN_UNIX_CALL_COUNT] = {
    unix_init,
    unix_set_vulkan_context,
    unix_create_swapchain,
    unix_destroy_swapchain,
    unix_acquire_image,
    unix_submit_image
};

__attribute__((destructor))
static void unix_shutdown(void)
{
    close_transport();
}
