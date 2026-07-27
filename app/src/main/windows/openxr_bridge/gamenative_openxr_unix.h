#pragma once

/*
 * Fixed-width ABI shared by the Windows PE runtime and its Wine unixlib.
 * No native pointers appear in this interface: 32-bit callers zero-extend
 * their pointers into the uint64 fields and use the same packed layout.
 */

#if defined(_MSC_VER)
typedef unsigned __int32 gn_u32;
typedef signed __int32 gn_i32;
typedef unsigned __int64 gn_u64;
typedef signed __int64 gn_i64;
#else
typedef unsigned int gn_u32;
typedef signed int gn_i32;
typedef unsigned long long gn_u64;
typedef signed long long gn_i64;
#endif

#define GN_UNIX_ABI_VERSION 3u
#define GN_UNIX_MAX_SWAPCHAINS 32u
#define GN_UNIX_MAX_IMAGES 4u

enum gn_unix_call_code {
    GN_UNIX_INIT = 0,
    GN_UNIX_SET_VULKAN_CONTEXT,
    GN_UNIX_CREATE_SWAPCHAIN,
    GN_UNIX_DESTROY_SWAPCHAIN,
    GN_UNIX_ACQUIRE_IMAGE,
    GN_UNIX_SUBMIT_IMAGE,
    GN_UNIX_CALL_COUNT
};

enum gn_unix_result {
    GN_UNIX_SUCCESS = 0,
    GN_UNIX_ERROR_ARGUMENT = -1,
    GN_UNIX_ERROR_UNAVAILABLE = -2,
    GN_UNIX_ERROR_VULKAN = -3,
    GN_UNIX_ERROR_TRANSPORT = -4,
    GN_UNIX_ERROR_TIMEOUT = -5
};

#pragma pack(push, 1)
struct gn_unix_init_args {
    gn_u32 abi_version;
    gn_i32 result;
};

struct gn_unix_vulkan_context_args {
    gn_u64 client_physical_device;
    gn_u64 client_device;
    gn_u64 client_queue;
    gn_u32 queue_family_index;
    gn_u32 queue_index;
    gn_u32 handles_are_host;
    gn_i32 result;
};

struct gn_unix_create_swapchain_args {
    gn_u32 slot;
    gn_u32 width;
    gn_u32 height;
    gn_u32 array_size;
    gn_u32 mip_count;
    gn_u32 sample_count;
    gn_i64 format;
    gn_u64 usage;
    gn_u32 image_count;
    gn_u64 images[GN_UNIX_MAX_IMAGES];
    gn_i32 result;
};

struct gn_unix_destroy_swapchain_args {
    gn_u32 slot;
    gn_i32 result;
};

struct gn_unix_acquire_image_args {
    gn_u32 slot;
    gn_u32 image_index;
    gn_i64 timeout_ns;
    gn_i32 result;
};

struct gn_unix_submit_image_args {
    gn_u32 slot;
    gn_u32 image_index;
    gn_u32 eye;
    gn_u32 array_index;
    gn_i32 rect_x;
    gn_i32 rect_y;
    gn_u32 rect_width;
    gn_u32 rect_height;
    gn_i64 orientation_micro[4];
    gn_i64 position_micro[3];
    gn_i64 fov_micro[4];
    gn_i32 result;
};
#pragma pack(pop)
