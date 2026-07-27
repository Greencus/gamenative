#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#define XR_USE_GRAPHICS_API_VULKAN
#include <d3d11.h>
#include <d3d12.h>
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <stdio.h>
#include <string.h>

static int fail(const char *operation, int result)
{
    fprintf(stderr, "%s failed: %d\n", operation, result);
    return 1;
}

static int luid_is_zero(const LUID *luid)
{
    return luid->LowPart == 0 && luid->HighPart == 0;
}

#define GET_XR_PROC(instance, name)                                             \
    do {                                                                        \
        PFN_xrVoidFunction raw = NULL;                                           \
        XrResult get_result = get_proc((instance), #name, &raw);                 \
        if (XR_FAILED(get_result) || !raw) return fail(#name, get_result);        \
        memcpy(&(name), &raw, sizeof(name));                                     \
    } while (0)

int main(void)
{
    HMODULE runtime = LoadLibraryA("gamenative_openxr64.dll");
    if (!runtime) return fail("LoadLibrary(runtime)", (int)GetLastError());

    PFN_xrGetInstanceProcAddr get_proc = NULL;
    FARPROC raw_get_proc = GetProcAddress(runtime, "xrGetInstanceProcAddr");
    memcpy(&get_proc, &raw_get_proc, sizeof(get_proc));
    if (!get_proc) return fail("GetProcAddress(xrGetInstanceProcAddr)", (int)GetLastError());

    PFN_xrCreateInstance xrCreateInstance = NULL;
    PFN_xrDestroyInstance xrDestroyInstance = NULL;
    PFN_xrGetInstanceProperties xrGetInstanceProperties = NULL;
    PFN_xrGetSystem xrGetSystem = NULL;
    PFN_xrGetSystemProperties xrGetSystemProperties = NULL;
    PFN_xrCreateSession xrCreateSession = NULL;
    PFN_xrDestroySession xrDestroySession = NULL;
    PFN_xrBeginSession xrBeginSession = NULL;
    PFN_xrEndSession xrEndSession = NULL;
    PFN_xrRequestExitSession xrRequestExitSession = NULL;
    PFN_xrPollEvent xrPollEvent = NULL;
    PFN_xrCreateReferenceSpace xrCreateReferenceSpace = NULL;
    PFN_xrGetReferenceSpaceBoundsRect xrGetReferenceSpaceBoundsRect = NULL;
    PFN_xrDestroySpace xrDestroySpace = NULL;
    PFN_xrLocateSpace xrLocateSpace = NULL;
    PFN_xrCreateActionSet xrCreateActionSet = NULL;
    PFN_xrDestroyActionSet xrDestroyActionSet = NULL;
    PFN_xrCreateAction xrCreateAction = NULL;
    PFN_xrStringToPath xrStringToPath = NULL;
    PFN_xrSuggestInteractionProfileBindings xrSuggestInteractionProfileBindings = NULL;
    PFN_xrEnumerateBoundSourcesForAction xrEnumerateBoundSourcesForAction = NULL;
    PFN_xrAttachSessionActionSets xrAttachSessionActionSets = NULL;
    PFN_xrSyncActions xrSyncActions = NULL;
    GET_XR_PROC(XR_NULL_HANDLE, xrCreateInstance);

    const char *extensions[] = {
        XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
        XR_KHR_D3D11_ENABLE_EXTENSION_NAME,
        XR_KHR_D3D12_ENABLE_EXTENSION_NAME
    };
    XrInstanceCreateInfo instance_info = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(instance_info.applicationInfo.applicationName, "GameNative Vulkan enable2 smoke");
    instance_info.applicationInfo.applicationVersion = 1;
    strcpy(instance_info.applicationInfo.engineName, "GameNative");
    instance_info.applicationInfo.engineVersion = 1;
    instance_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    instance_info.enabledExtensionCount =
        (uint32_t)(sizeof(extensions) / sizeof(extensions[0]));
    instance_info.enabledExtensionNames = extensions;
    XrInstance xr_instance = XR_NULL_HANDLE;
    XrResult xr_result = xrCreateInstance(&instance_info, &xr_instance);
    if (XR_FAILED(xr_result)) return fail("xrCreateInstance", xr_result);

    GET_XR_PROC(xr_instance, xrDestroyInstance);
    GET_XR_PROC(xr_instance, xrGetInstanceProperties);
    GET_XR_PROC(xr_instance, xrGetSystem);
    GET_XR_PROC(xr_instance, xrGetSystemProperties);
    GET_XR_PROC(xr_instance, xrCreateSession);
    GET_XR_PROC(xr_instance, xrDestroySession);
    GET_XR_PROC(xr_instance, xrBeginSession);
    GET_XR_PROC(xr_instance, xrEndSession);
    GET_XR_PROC(xr_instance, xrRequestExitSession);
    GET_XR_PROC(xr_instance, xrPollEvent);
    GET_XR_PROC(xr_instance, xrCreateReferenceSpace);
    GET_XR_PROC(xr_instance, xrGetReferenceSpaceBoundsRect);
    GET_XR_PROC(xr_instance, xrDestroySpace);
    GET_XR_PROC(xr_instance, xrLocateSpace);
    GET_XR_PROC(xr_instance, xrCreateActionSet);
    GET_XR_PROC(xr_instance, xrDestroyActionSet);
    GET_XR_PROC(xr_instance, xrCreateAction);
    GET_XR_PROC(xr_instance, xrStringToPath);
    GET_XR_PROC(xr_instance, xrSuggestInteractionProfileBindings);
    GET_XR_PROC(xr_instance, xrEnumerateBoundSourcesForAction);
    GET_XR_PROC(xr_instance, xrAttachSessionActionSets);
    GET_XR_PROC(xr_instance, xrSyncActions);
    PFN_xrGetVulkanGraphicsRequirements2KHR xrGetVulkanGraphicsRequirements2KHR = NULL;
    PFN_xrCreateVulkanInstanceKHR xrCreateVulkanInstanceKHR = NULL;
    PFN_xrGetVulkanGraphicsDevice2KHR xrGetVulkanGraphicsDevice2KHR = NULL;
    PFN_xrCreateVulkanDeviceKHR xrCreateVulkanDeviceKHR = NULL;
    PFN_xrGetD3D11GraphicsRequirementsKHR xrGetD3D11GraphicsRequirementsKHR = NULL;
    PFN_xrGetD3D12GraphicsRequirementsKHR xrGetD3D12GraphicsRequirementsKHR = NULL;
    GET_XR_PROC(xr_instance, xrGetVulkanGraphicsRequirements2KHR);
    GET_XR_PROC(xr_instance, xrCreateVulkanInstanceKHR);
    GET_XR_PROC(xr_instance, xrGetVulkanGraphicsDevice2KHR);
    GET_XR_PROC(xr_instance, xrCreateVulkanDeviceKHR);
    GET_XR_PROC(xr_instance, xrGetD3D11GraphicsRequirementsKHR);
    GET_XR_PROC(xr_instance, xrGetD3D12GraphicsRequirementsKHR);

    XrInstanceProperties instance_properties = {XR_TYPE_INSTANCE_PROPERTIES};
    xr_result = xrGetInstanceProperties(xr_instance, &instance_properties);
    if (XR_FAILED(xr_result)) return fail("xrGetInstanceProperties", xr_result);
    if (!strstr(instance_properties.runtimeName, "GameNativeVR"))
        return fail("GameNativeVR runtime branding", 1);

    XrSystemGetInfo system_info = {XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system_id = XR_NULL_SYSTEM_ID;
    xr_result = xrGetSystem(xr_instance, &system_info, &system_id);
    if (XR_FAILED(xr_result)) return fail("xrGetSystem", xr_result);

    XrSystemProperties system_properties = {XR_TYPE_SYSTEM_PROPERTIES};
    xr_result = xrGetSystemProperties(xr_instance, system_id, &system_properties);
    if (XR_FAILED(xr_result)) return fail("xrGetSystemProperties", xr_result);
    if (!strstr(system_properties.systemName, "GameNativeVR"))
        return fail("GameNativeVR system branding", 1);

    XrGraphicsRequirementsD3D11KHR d3d11_requirements = {
        XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR
    };
    xr_result = xrGetD3D11GraphicsRequirementsKHR(
        xr_instance, system_id, &d3d11_requirements);
    if (XR_FAILED(xr_result))
        return fail("xrGetD3D11GraphicsRequirementsKHR", xr_result);
    if (luid_is_zero(&d3d11_requirements.adapterLuid))
        return fail("D3D11 adapter LUID", 1);

    XrGraphicsRequirementsD3D12KHR d3d12_requirements = {
        XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR
    };
    xr_result = xrGetD3D12GraphicsRequirementsKHR(
        xr_instance, system_id, &d3d12_requirements);
    if (XR_FAILED(xr_result))
        return fail("xrGetD3D12GraphicsRequirementsKHR", xr_result);
    if (luid_is_zero(&d3d12_requirements.adapterLuid))
        return fail("D3D12 adapter LUID", 1);

    XrGraphicsRequirementsVulkan2KHR requirements = {
        XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR
    };
    xr_result = xrGetVulkanGraphicsRequirements2KHR(
        xr_instance, system_id, &requirements);
    if (XR_FAILED(xr_result)) return fail("xrGetVulkanGraphicsRequirements2KHR", xr_result);

    VkApplicationInfo vk_application = {
        VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL,
        "GameNative Vulkan enable2 smoke", 1, "GameNative", 1, VK_API_VERSION_1_1
    };
    VkInstanceCreateInfo vk_instance_info = {
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &vk_application,
        0, NULL, 0, NULL
    };
    XrVulkanInstanceCreateInfoKHR xr_vk_instance_info = {
        XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR
    };
    xr_vk_instance_info.systemId = system_id;
    xr_vk_instance_info.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xr_vk_instance_info.vulkanCreateInfo = &vk_instance_info;
    VkInstance vk_instance = VK_NULL_HANDLE;
    VkResult vk_result = VK_ERROR_INITIALIZATION_FAILED;
    xr_result = xrCreateVulkanInstanceKHR(
        xr_instance, &xr_vk_instance_info, &vk_instance, &vk_result);
    if (XR_FAILED(xr_result) || vk_result != VK_SUCCESS)
        return fail("xrCreateVulkanInstanceKHR", XR_FAILED(xr_result) ? xr_result : vk_result);

    XrVulkanGraphicsDeviceGetInfoKHR device_get = {
        XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR
    };
    device_get.systemId = system_id;
    device_get.vulkanInstance = vk_instance;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    xr_result = xrGetVulkanGraphicsDevice2KHR(
        xr_instance, &device_get, &physical_device);
    if (XR_FAILED(xr_result) || !physical_device)
        return fail("xrGetVulkanGraphicsDevice2KHR", xr_result);

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, NULL);
    VkQueueFamilyProperties families[32];
    if (!family_count || family_count > 32) return fail("queue families", 1);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, families);
    uint32_t family = family_count;
    for (uint32_t i = 0; i < family_count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            family = i;
            break;
        }
    }
    if (family == family_count) return fail("graphics queue", 1);

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0,
        family, 1, &priority
    };
    VkDeviceCreateInfo vk_device_info = {
        VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0,
        1, &queue_info, 0, NULL, 0, NULL, NULL
    };
    XrVulkanDeviceCreateInfoKHR xr_vk_device_info = {
        XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR
    };
    xr_vk_device_info.systemId = system_id;
    xr_vk_device_info.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xr_vk_device_info.vulkanPhysicalDevice = physical_device;
    xr_vk_device_info.vulkanCreateInfo = &vk_device_info;
    VkDevice device = VK_NULL_HANDLE;
    vk_result = VK_ERROR_INITIALIZATION_FAILED;
    xr_result = xrCreateVulkanDeviceKHR(
        xr_instance, &xr_vk_device_info, &device, &vk_result);
    if (XR_FAILED(xr_result) || vk_result != VK_SUCCESS)
        return fail("xrCreateVulkanDeviceKHR", XR_FAILED(xr_result) ? xr_result : vk_result);

    XrGraphicsBindingVulkanKHR graphics_binding = {
        XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR
    };
    graphics_binding.instance = vk_instance;
    graphics_binding.physicalDevice = physical_device;
    graphics_binding.device = device;
    graphics_binding.queueFamilyIndex = family;
    graphics_binding.queueIndex = 0;
    XrSessionCreateInfo session_info = {XR_TYPE_SESSION_CREATE_INFO};
    session_info.next = &graphics_binding;
    session_info.systemId = system_id;
    XrSession session = XR_NULL_HANDLE;
    xr_result = xrCreateSession(xr_instance, &session_info, &session);
    if (XR_FAILED(xr_result)) return fail("xrCreateSession", xr_result);

    XrReferenceSpaceCreateInfo base_space_info = {
        XR_TYPE_REFERENCE_SPACE_CREATE_INFO
    };
    base_space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    base_space_info.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace base_space = XR_NULL_HANDLE;
    xr_result = xrCreateReferenceSpace(session, &base_space_info, &base_space);
    if (XR_FAILED(xr_result)) return fail("xrCreateReferenceSpace(base)", xr_result);
    {
        XrExtent2Df bounds = {99.0f, 99.0f};
        xr_result = xrGetReferenceSpaceBoundsRect(
            session, XR_REFERENCE_SPACE_TYPE_STAGE, &bounds);
        if (xr_result != XR_SPACE_BOUNDS_UNAVAILABLE ||
            bounds.width != 0.0f || bounds.height != 0.0f) {
            return fail("xrGetReferenceSpaceBoundsRect", xr_result);
        }
    }

    XrReferenceSpaceCreateInfo offset_space_info = base_space_info;
    offset_space_info.poseInReferenceSpace.position.x = 1.0f;
    offset_space_info.poseInReferenceSpace.position.y = 2.0f;
    offset_space_info.poseInReferenceSpace.position.z = 3.0f;
    XrSpace offset_space = XR_NULL_HANDLE;
    xr_result = xrCreateReferenceSpace(session, &offset_space_info, &offset_space);
    if (XR_FAILED(xr_result)) return fail("xrCreateReferenceSpace(offset)", xr_result);

    XrSpaceVelocity velocity = {XR_TYPE_SPACE_VELOCITY};
    XrSpaceLocation location = {XR_TYPE_SPACE_LOCATION};
    location.next = &velocity;
    xr_result = xrLocateSpace(offset_space, base_space, 1, &location);
    if (XR_FAILED(xr_result)) return fail("xrLocateSpace", xr_result);
    if ((location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) == 0 ||
        location.pose.position.x != 1.0f ||
        location.pose.position.y != 2.0f ||
        location.pose.position.z != 3.0f ||
        (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) == 0) {
        return fail("reference-space composition/velocity", 1);
    }

    XrActionSetCreateInfo action_set_info = {XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(action_set_info.actionSetName, "active");
    strcpy(action_set_info.localizedActionSetName, "Active");
    XrActionSet active_set = XR_NULL_HANDLE;
    xr_result = xrCreateActionSet(xr_instance, &action_set_info, &active_set);
    if (XR_FAILED(xr_result)) return fail("xrCreateActionSet(active)", xr_result);
    strcpy(action_set_info.actionSetName, "unattached");
    strcpy(action_set_info.localizedActionSetName, "Unattached");
    XrActionSet unattached_set = XR_NULL_HANDLE;
    xr_result = xrCreateActionSet(xr_instance, &action_set_info, &unattached_set);
    if (XR_FAILED(xr_result)) return fail("xrCreateActionSet(unattached)", xr_result);

    XrActionCreateInfo action_info = {XR_TYPE_ACTION_CREATE_INFO};
    strcpy(action_info.actionName, "select");
    strcpy(action_info.localizedActionName, "Select");
    action_info.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    XrAction action = XR_NULL_HANDLE;
    xr_result = xrCreateAction(active_set, &action_info, &action);
    if (XR_FAILED(xr_result)) return fail("xrCreateAction", xr_result);

    XrPath touch_profile = XR_NULL_PATH;
    XrPath touch_binding = XR_NULL_PATH;
    XrPath simple_profile = XR_NULL_PATH;
    XrPath simple_binding = XR_NULL_PATH;
    xrStringToPath(
        xr_instance, "/interaction_profiles/oculus/touch_controller",
        &touch_profile);
    xrStringToPath(
        xr_instance, "/user/hand/left/input/thumbstick/click",
        &touch_binding);
    xrStringToPath(
        xr_instance, "/interaction_profiles/khr/simple_controller",
        &simple_profile);
    xrStringToPath(
        xr_instance, "/user/hand/left/input/select/click",
        &simple_binding);
    XrActionSuggestedBinding suggested_binding = {action, touch_binding};
    XrInteractionProfileSuggestedBinding profile_bindings = {
        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING
    };
    profile_bindings.interactionProfile = touch_profile;
    profile_bindings.countSuggestedBindings = 1;
    profile_bindings.suggestedBindings = &suggested_binding;
    xr_result = xrSuggestInteractionProfileBindings(
        xr_instance, &profile_bindings);
    if (XR_FAILED(xr_result))
        return fail("touch interaction-profile bindings", xr_result);
    suggested_binding.binding = simple_binding;
    profile_bindings.interactionProfile = simple_profile;
    xr_result = xrSuggestInteractionProfileBindings(
        xr_instance, &profile_bindings);
    if (XR_FAILED(xr_result))
        return fail("simple interaction-profile bindings", xr_result);

    XrBoundSourcesForActionEnumerateInfo bound_info = {
        XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO
    };
    bound_info.action = action;
    uint32_t bound_count = 0;
    XrPath bound_sources[2] = {XR_NULL_PATH, XR_NULL_PATH};
    xr_result = xrEnumerateBoundSourcesForAction(
        session, &bound_info, 2, &bound_count, bound_sources);
    if (XR_FAILED(xr_result) || bound_count != 1 ||
        bound_sources[0] != touch_binding) {
        return fail("interaction-profile precedence", xr_result);
    }

    XrSessionActionSetsAttachInfo attach_info = {
        XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO
    };
    attach_info.countActionSets = 1;
    attach_info.actionSets = &active_set;
    xr_result = xrAttachSessionActionSets(session, &attach_info);
    if (XR_FAILED(xr_result)) return fail("xrAttachSessionActionSets", xr_result);
    xr_result = xrAttachSessionActionSets(session, &attach_info);
    if (xr_result != XR_ERROR_ACTIONSETS_ALREADY_ATTACHED)
        return fail("attach-once enforcement", xr_result);

    XrActiveActionSet active_action_set = {active_set, XR_NULL_PATH};
    XrActionsSyncInfo sync_info = {XR_TYPE_ACTIONS_SYNC_INFO};
    sync_info.countActiveActionSets = 1;
    sync_info.activeActionSets = &active_action_set;
    xr_result = xrSyncActions(session, &sync_info);
    if (XR_FAILED(xr_result)) return fail("xrSyncActions(active)", xr_result);
    active_action_set.actionSet = unattached_set;
    xr_result = xrSyncActions(session, &sync_info);
    if (xr_result != XR_ERROR_ACTIONSET_NOT_ATTACHED)
        return fail("activeActionSets filtering", xr_result);

    XrSessionBeginInfo begin_info = {XR_TYPE_SESSION_BEGIN_INFO};
    begin_info.primaryViewConfigurationType =
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    xr_result = xrBeginSession(session, &begin_info);
    if (XR_FAILED(xr_result)) return fail("xrBeginSession", xr_result);
    xr_result = xrRequestExitSession(session);
    if (XR_FAILED(xr_result)) return fail("xrRequestExitSession", xr_result);
    xr_result = xrEndSession(session);
    if (XR_FAILED(xr_result)) return fail("xrEndSession", xr_result);

    int saw_stopping = 0;
    int saw_exiting = 0;
    for (int i = 0; i < 16; ++i) {
        XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
        xr_result = xrPollEvent(xr_instance, &event);
        if (xr_result == XR_EVENT_UNAVAILABLE) break;
        if (XR_FAILED(xr_result)) return fail("xrPollEvent", xr_result);
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged *changed =
                (const XrEventDataSessionStateChanged *)&event;
            if (changed->state == XR_SESSION_STATE_STOPPING) saw_stopping = 1;
            if (changed->state == XR_SESSION_STATE_EXITING) saw_exiting = 1;
        }
    }
    if (!saw_stopping || !saw_exiting)
        return fail("session STOPPING/EXITING events", 1);

    xrDestroyActionSet(unattached_set);
    xrDestroyActionSet(active_set);
    xrDestroySpace(offset_space);
    xrDestroySpace(base_space);
    xrDestroySession(session);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(vk_instance, NULL);
    xrDestroyInstance(xr_instance);
    FreeLibrary(runtime);
    puts("OpenXR graphics, actions, spaces, and lifecycle smoke passed");
    return 0;
}
