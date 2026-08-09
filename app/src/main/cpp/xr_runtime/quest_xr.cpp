#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES

#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <unistd.h>
#include <jni.h>
#include <openxr.h>
#include <openxr_platform.h>

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "xr_transport.h"
#include "xr_test_producer.h"

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

#define LOG_TAG "GameNativeVR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {
constexpr XrViewConfigurationType kViewType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
constexpr uint32_t kDrmFormatAbgr8888 =
    static_cast<uint32_t>('A') | (static_cast<uint32_t>('B') << 8) |
    (static_cast<uint32_t>('2') << 16) | (static_cast<uint32_t>('4') << 24);
constexpr uint32_t kDrmFormatArgb8888 =
    static_cast<uint32_t>('A') | (static_cast<uint32_t>('R') << 8) |
    (static_cast<uint32_t>('2') << 16) | (static_cast<uint32_t>('4') << 24);

struct DmaBufSync {
    uint64_t flags;
};

constexpr uint64_t kDmaBufSyncRead = 1u << 0;
constexpr uint64_t kDmaBufSyncEnd = 1u << 2;
#ifndef DMA_BUF_IOCTL_SYNC
#define DMA_BUF_IOCTL_SYNC _IOW('b', 0, DmaBufSync)
#endif

bool xrFailed(XrResult result, const char* call) {
    if (XR_FAILED(result)) {
        LOGE("%s failed: %d", call, result);
        return true;
    }
    return false;
}

#define XR_CHECK(call)        \
    do {                      \
        XrResult r = (call);  \
        if (xrFailed(r, #call)) return false; \
    } while (0)

struct EyeSwapchain {
    XrSwapchain swapchain{XR_NULL_HANDLE};
    int32_t width{0};
    int32_t height{0};
    std::vector<XrSwapchainImageOpenGLESKHR> images;
};

struct ScreenSwapchain {
    XrSwapchain swapchain{XR_NULL_HANDLE};
    int32_t width{1920};
    int32_t height{1080};
    std::vector<XrSwapchainImageOpenGLESKHR> images;
};

// One Quest Touch controller's state, refreshed each frame and forwarded to the
// Windows-side bridge through XrBridgeServer (GET_INPUT).
struct HandInput {
    bool active{false};
    bool gripValid{false};
    bool aimValid{false};
    uint32_t buttons{0};  // bit0 A/X, bit1 B/Y, bit2 stick click, bit3 menu
    float trigger{0.0f};
    float squeeze{0.0f};
    float stickX{0.0f};
    float stickY{0.0f};
    XrPosef grip{{0, 0, 0, 1}, {0, 0, 0}};
    XrPosef aim{{0, 0, 0, 1}, {0, 0, 0}};
};

struct PendingHaptic {
    bool pending{false};
    float amplitude{0.0f};
    int64_t durationNs{0};
    float frequency{0.0f};
};

class QuestXrApp {
public:
    QuestXrApp(
        JavaVM* vm,
        jobject activity,
        int renderScalePercent,
        bool theaterScreenEnabled,
        bool clockEnabled)
        : javaVm(vm),
          activityRef(activity),
          renderScalePercent(std::clamp(renderScalePercent, 50, 150)),
          theaterScreenEnabled(theaterScreenEnabled),
          clockEnabled(clockEnabled) {}

    ~QuestXrApp() {
        stop();
        JNIEnv* env = nullptr;
        if (javaVm && javaVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
            env->DeleteGlobalRef(activityRef);
        }
    }

    void start() {
        running = true;
        renderThread = std::thread([this] { threadMain(); });
    }

    void stop() {
        running = false;
        if (renderThread.joinable()) {
            renderThread.join();
        }
    }

    // Called from the bridge-server thread when the Windows game requests haptics.
    void queueHaptic(int hand, float amplitude, int64_t durationNs, float frequency) {
        if (hand < 0 || hand > 1) return;
        std::lock_guard<std::mutex> lock(hapticMutex);
        pendingHaptics[hand] = {true, amplitude, durationNs, frequency};
    }

    void requestExit() {
        exitRequested.store(true);
    }

private:
    JavaVM* javaVm{nullptr};
    jobject activityRef{nullptr};
    std::atomic<bool> running{false};
    std::atomic<bool> exitRequested{false};
    std::thread renderThread;
    int renderScalePercent{100};
    bool theaterScreenEnabled{true};
    bool clockEnabled{true};

    EGLDisplay eglDisplay{EGL_NO_DISPLAY};
    EGLConfig eglConfig{nullptr};
    EGLContext eglContext{EGL_NO_CONTEXT};
    EGLSurface eglSurface{EGL_NO_SURFACE};
    GLuint framebuffer{0};
    GLuint externalTexture{0};
    GLuint blitProgram{0};
    GLuint blitVbo{0};
    GLuint blitVao{0};
    GLint blitTextureUniform{-1};
    jobject surfaceTextureRef{nullptr};
    jmethodID surfaceTextureUpdateTexImage{nullptr};
    jmethodID activityFrameStateMethod{nullptr};
    jmethodID activityInputMethod{nullptr};
    jmethodID activityViewConfigMethod{nullptr};
    jmethodID activityOverlayReadyMethod{nullptr};
    jmethodID activityDiagnosticMethod{nullptr};
    jfloatArray trackingArrayRef{nullptr};  // 22 eye floats + stage width/height
    jfloatArray inputArrayRef{nullptr};     // global ref, 18 floats (analogs + grip/aim poses)

    XrInstance instance{XR_NULL_HANDLE};
    XrSystemId systemId{XR_NULL_SYSTEM_ID};
    XrSession session{XR_NULL_HANDLE};
    XrSpace appSpace{XR_NULL_HANDLE};
    XrSessionState sessionState{XR_SESSION_STATE_UNKNOWN};
    bool sessionRunning{false};
    bool usingStageSpace{false};
    XrExtent2Df stageBounds{};
    XrEnvironmentBlendMode blendMode{XR_ENVIRONMENT_BLEND_MODE_OPAQUE};

    std::vector<XrViewConfigurationView> configViews;
    std::vector<XrView> views;
    std::vector<XrCompositionLayerProjectionView> compositionViews;
    std::vector<EyeSwapchain> eyeSwapchains;
    ScreenSwapchain screenSwapchain;
    ScreenSwapchain clockSwapchain{XR_NULL_HANDLE, 384, 160, {}};

    // Zero-copy eye-buffer transport: the Wine producer ships per-eye AHardwareBuffers
    // (the game's actual swapchain images) over an AF_UNIX socket. We import each into a
    // GL texture and blit it into the real Quest eye swapchain for a true stereo
    // projection layer. Until a producer connects this stays idle and the legacy theater
    // path below is used unchanged.
    gamenative::xr::FrameTransport frameTransport;

    // Controller input plumbing (Quest Touch -> bridge server -> Windows game).
    XrActionSet actionSet{XR_NULL_HANDLE};
    XrAction gripPoseAction{XR_NULL_HANDLE};
    XrAction aimPoseAction{XR_NULL_HANDLE};
    XrAction triggerAction{XR_NULL_HANDLE};
    XrAction squeezeAction{XR_NULL_HANDLE};
    XrAction stickAction{XR_NULL_HANDLE};
    XrAction primaryAction{XR_NULL_HANDLE};
    XrAction secondaryAction{XR_NULL_HANDLE};
    XrAction stickClickAction{XR_NULL_HANDLE};
    XrAction menuAction{XR_NULL_HANDLE};
    XrAction hapticAction{XR_NULL_HANDLE};
    XrPath handPaths[2]{XR_NULL_PATH, XR_NULL_PATH};
    XrSpace gripSpaces[2]{XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrSpace aimSpaces[2]{XR_NULL_HANDLE, XR_NULL_HANDLE};
    bool inputReady{false};
    HandInput handInputs[2];
    std::mutex hapticMutex;
    PendingHaptic pendingHaptics[2];

    GLuint blit2DProgram{0};
    GLuint blit2DVbo{0};
    GLuint blit2DVao{0};
    GLint blit2DTextureUniform{-1};
    GLint blit2DUvTransformUniform{-1};
    GLuint overlayProgram{0};
    GLuint overlayVbo{0};
    GLint overlayColorUniform{-1};
    bool overlayReady{false};
    int64_t renderedClockMinute{-1};
    GLuint eyeTextures[gamenative::xr::FrameTransport::kEyeCount]
                      [gamenative::xr::FrameTransport::kMaxImages]{};
    EGLImageKHR eyeImages[gamenative::xr::FrameTransport::kEyeCount]
                          [gamenative::xr::FrameTransport::kMaxImages]{};
    uint64_t eyeImportedRegistration[gamenative::xr::FrameTransport::kEyeCount]
                                    [gamenative::xr::FrameTransport::kMaxImages]{};
    bool eyeDmaBufCpuFallback[gamenative::xr::FrameTransport::kEyeCount]
                              [gamenative::xr::FrameTransport::kMaxImages]{};
    int eyeCpuTextureWidth[gamenative::xr::FrameTransport::kEyeCount]
                          [gamenative::xr::FrameTransport::kMaxImages]{};
    int eyeCpuTextureHeight[gamenative::xr::FrameTransport::kEyeCount]
                           [gamenative::xr::FrameTransport::kMaxImages]{};
    void* eyeCpuMappings[gamenative::xr::FrameTransport::kEyeCount]
                        [gamenative::xr::FrameTransport::kMaxImages]{};
    size_t eyeCpuMappingLengths[gamenative::xr::FrameTransport::kEyeCount]
                               [gamenative::xr::FrameTransport::kMaxImages]{};
    uint64_t eyeCpuMappingRegistrations[gamenative::xr::FrameTransport::kEyeCount]
                                       [gamenative::xr::FrameTransport::kMaxImages]{};
    uint64_t eyeRenderedSerial[2]{0, 0};
    bool stereoResourcesReady{false};
    bool stereoProjectionAnnounced{false};
    bool stereoMilestonePublished{false};

    void threadMain() {
        JNIEnv* env = nullptr;
        const bool attached = javaVm->AttachCurrentThread(&env, nullptr) == JNI_OK;
        if (!attached || env == nullptr) {
            LOGE("Failed to attach GameNativeVR thread to JVM");
            return;
        }

        if (!initialize(env)) {
            LOGE("OpenXR initialization failed");
            cleanup(env);
            javaVm->DetachCurrentThread();
            return;
        }

        LOGI("OpenXR frame loop started");
        while (running) {
            pollEvents();
            if (exitRequested.exchange(false) && session != XR_NULL_HANDLE && sessionRunning) {
                XrResult result = xrRequestExitSession(session);
                if (XR_FAILED(result)) {
                    LOGW("xrRequestExitSession failed: %d", result);
                }
            }
            if (sessionRunning) {
                renderFrame(env);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        }

        cleanup(env);
        javaVm->DetachCurrentThread();
        LOGI("OpenXR frame loop stopped");
    }

    bool initialize(JNIEnv* env) {
        if (!initializeLoader(env)) return false;
        if (!initializeEgl()) return false;
        cacheActivityMethods(env);
        if (!createInstance()) return false;
        if (!createSession()) return false;
        if (!createReferenceSpace()) return false;
        publishViewConfig(env);
        if (!createActions()) {
            LOGW("Controller input unavailable; games will see inactive actions");
        }

        // The SurfaceTexture is required by the Wine/X11 display even when its optional
        // theater layer is disabled. Avoid allocating the separate 1080p OpenXR swapchain
        // and shader resources in that case.
        const bool screenReady = theaterScreenEnabled
            ? createScreenSwapchain(env)
            : createExternalTextureSurface(env);
        if (!screenReady) LOGW("OpenXR texture surface unavailable");
        // Eye swapchains back both the legacy placeholder and the new stereo path, so
        // create them whenever they don't already exist.
        if (eyeSwapchains.empty() && !createEyeSwapchains()) {
            if (!theaterScreenEnabled || screenSwapchain.swapchain == XR_NULL_HANDLE) {
                return false;  // no usable presentation path at all
            }
        }
        glGenFramebuffers(1, &framebuffer);

        if (clockEnabled && createClockSwapchain() && createOverlayResources()) {
            overlayReady = true;
            LOGI("XR activity overlay ready: compositor clock quad");
            publishOverlayReady(env);
        } else if (clockEnabled) {
            LOGW("XR activity overlay unavailable; base XR presentation remains active");
        }

        // Bring up the zero-copy eye-buffer transport. It is harmless when no producer
        // is connected (hasStereoContent() stays false and we render the legacy path).
        if (!eyeSwapchains.empty() && createEyeBlitResources()) {
            startStereoTransport();
        } else {
            LOGW("Stereo blit resources unavailable; true-stereo path disabled");
        }
        return true;
    }

    void startStereoTransport() {
        // Abstract-namespace AF_UNIX socket (leading '@'): no filesystem path, so it works
        // from the Android app process (which has no writable /tmp) and stays visible across
        // the proot/container net namespace for the real producer later.
        const char* envPath = std::getenv("GAMENATIVE_XR_SOCKET");
        std::string socketPath = (envPath && *envPath) ? envPath : "@gamenative-xr";
        frameTransport.start(socketPath);
        LOGI("Stereo eye-buffer transport started on %s", socketPath.c_str());

        // The in-process test producer shares two test-pattern eye buffers, proving the
        // full stereo path without Wine. Opt-in only: when active it outranks the theater
        // quad, so it must never run for normal flat-game sessions.
        //   adb shell setprop debug.gamenative.xr.testproducer 1
        bool testProducer = false;
        char prop[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.gamenative.xr.testproducer", prop) > 0) {
            testProducer = prop[0] == '1' || prop[0] == 't' || prop[0] == 'y';
        }
        const char* envToggle = std::getenv("GAMENATIVE_XR_TEST_PRODUCER");
        if (envToggle && envToggle[0] == '1') testProducer = true;

        if (testProducer && !eyeSwapchains.empty()) {
            gamenative::xr::startTestProducer(socketPath, eyeSwapchains[0].width, eyeSwapchains[0].height);
            LOGI("Stereo test producer enabled (debug.gamenative.xr.testproducer)");
        }
    }

    void cacheActivityMethods(JNIEnv* env) {
        jclass activityClass = env->GetObjectClass(activityRef);
        if (activityClass == nullptr) return;
        activityFrameStateMethod = env->GetMethodID(
            activityClass,
            "onNativeXrFrameState",
            "(JJZI[F)V");
        if (env->ExceptionCheck()) env->ExceptionClear();
        activityInputMethod = env->GetMethodID(
            activityClass,
            "onNativeXrInput",
            "(IIZ[F)V");
        if (env->ExceptionCheck()) env->ExceptionClear();
        activityViewConfigMethod = env->GetMethodID(
            activityClass,
            "onNativeXrViewConfig",
            "(II)V");
        if (env->ExceptionCheck()) env->ExceptionClear();
        activityOverlayReadyMethod = env->GetMethodID(
            activityClass,
            "onNativeXrOverlayReady",
            "()V");
        if (env->ExceptionCheck()) env->ExceptionClear();
        activityDiagnosticMethod = env->GetMethodID(
            activityClass,
            "onNativeXrDiagnostic",
            "(Ljava/lang/String;)V");
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(activityClass);

        jfloatArray tracking = env->NewFloatArray(24);
        if (tracking != nullptr) {
            trackingArrayRef = static_cast<jfloatArray>(env->NewGlobalRef(tracking));
            env->DeleteLocalRef(tracking);
        }
        jfloatArray input = env->NewFloatArray(18);
        if (input != nullptr) {
            inputArrayRef = static_cast<jfloatArray>(env->NewGlobalRef(input));
            env->DeleteLocalRef(input);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    void publishViewConfig(JNIEnv* env) {
        if (activityViewConfigMethod == nullptr || configViews.empty()) return;
        const auto& view = configViews[0];
        const uint32_t width = std::min(
            view.maxImageRectWidth,
            static_cast<uint32_t>(std::lround(
                view.recommendedImageRectWidth * renderScalePercent / 100.0)));
        const uint32_t height = std::min(
            view.maxImageRectHeight,
            static_cast<uint32_t>(std::lround(
                view.recommendedImageRectHeight * renderScalePercent / 100.0)));
        LOGI(
            "Game render resolution scale=%d%% recommended=%ux%u requested=%ux%u",
            renderScalePercent,
            view.recommendedImageRectWidth,
            view.recommendedImageRectHeight,
            width,
            height);
        env->CallVoidMethod(
            activityRef,
            activityViewConfigMethod,
            static_cast<jint>(width),
            static_cast<jint>(height));
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    void publishOverlayReady(JNIEnv* env) {
        if (env == nullptr || activityOverlayReadyMethod == nullptr) return;
        env->CallVoidMethod(activityRef, activityOverlayReadyMethod);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    void publishDiagnostic(JNIEnv* env, const char* message) {
        if (env == nullptr || activityDiagnosticMethod == nullptr || message == nullptr) return;
        jstring value = env->NewStringUTF(message);
        if (value == nullptr) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            return;
        }
        env->CallVoidMethod(activityRef, activityDiagnosticMethod, value);
        env->DeleteLocalRef(value);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    bool initializeLoader(JNIEnv*) {
        PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
        XrResult result = xrGetInstanceProcAddr(
            XR_NULL_HANDLE,
            "xrInitializeLoaderKHR",
            reinterpret_cast<PFN_xrVoidFunction*>(&initializeLoader));

        if (XR_FAILED(result) || initializeLoader == nullptr) {
            LOGE("xrInitializeLoaderKHR unavailable");
            return false;
        }

        XrLoaderInitInfoAndroidKHR loaderInfo{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        loaderInfo.applicationVM = javaVm;
        loaderInfo.applicationContext = activityRef;
        XR_CHECK(initializeLoader(reinterpret_cast<XrLoaderInitInfoBaseHeaderKHR*>(&loaderInfo)));
        return true;
    }

    bool initializeEgl() {
        eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (eglDisplay == EGL_NO_DISPLAY) {
            LOGE("eglGetDisplay failed");
            return false;
        }
        if (!eglInitialize(eglDisplay, nullptr, nullptr)) {
            LOGE("eglInitialize failed");
            return false;
        }
        const char* eglExtensions = eglQueryString(eglDisplay, EGL_EXTENSIONS);
        const bool hasDmaBufImport = eglExtensions != nullptr &&
            std::strstr(eglExtensions, "EGL_EXT_image_dma_buf_import") != nullptr;
        const bool hasDmaBufModifiers = eglExtensions != nullptr &&
            std::strstr(eglExtensions, "EGL_EXT_image_dma_buf_import_modifiers") != nullptr;
        LOGI("Quest EGL dma-buf import=%d modifiers=%d",
             hasDmaBufImport ? 1 : 0, hasDmaBufModifiers ? 1 : 0);

        const EGLint configAttribs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 24,
            EGL_NONE,
        };
        EGLint numConfigs = 0;
        if (!eglChooseConfig(eglDisplay, configAttribs, &eglConfig, 1, &numConfigs) || numConfigs < 1) {
            LOGE("eglChooseConfig failed");
            return false;
        }

        const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        eglContext = eglCreateContext(eglDisplay, eglConfig, EGL_NO_CONTEXT, contextAttribs);
        if (eglContext == EGL_NO_CONTEXT) {
            LOGE("eglCreateContext failed");
            return false;
        }

        const EGLint surfaceAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        eglSurface = eglCreatePbufferSurface(eglDisplay, eglConfig, surfaceAttribs);
        if (eglSurface == EGL_NO_SURFACE) {
            LOGE("eglCreatePbufferSurface failed");
            return false;
        }
        if (!eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext)) {
            LOGE("eglMakeCurrent failed");
            return false;
        }
        return true;
    }

    bool createInstance() {
        std::vector<const char*> extensions = {
            XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
            XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
        };

        XrApplicationInfo appInfo{};
        std::strncpy(appInfo.applicationName, "GameNativeVR", XR_MAX_APPLICATION_NAME_SIZE - 1);
        appInfo.applicationVersion = 1;
        std::strncpy(appInfo.engineName, "GameNativeVR", XR_MAX_ENGINE_NAME_SIZE - 1);
        appInfo.engineVersion = 1;
        appInfo.apiVersion = XR_CURRENT_API_VERSION;

        XrInstanceCreateInfoAndroidKHR androidInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
        androidInfo.applicationVM = javaVm;
        androidInfo.applicationActivity = activityRef;

        XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
        createInfo.next = &androidInfo;
        createInfo.applicationInfo = appInfo;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.enabledExtensionNames = extensions.data();

        XR_CHECK(xrCreateInstance(&createInfo, &instance));

        XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XR_CHECK(xrGetSystem(instance, &systemInfo, &systemId));

        chooseBlendMode();
        return true;
    }

    void chooseBlendMode() {
        uint32_t count = 0;
        if (XR_FAILED(xrEnumerateEnvironmentBlendModes(instance, systemId, kViewType, 0, &count, nullptr)) || count == 0) {
            return;
        }
        std::vector<XrEnvironmentBlendMode> modes(count);
        if (XR_FAILED(xrEnumerateEnvironmentBlendModes(instance, systemId, kViewType, count, &count, modes.data()))) {
            return;
        }
        for (auto mode : modes) {
            if (mode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE) {
                blendMode = mode;
                return;
            }
        }
        blendMode = modes[0];
    }

    bool createSession() {
        uint32_t viewCount = 0;
        XR_CHECK(xrEnumerateViewConfigurationViews(instance, systemId, kViewType, 0, &viewCount, nullptr));
        if (viewCount == 0) {
            LOGE("Runtime reported no stereo views");
            return false;
        }
        configViews.assign(viewCount, XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW});
        XR_CHECK(xrEnumerateViewConfigurationViews(instance, systemId, kViewType, viewCount, &viewCount, configViews.data()));
        views.assign(viewCount, XrView{XR_TYPE_VIEW});
        compositionViews.assign(
            viewCount,
            XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});

        PFN_xrGetOpenGLESGraphicsRequirementsKHR getRequirements = nullptr;
        XR_CHECK(xrGetInstanceProcAddr(
            instance,
            "xrGetOpenGLESGraphicsRequirementsKHR",
            reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements)));
        if (getRequirements) {
            XrGraphicsRequirementsOpenGLESKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
            XR_CHECK(getRequirements(instance, systemId, &requirements));
        }

        XrGraphicsBindingOpenGLESAndroidKHR graphicsBinding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
        graphicsBinding.display = eglDisplay;
        graphicsBinding.config = eglConfig;
        graphicsBinding.context = eglContext;

        XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
        sessionInfo.next = &graphicsBinding;
        sessionInfo.systemId = systemId;
        XR_CHECK(xrCreateSession(instance, &sessionInfo, &session));
        return true;
    }

    bool createReferenceSpace() {
        uint32_t count = 0;
        if (XR_SUCCEEDED(xrEnumerateReferenceSpaces(session, 0, &count, nullptr)) && count > 0) {
            std::vector<XrReferenceSpaceType> spaces(count);
            if (XR_SUCCEEDED(xrEnumerateReferenceSpaces(session, count, &count, spaces.data()))) {
                for (auto space : spaces) {
                    if (space == XR_REFERENCE_SPACE_TYPE_STAGE) {
                        usingStageSpace = true;
                        break;
                    }
                }
            }
        }

        XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        spaceInfo.referenceSpaceType = usingStageSpace
            ? XR_REFERENCE_SPACE_TYPE_STAGE
            : XR_REFERENCE_SPACE_TYPE_LOCAL;
        spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
        XR_CHECK(xrCreateReferenceSpace(session, &spaceInfo, &appSpace));
        if (usingStageSpace) {
            XrExtent2Df bounds{};
            if (XR_SUCCEEDED(xrGetReferenceSpaceBoundsRect(
                    session, XR_REFERENCE_SPACE_TYPE_STAGE, &bounds)) &&
                bounds.width > 0.0f && bounds.height > 0.0f) {
                stageBounds = bounds;
            }
        }
        return true;
    }

    bool createAction(const char* name, XrActionType type, XrAction* out) {
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
        info.actionType = type;
        std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
        std::strncpy(info.localizedActionName, name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        info.countSubactionPaths = 2;
        info.subactionPaths = handPaths;
        return !xrFailed(xrCreateAction(actionSet, &info, out), "xrCreateAction");
    }

    XrPath makePath(const char* str) {
        XrPath path = XR_NULL_PATH;
        xrStringToPath(instance, str, &path);
        return path;
    }

    bool createActions() {
        XR_CHECK(xrStringToPath(instance, "/user/hand/left", &handPaths[0]));
        XR_CHECK(xrStringToPath(instance, "/user/hand/right", &handPaths[1]));

        XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::strncpy(setInfo.actionSetName, "gamenative", XR_MAX_ACTION_SET_NAME_SIZE - 1);
        std::strncpy(setInfo.localizedActionSetName, "GameNativeVR", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
        XR_CHECK(xrCreateActionSet(instance, &setInfo, &actionSet));

        if (!createAction("grip_pose", XR_ACTION_TYPE_POSE_INPUT, &gripPoseAction) ||
            !createAction("aim_pose", XR_ACTION_TYPE_POSE_INPUT, &aimPoseAction) ||
            !createAction("trigger", XR_ACTION_TYPE_FLOAT_INPUT, &triggerAction) ||
            !createAction("squeeze", XR_ACTION_TYPE_FLOAT_INPUT, &squeezeAction) ||
            !createAction("thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, &stickAction) ||
            !createAction("primary_button", XR_ACTION_TYPE_BOOLEAN_INPUT, &primaryAction) ||
            !createAction("secondary_button", XR_ACTION_TYPE_BOOLEAN_INPUT, &secondaryAction) ||
            !createAction("stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, &stickClickAction) ||
            !createAction("menu_button", XR_ACTION_TYPE_BOOLEAN_INPUT, &menuAction) ||
            !createAction("haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, &hapticAction)) {
            return false;
        }

        const XrActionSuggestedBinding bindings[] = {
            {gripPoseAction, makePath("/user/hand/left/input/grip/pose")},
            {gripPoseAction, makePath("/user/hand/right/input/grip/pose")},
            {aimPoseAction, makePath("/user/hand/left/input/aim/pose")},
            {aimPoseAction, makePath("/user/hand/right/input/aim/pose")},
            {triggerAction, makePath("/user/hand/left/input/trigger/value")},
            {triggerAction, makePath("/user/hand/right/input/trigger/value")},
            {squeezeAction, makePath("/user/hand/left/input/squeeze/value")},
            {squeezeAction, makePath("/user/hand/right/input/squeeze/value")},
            {stickAction, makePath("/user/hand/left/input/thumbstick")},
            {stickAction, makePath("/user/hand/right/input/thumbstick")},
            {stickClickAction, makePath("/user/hand/left/input/thumbstick/click")},
            {stickClickAction, makePath("/user/hand/right/input/thumbstick/click")},
            {primaryAction, makePath("/user/hand/left/input/x/click")},
            {primaryAction, makePath("/user/hand/right/input/a/click")},
            {secondaryAction, makePath("/user/hand/left/input/y/click")},
            {secondaryAction, makePath("/user/hand/right/input/b/click")},
            {menuAction, makePath("/user/hand/left/input/menu/click")},
            {hapticAction, makePath("/user/hand/left/output/haptic")},
            {hapticAction, makePath("/user/hand/right/output/haptic")},
        };

        XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggested.interactionProfile = makePath("/interaction_profiles/oculus/touch_controller");
        suggested.countSuggestedBindings = static_cast<uint32_t>(sizeof(bindings) / sizeof(bindings[0]));
        suggested.suggestedBindings = bindings;
        XR_CHECK(xrSuggestInteractionProfileBindings(instance, &suggested));

        for (int hand = 0; hand < 2; ++hand) {
            XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            spaceInfo.poseInActionSpace.orientation.w = 1.0f;
            spaceInfo.subactionPath = handPaths[hand];
            spaceInfo.action = gripPoseAction;
            XR_CHECK(xrCreateActionSpace(session, &spaceInfo, &gripSpaces[hand]));
            spaceInfo.action = aimPoseAction;
            XR_CHECK(xrCreateActionSpace(session, &spaceInfo, &aimSpaces[hand]));
        }

        XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attachInfo.countActionSets = 1;
        attachInfo.actionSets = &actionSet;
        XR_CHECK(xrAttachSessionActionSets(session, &attachInfo));

        inputReady = true;
        LOGI("Controller input actions attached (oculus/touch_controller)");
        return true;
    }

    float getFloatState(XrAction action, XrPath hand) {
        XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
        getInfo.action = action;
        getInfo.subactionPath = hand;
        XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_FAILED(xrGetActionStateFloat(session, &getInfo, &state)) || !state.isActive) return 0.0f;
        return state.currentState;
    }

    bool getBoolState(XrAction action, XrPath hand) {
        XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
        getInfo.action = action;
        getInfo.subactionPath = hand;
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_FAILED(xrGetActionStateBoolean(session, &getInfo, &state)) || !state.isActive) return false;
        return state.currentState == XR_TRUE;
    }

    bool locatePose(XrSpace space, XrTime time, XrPosef* out) {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(space, appSpace, time, &location))) return false;
        constexpr XrSpaceLocationFlags required =
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if ((location.locationFlags & required) == required) {
            *out = location.pose;
            return true;
        }
        return false;
    }

    void syncInput(XrTime displayTime) {
        if (!inputReady) return;

        XrActiveActionSet activeSet{actionSet, XR_NULL_PATH};
        XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
        syncInfo.countActiveActionSets = 1;
        syncInfo.activeActionSets = &activeSet;
        const XrResult syncResult = xrSyncActions(session, &syncInfo);
        const bool focused = XR_SUCCEEDED(syncResult) && syncResult != XR_SESSION_NOT_FOCUSED;

        for (int hand = 0; hand < 2; ++hand) {
            HandInput& input = handInputs[hand];
            if (!focused) {
                input = HandInput{};
                publishInput(hand);
                continue;
            }

            XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
            getInfo.action = gripPoseAction;
            getInfo.subactionPath = handPaths[hand];
            XrActionStatePose poseState{XR_TYPE_ACTION_STATE_POSE};
            input.active = XR_SUCCEEDED(xrGetActionStatePose(session, &getInfo, &poseState)) &&
                poseState.isActive == XR_TRUE;
            input.gripValid = false;
            input.aimValid = false;

            input.trigger = getFloatState(triggerAction, handPaths[hand]);
            input.squeeze = getFloatState(squeezeAction, handPaths[hand]);

            XrActionStateGetInfo stickInfo{XR_TYPE_ACTION_STATE_GET_INFO};
            stickInfo.action = stickAction;
            stickInfo.subactionPath = handPaths[hand];
            XrActionStateVector2f stickState{XR_TYPE_ACTION_STATE_VECTOR2F};
            if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &stickInfo, &stickState)) && stickState.isActive) {
                input.stickX = stickState.currentState.x;
                input.stickY = stickState.currentState.y;
            } else {
                input.stickX = 0.0f;
                input.stickY = 0.0f;
            }

            input.buttons = 0;
            if (getBoolState(primaryAction, handPaths[hand])) input.buttons |= 1u;
            if (getBoolState(secondaryAction, handPaths[hand])) input.buttons |= 2u;
            if (getBoolState(stickClickAction, handPaths[hand])) input.buttons |= 4u;
            if (hand == 0 && getBoolState(menuAction, handPaths[hand])) input.buttons |= 8u;

            if (input.active) {
                input.gripValid = locatePose(gripSpaces[hand], displayTime, &input.grip);
                input.aimValid = locatePose(aimSpaces[hand], displayTime, &input.aim);
            }
            publishInput(hand);
        }

        applyPendingHaptics();
    }

    void publishInput(int hand) {
        if (activityInputMethod == nullptr || inputArrayRef == nullptr) return;
        JNIEnv* env = nullptr;
        if (javaVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr) {
            return;
        }
        const HandInput& input = handInputs[hand];
        const float data[18] = {
            input.trigger, input.squeeze, input.stickX, input.stickY,
            input.grip.orientation.x, input.grip.orientation.y, input.grip.orientation.z, input.grip.orientation.w,
            input.grip.position.x, input.grip.position.y, input.grip.position.z,
            input.aim.orientation.x, input.aim.orientation.y, input.aim.orientation.z, input.aim.orientation.w,
            input.aim.position.x, input.aim.position.y, input.aim.position.z,
        };
        env->SetFloatArrayRegion(inputArrayRef, 0, 18, data);
        env->CallVoidMethod(
            activityRef,
            activityInputMethod,
            static_cast<jint>(hand),
            static_cast<jint>(input.buttons),
            static_cast<jboolean>(input.active),
            inputArrayRef);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    void applyPendingHaptics() {
        PendingHaptic local[2];
        {
            std::lock_guard<std::mutex> lock(hapticMutex);
            local[0] = pendingHaptics[0];
            local[1] = pendingHaptics[1];
            pendingHaptics[0].pending = false;
            pendingHaptics[1].pending = false;
        }
        for (int hand = 0; hand < 2; ++hand) {
            if (!local[hand].pending || hapticAction == XR_NULL_HANDLE) continue;
            XrHapticActionInfo actionInfo{XR_TYPE_HAPTIC_ACTION_INFO};
            actionInfo.action = hapticAction;
            actionInfo.subactionPath = handPaths[hand];
            if (local[hand].amplitude <= 0.0f) {
                xrStopHapticFeedback(session, &actionInfo);
                continue;
            }
            XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
            vibration.amplitude = local[hand].amplitude > 1.0f ? 1.0f : local[hand].amplitude;
            vibration.duration = local[hand].durationNs > 0 ? local[hand].durationNs : XR_MIN_HAPTIC_DURATION;
            vibration.frequency = local[hand].frequency > 0.0f ? local[hand].frequency : XR_FREQUENCY_UNSPECIFIED;
            xrApplyHapticFeedback(session, &actionInfo, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
        }
    }

    bool createScreenSwapchain(JNIEnv* env) {
        uint32_t formatCount = 0;
        XR_CHECK(xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr));
        if (formatCount == 0) {
            LOGE("Runtime reported no swapchain formats");
            return false;
        }
        std::vector<int64_t> formats(formatCount);
        XR_CHECK(xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data()));

        int64_t selectedFormat = formats[0];
        for (int64_t format : formats) {
            if (format == GL_RGBA8) {
                selectedFormat = format;
                break;
            }
        }

        XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        createInfo.format = selectedFormat;
        createInfo.sampleCount = 1;
        createInfo.width = screenSwapchain.width;
        createInfo.height = screenSwapchain.height;
        createInfo.faceCount = 1;
        createInfo.arraySize = 1;
        createInfo.mipCount = 1;

        XR_CHECK(xrCreateSwapchain(session, &createInfo, &screenSwapchain.swapchain));

        uint32_t imageCount = 0;
        XR_CHECK(xrEnumerateSwapchainImages(screenSwapchain.swapchain, 0, &imageCount, nullptr));
        screenSwapchain.images.assign(imageCount, XrSwapchainImageOpenGLESKHR{XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        XR_CHECK(xrEnumerateSwapchainImages(
            screenSwapchain.swapchain,
            imageCount,
            &imageCount,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(screenSwapchain.images.data())));

        if (!createExternalTextureSurface(env)) {
            return false;
        }
        if (!createBlitResources()) {
            return false;
        }
        LOGI("OpenXR SurfaceTexture screen path ready");
        return true;
    }

    bool createExternalTextureSurface(JNIEnv* env) {
        glGenTextures(1, &externalTexture);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, externalTexture);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

        jclass activityClass = env->GetObjectClass(activityRef);
        if (activityClass == nullptr) return false;
        jmethodID method = env->GetMethodID(
            activityClass,
            "createNativeXrSurfaceTexture",
            "(III)Landroid/graphics/SurfaceTexture;");
        env->DeleteLocalRef(activityClass);
        if (method == nullptr) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            LOGE("QuestVrActivity.createNativeXrSurfaceTexture not found");
            return false;
        }

        jobject surfaceTexture = env->CallObjectMethod(
            activityRef,
            method,
            static_cast<jint>(externalTexture),
            screenSwapchain.width,
            screenSwapchain.height);
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            LOGE("QuestVrActivity.createNativeXrSurfaceTexture threw");
            return false;
        }
        if (surfaceTexture == nullptr) {
            LOGE("QuestVrActivity returned null SurfaceTexture");
            return false;
        }

        surfaceTextureRef = env->NewGlobalRef(surfaceTexture);
        env->DeleteLocalRef(surfaceTexture);
        jclass textureClass = env->GetObjectClass(surfaceTextureRef);
        surfaceTextureUpdateTexImage = env->GetMethodID(textureClass, "updateTexImage", "()V");
        env->DeleteLocalRef(textureClass);
        if (surfaceTextureUpdateTexImage == nullptr) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            LOGE("SurfaceTexture.updateTexImage not found");
            return false;
        }
        return true;
    }

    GLuint compileShader(GLenum type, const char* source) {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint ok = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (ok != GL_TRUE) {
            char log[512] = {};
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            LOGE("Shader compile failed: %s", log);
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    bool createBlitResources() {
        const char* vertexSource = R"(#version 300 es
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
out vec2 vUv;
void main() {
    vUv = aUv;
    gl_Position = vec4(aPos, 0.0, 1.0);
})";
        const char* fragmentSource = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
uniform samplerExternalOES uTexture;
in vec2 vUv;
out vec4 fragColor;
void main() {
    fragColor = texture(uTexture, vUv);
})";

        GLuint vs = compileShader(GL_VERTEX_SHADER, vertexSource);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
        if (vs == 0 || fs == 0) {
            if (vs != 0) glDeleteShader(vs);
            if (fs != 0) glDeleteShader(fs);
            return false;
        }
        blitProgram = glCreateProgram();
        glAttachShader(blitProgram, vs);
        glAttachShader(blitProgram, fs);
        glLinkProgram(blitProgram);
        glDeleteShader(vs);
        glDeleteShader(fs);

        GLint ok = GL_FALSE;
        glGetProgramiv(blitProgram, GL_LINK_STATUS, &ok);
        if (ok != GL_TRUE) {
            char log[512] = {};
            glGetProgramInfoLog(blitProgram, sizeof(log), nullptr, log);
            LOGE("Blit program link failed: %s", log);
            glDeleteProgram(blitProgram);
            blitProgram = 0;
            return false;
        }
        blitTextureUniform = glGetUniformLocation(blitProgram, "uTexture");

        const float vertices[] = {
            -1.0f, -1.0f, 0.0f, 1.0f,
             1.0f, -1.0f, 1.0f, 1.0f,
            -1.0f,  1.0f, 0.0f, 0.0f,
             1.0f,  1.0f, 1.0f, 0.0f,
        };
        glGenBuffers(1, &blitVbo);
        glBindBuffer(GL_ARRAY_BUFFER, blitVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        glGenVertexArrays(1, &blitVao);
        glBindVertexArray(blitVao);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<const void*>(0));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<const void*>(2 * sizeof(float)));
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glUseProgram(blitProgram);
        glUniform1i(blitTextureUniform, 0);
        glUseProgram(0);
        return true;
    }

    // Build the GL resources used to sample imported AHardwareBuffer eye images
    // (sampler2D, unlike the SurfaceTexture path which uses samplerExternalOES).
    bool createEyeBlitResources() {
        const char* vertexSource = R"(#version 300 es
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
out vec2 vUv;
void main() {
    vUv = aUv;
    gl_Position = vec4(aPos, 0.0, 1.0);
})";
        const char* fragmentSource = R"(#version 300 es
precision mediump float;
uniform sampler2D uTexture;
uniform vec4 uUvTransform;
in vec2 vUv;
out vec4 fragColor;
void main() {
    fragColor = texture(uTexture, uUvTransform.xy + vUv * uUvTransform.zw);
})";

        GLuint vs = compileShader(GL_VERTEX_SHADER, vertexSource);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
        if (vs == 0 || fs == 0) {
            if (vs != 0) glDeleteShader(vs);
            if (fs != 0) glDeleteShader(fs);
            return false;
        }
        blit2DProgram = glCreateProgram();
        glAttachShader(blit2DProgram, vs);
        glAttachShader(blit2DProgram, fs);
        glLinkProgram(blit2DProgram);
        glDeleteShader(vs);
        glDeleteShader(fs);

        GLint ok = GL_FALSE;
        glGetProgramiv(blit2DProgram, GL_LINK_STATUS, &ok);
        if (ok != GL_TRUE) {
            char log[512] = {};
            glGetProgramInfoLog(blit2DProgram, sizeof(log), nullptr, log);
            LOGE("Eye blit program link failed: %s", log);
            glDeleteProgram(blit2DProgram);
            blit2DProgram = 0;
            return false;
        }
        blit2DTextureUniform = glGetUniformLocation(blit2DProgram, "uTexture");
        blit2DUvTransformUniform = glGetUniformLocation(blit2DProgram, "uUvTransform");

        const float vertices[] = {
            -1.0f, -1.0f, 0.0f, 1.0f,
             1.0f, -1.0f, 1.0f, 1.0f,
            -1.0f,  1.0f, 0.0f, 0.0f,
             1.0f,  1.0f, 1.0f, 0.0f,
        };
        glGenBuffers(1, &blit2DVbo);
        glBindBuffer(GL_ARRAY_BUFFER, blit2DVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        glGenVertexArrays(1, &blit2DVao);
        glBindVertexArray(blit2DVao);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<const void*>(0));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<const void*>(2 * sizeof(float)));
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glUseProgram(blit2DProgram);
        glUniform1i(blit2DTextureUniform, 0);
        glUseProgram(0);
        stereoResourcesReady = true;
        return true;
    }

    // Build an EGLImage from an AHardwareBuffer (test-producer path).
    EGLImageKHR createImageFromHardwareBuffer(AHardwareBuffer* buffer) {
        EGLClientBuffer clientBuffer = eglGetNativeClientBufferANDROID(buffer);
        if (clientBuffer == nullptr) {
            LOGW("eglGetNativeClientBufferANDROID failed");
            return EGL_NO_IMAGE_KHR;
        }
        const EGLint attribs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        return eglCreateImageKHR(eglDisplay, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                 clientBuffer, attribs);
    }

    // Build an EGLImage from a one-to-four-plane dma-buf (real game path). EGL duplicates
    // the fds, so the transport keeps ownership of the registered copies.
    EGLImageKHR createImageFromDmabuf(const gamenative::xr::EyeFrame& f) {
        static constexpr EGLint kPlaneFd[] = {
            EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
            EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT};
        static constexpr EGLint kPlaneOffset[] = {
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
            EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT};
        static constexpr EGLint kPlanePitch[] = {
            EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
            EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT};
        static constexpr EGLint kModifierLo[] = {
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
            EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
        static constexpr EGLint kModifierHi[] = {
            EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
            EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};
        EGLint attribs[64];
        int i = 0;
        attribs[i++] = EGL_WIDTH;                      attribs[i++] = f.width;
        attribs[i++] = EGL_HEIGHT;                     attribs[i++] = f.height;
        attribs[i++] = EGL_LINUX_DRM_FOURCC_EXT;       attribs[i++] = static_cast<EGLint>(f.fourcc);
        for (int plane = 0; plane < f.planeCount; ++plane) {
            attribs[i++] = kPlaneFd[plane];     attribs[i++] = f.dmabufFds[plane];
            attribs[i++] = kPlaneOffset[plane]; attribs[i++] = static_cast<EGLint>(f.offsets[plane]);
            attribs[i++] = kPlanePitch[plane];  attribs[i++] = static_cast<EGLint>(f.strides[plane]);
            if (f.modifier != 0) {
                attribs[i++] = kModifierLo[plane];
                attribs[i++] = static_cast<EGLint>(f.modifier & 0xFFFFFFFFu);
                attribs[i++] = kModifierHi[plane];
                attribs[i++] = static_cast<EGLint>(f.modifier >> 32);
            }
        }
        attribs[i++] = EGL_NONE;
        EGLImageKHR image = eglCreateImageKHR(
            eglDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
            static_cast<EGLClientBuffer>(nullptr), attribs);
        if (image == EGL_NO_IMAGE_KHR) {
            const EGLint error = eglGetError();
            LOGE("dma-buf EGL import failed error=0x%x size=%dx%d fourcc=0x%08x "
                 "planes=%d modifier=0x%llx stride0=%u offset0=%u fd0=%d",
                 error, f.width, f.height, f.fourcc, f.planeCount,
                 static_cast<unsigned long long>(f.modifier), f.strides[0],
                 f.offsets[0], f.dmabufFds[0]);
        }
        return image;
    }

    // Import the latest buffer for `eye` into eyeTextures[eye] as a GL_TEXTURE_2D, dispatching
    // on how it was shared (AHardwareBuffer vs dma-buf fd). Reuses the existing EGLImage when
    // the producer hasn't presented a new buffer, so the steady-state per-frame cost is a
    // single texture bind (zero-copy). Returns false when no buffer is available yet.
    bool waitForAcquireFence(int fenceFd) {
        if (fenceFd < 0) return true;
        const EGLint attrs[] = {
            EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fenceFd,
            EGL_NONE
        };
        EGLSyncKHR sync = eglCreateSyncKHR(
            eglDisplay, EGL_SYNC_NATIVE_FENCE_ANDROID, attrs);
        if (sync != EGL_NO_SYNC_KHR) {
            // eglCreateSyncKHR takes ownership of fenceFd on success. This inserts a
            // server-side wait without stalling the XR render thread.
            const EGLBoolean waited = eglWaitSyncKHR(eglDisplay, sync, 0);
            eglDestroySyncKHR(eglDisplay, sync);
            return waited == EGL_TRUE;
        }

        // Older EGL stacks may omit Android native-fence sync. Preserve correctness
        // with a CPU wait in that exceptional path.
        pollfd pfd{fenceFd, POLLIN, 0};
        int result;
        do { result = ::poll(&pfd, 1, 5000); } while (result < 0 && errno == EINTR);
        ::close(fenceFd);
        return result > 0;
    }

    int createReleaseFence() {
        const auto dupNativeFenceFd =
            reinterpret_cast<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>(
                eglGetProcAddress("eglDupNativeFenceFDANDROID"));
        if (dupNativeFenceFd == nullptr) {
            glFinish();
            return -1;
        }
        const EGLint attrs[] = {
            EGL_SYNC_NATIVE_FENCE_FD_ANDROID, EGL_NO_NATIVE_FENCE_FD_ANDROID,
            EGL_NONE
        };
        EGLSyncKHR sync = eglCreateSyncKHR(
            eglDisplay, EGL_SYNC_NATIVE_FENCE_ANDROID, attrs);
        if (sync == EGL_NO_SYNC_KHR) {
            // Correct fallback for devices lacking the extension: finish the sampling
            // work and tell the producer it may reuse the image without a fence.
            glFinish();
            return -1;
        }
        glFlush();
        const int fd = dupNativeFenceFd(eglDisplay, sync);
        eglDestroySyncKHR(eglDisplay, sync);
        if (fd < 0) glFinish();
        return fd;
    }

    bool uploadLinearDmabufToTexture(
        int eye, int imageIndex, const gamenative::xr::EyeFrame& frame,
        GLuint& texture, uint64_t& cachedRegistration) {
        if (frame.planeCount != 1 || frame.dmabufFds[0] < 0 ||
            frame.modifier != 0 || frame.width <= 0 || frame.height <= 0 ||
            frame.strides[0] < static_cast<uint32_t>(frame.width) * 4u ||
            (frame.fourcc != kDrmFormatAbgr8888 &&
             frame.fourcc != kDrmFormatArgb8888)) {
            LOGE("CPU dma-buf fallback does not support eye=%d fourcc=0x%08x "
                 "planes=%d modifier=0x%llx stride=%u",
                 eye, frame.fourcc, frame.planeCount,
                 static_cast<unsigned long long>(frame.modifier), frame.strides[0]);
            return false;
        }

        const size_t rowBytes = static_cast<size_t>(frame.width) * 4u;
        const size_t mapLength = static_cast<size_t>(frame.offsets[0]) +
            static_cast<size_t>(frame.strides[0]) *
                static_cast<size_t>(frame.height - 1) + rowBytes;
        if (mapLength < rowBytes) return false;

        void*& mapping = eyeCpuMappings[eye][imageIndex];
        size_t& mappedLength = eyeCpuMappingLengths[eye][imageIndex];
        uint64_t& mappedRegistration = eyeCpuMappingRegistrations[eye][imageIndex];
        if (mapping != nullptr &&
            (mappedRegistration != frame.registrationSerial || mappedLength < mapLength)) {
            ::munmap(mapping, mappedLength);
            mapping = nullptr;
            mappedLength = 0;
            mappedRegistration = 0;
        }
        if (mapping == nullptr) {
            mapping = ::mmap(
                nullptr, mapLength, PROT_READ, MAP_SHARED, frame.dmabufFds[0], 0);
            if (mapping == MAP_FAILED) {
                mapping = nullptr;
                LOGE("CPU dma-buf mmap failed eye=%d length=%zu offset=%u: %s",
                     eye, mapLength, frame.offsets[0], strerror(errno));
                return false;
            }
            mappedLength = mapLength;
            mappedRegistration = frame.registrationSerial;
        }

        if (texture == 0) glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        while (glGetError() != GL_NO_ERROR) {}
        const bool allocate =
            eyeCpuTextureWidth[eye][imageIndex] != frame.width ||
            eyeCpuTextureHeight[eye][imageIndex] != frame.height ||
            cachedRegistration != frame.registrationSerial;
        if (allocate) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            // ARGB8888 is byte-ordered BGRA on little-endian Android. Let the texture
            // sampler exchange red and blue instead of converting every pixel on the CPU.
            const bool swapRedBlue = frame.fourcc == kDrmFormatArgb8888;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R,
                            swapRedBlue ? GL_BLUE : GL_RED);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B,
                            swapRedBlue ? GL_RED : GL_BLUE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ALPHA);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, frame.width, frame.height,
                         0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        }

        DmaBufSync sync{kDmaBufSyncRead};
        if (::ioctl(frame.dmabufFds[0], DMA_BUF_IOCTL_SYNC, &sync) != 0 &&
            errno != ENOTTY && errno != EINVAL) {
            LOGW("DMA_BUF_IOCTL_SYNC start failed eye=%d: %s", eye, strerror(errno));
        }
        const auto* sourceBase = static_cast<const uint8_t*>(mapping) + frame.offsets[0];
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(frame.strides[0] / 4u));
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height,
                        GL_RGBA, GL_UNSIGNED_BYTE, sourceBase);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        const GLenum uploadError = glGetError();
        glBindTexture(GL_TEXTURE_2D, 0);
        sync.flags = kDmaBufSyncRead | kDmaBufSyncEnd;
        if (::ioctl(frame.dmabufFds[0], DMA_BUF_IOCTL_SYNC, &sync) != 0 &&
            errno != ENOTTY && errno != EINVAL) {
            LOGW("DMA_BUF_IOCTL_SYNC end failed eye=%d: %s", eye, strerror(errno));
        }
        if (uploadError != GL_NO_ERROR) {
            LOGE("CPU dma-buf texture upload failed eye=%d index=%d error=0x%x",
                 eye, imageIndex, uploadError);
            return false;
        }
        if (allocate) {
            LOGI("Direct mapped dma-buf fallback active eye=%d index=%d size=%dx%d fourcc=0x%08x",
                 eye, imageIndex, frame.width, frame.height, frame.fourcc);
        }
        eyeCpuTextureWidth[eye][imageIndex] = frame.width;
        eyeCpuTextureHeight[eye][imageIndex] = frame.height;
        cachedRegistration = frame.registrationSerial;
        return true;
    }

    bool importEyeBuffer(int eye, gamenative::xr::EyeFrame& frame, bool& freshFrame) {
        frame = frameTransport.pollEye(eye);
        if (frame.kind == gamenative::xr::BufferKind::None) return false;
        freshFrame = frame.serial != eyeRenderedSerial[eye];
        if (!waitForAcquireFence(frame.acquireFenceFd)) {
            LOGE("Acquire fence wait failed/timed out for eye %d index %d", eye, frame.imageIndex);
            frameTransport.discardFrame(
                eye, frame.imageIndex, frame.serial);
            if (freshFrame) {
                eyeRenderedSerial[eye] = frame.serial;
            }
            return false;
        }
        frame.acquireFenceFd = -1;
        const int imageIndex = frame.imageIndex;
        if (imageIndex < 0 ||
            imageIndex >= gamenative::xr::FrameTransport::kMaxImages) {
            frameTransport.discardFrame(
                eye, frame.imageIndex, frame.serial);
            if (freshFrame) {
                eyeRenderedSerial[eye] = frame.serial;
            }
            return false;
        }
        EGLImageKHR& cachedImage = eyeImages[eye][imageIndex];
        GLuint& cachedTexture = eyeTextures[eye][imageIndex];
        uint64_t& cachedRegistration =
            eyeImportedRegistration[eye][imageIndex];
        if (cachedImage != EGL_NO_IMAGE_KHR &&
            frame.registrationSerial == cachedRegistration) {
            return true;  // same registered buffer, newly synchronized contents
        }

        if (eyeDmaBufCpuFallback[eye][imageIndex] &&
            frame.registrationSerial == cachedRegistration && !freshFrame) {
            return cachedTexture != 0;
        }

        if (cachedImage != EGL_NO_IMAGE_KHR) {
            eglDestroyImageKHR(eglDisplay, cachedImage);
            cachedImage = EGL_NO_IMAGE_KHR;
        }

        EGLImageKHR image = EGL_NO_IMAGE_KHR;
        if (frame.kind == gamenative::xr::BufferKind::HardwareBuffer && frame.buffer != nullptr) {
            image = createImageFromHardwareBuffer(frame.buffer);
        } else if (frame.kind == gamenative::xr::BufferKind::DmaBuf &&
                   frame.planeCount > 0 && frame.dmabufFds[0] >= 0) {
            if (!eyeDmaBufCpuFallback[eye][imageIndex]) {
                image = createImageFromDmabuf(frame);
                if (image == EGL_NO_IMAGE_KHR) {
                    eyeDmaBufCpuFallback[eye][imageIndex] = true;
                }
            }
        }
        if (image == EGL_NO_IMAGE_KHR) {
            if (frame.kind == gamenative::xr::BufferKind::DmaBuf &&
                eyeDmaBufCpuFallback[eye][imageIndex] &&
                uploadLinearDmabufToTexture(
                    eye, imageIndex, frame, cachedTexture, cachedRegistration)) {
                return true;
            }
            LOGE("eglCreateImageKHR failed for eye %d (kind=%d)", eye, static_cast<int>(frame.kind));
            frameTransport.discardFrame(
                eye, frame.imageIndex, frame.serial);
            if (freshFrame) {
                eyeRenderedSerial[eye] = frame.serial;
            }
            return false;
        }

        if (cachedTexture == 0) glGenTextures(1, &cachedTexture);
        glBindTexture(GL_TEXTURE_2D, cachedTexture);
        while (glGetError() != GL_NO_ERROR) {}
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
        const GLenum importError = glGetError();
        if (importError != GL_NO_ERROR) {
            LOGE("glEGLImageTargetTexture2DOES failed eye=%d index=%d error=0x%x",
                 eye, imageIndex, importError);
            glBindTexture(GL_TEXTURE_2D, 0);
            eglDestroyImageKHR(eglDisplay, image);
            frameTransport.discardFrame(eye, frame.imageIndex, frame.serial);
            if (freshFrame) eyeRenderedSerial[eye] = frame.serial;
            return false;
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R,
                        frame.swapRedBlue ? GL_BLUE : GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B,
                        frame.swapRedBlue ? GL_RED : GL_BLUE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ALPHA);
        glBindTexture(GL_TEXTURE_2D, 0);

        cachedImage = image;
        cachedRegistration = frame.registrationSerial;
        return true;
    }

    void blit2DTexture(GLuint texture, int width, int height,
                       const gamenative::xr::EyeFrame& source) {
        glViewport(0, 0, width, height);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        glUseProgram(blit2DProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        const float sourceWidth = source.sourceWidth > 0
            ? static_cast<float>(source.sourceWidth)
            : static_cast<float>(source.width);
        const float sourceHeight = source.sourceHeight > 0
            ? static_cast<float>(source.sourceHeight)
            : static_cast<float>(source.height);
        glUniform4f(
            blit2DUvTransformUniform,
            static_cast<float>(source.sourceX) / static_cast<float>(source.width),
            static_cast<float>(source.sourceY) / static_cast<float>(source.height),
            sourceWidth / static_cast<float>(source.width),
            sourceHeight / static_cast<float>(source.height));
        glBindVertexArray(blit2DVao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
    }

    // True per-eye stereo: import each eye's game-rendered AHardwareBuffer and blit it into
    // the matching Quest eye swapchain, then submit a projection layer. Returns false when
    // stereo content isn't ready, so the caller can fall back to the legacy path.
    bool renderStereoProjection(uint32_t viewCount,
                                XrCompositionLayerProjection& projectionLayer,
                                const XrCompositionLayerBaseHeader*& submittedLayer) {
        if (!stereoResourcesReady || !frameTransport.hasStereoContent() || eyeSwapchains.empty()) {
            return false;
        }

        if (viewCount > eyeSwapchains.size()) viewCount = static_cast<uint32_t>(eyeSwapchains.size());

        for (uint32_t eye = 0; eye < viewCount; ++eye) {
            compositionViews[eye] = {
                XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            auto& swapchain = eyeSwapchains[eye];
            gamenative::xr::EyeFrame sourceFrame;
            bool freshFrame = false;
            if (!importEyeBuffer(static_cast<int>(eye), sourceFrame, freshFrame)) return false;

            uint32_t imageIndex = 0;
            XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            if (xrFailed(xrAcquireSwapchainImage(
                    swapchain.swapchain, &acquireInfo, &imageIndex),
                    "xrAcquireSwapchainImage(stereo)")) {
                if (freshFrame) {
                    frameTransport.discardFrame(
                        static_cast<int>(eye), sourceFrame.imageIndex,
                        sourceFrame.serial);
                    eyeRenderedSerial[eye] = sourceFrame.serial;
                }
                return false;
            }
            XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            waitInfo.timeout = XR_INFINITE_DURATION;
            if (xrFailed(xrWaitSwapchainImage(
                    swapchain.swapchain, &waitInfo),
                    "xrWaitSwapchainImage(stereo)")) {
                if (freshFrame) {
                    frameTransport.discardFrame(
                        static_cast<int>(eye), sourceFrame.imageIndex,
                        sourceFrame.serial);
                    eyeRenderedSerial[eye] = sourceFrame.serial;
                }
                return false;
            }

            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   swapchain.images[imageIndex].image, 0);
            blit2DTexture(
                eyeTextures[eye][sourceFrame.imageIndex],
                swapchain.width, swapchain.height, sourceFrame);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (freshFrame) {
                const int releaseFenceFd = createReleaseFence();
                frameTransport.publishReleaseFence(
                    static_cast<int>(eye), sourceFrame.imageIndex, releaseFenceFd);
                eyeRenderedSerial[eye] = sourceFrame.serial;
            } else {
                glFlush();
            }

            XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(swapchain.swapchain, &releaseInfo);

            if (sourceFrame.projectionValid) {
                compositionViews[eye].pose.orientation = {
                    sourceFrame.projectionOrientation[0],
                    sourceFrame.projectionOrientation[1],
                    sourceFrame.projectionOrientation[2],
                    sourceFrame.projectionOrientation[3],
                };
                compositionViews[eye].pose.position = {
                    sourceFrame.projectionPosition[0],
                    sourceFrame.projectionPosition[1],
                    sourceFrame.projectionPosition[2],
                };
                compositionViews[eye].fov = {
                    sourceFrame.projectionFov[0],
                    sourceFrame.projectionFov[1],
                    sourceFrame.projectionFov[2],
                    sourceFrame.projectionFov[3],
                };
            } else {
                compositionViews[eye].pose = views[eye].pose;
                compositionViews[eye].fov = views[eye].fov;
            }
            compositionViews[eye].subImage.swapchain = swapchain.swapchain;
            compositionViews[eye].subImage.imageRect.offset = {0, 0};
            compositionViews[eye].subImage.imageRect.extent = {swapchain.width, swapchain.height};
        }

        projectionLayer.space = appSpace;
        projectionLayer.viewCount = viewCount;
        projectionLayer.views = compositionViews.data();
        submittedLayer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projectionLayer);

        if (!stereoProjectionAnnounced) {
            LOGI("Stereo projection ACTIVE: presenting %u eye buffers", projectionLayer.viewCount);
            stereoProjectionAnnounced = true;
        }
        return true;
    }

    bool createEyeSwapchains() {
        uint32_t formatCount = 0;
        XR_CHECK(xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr));
        if (formatCount == 0) {
            LOGE("Runtime reported no swapchain formats");
            return false;
        }
        std::vector<int64_t> formats(formatCount);
        XR_CHECK(xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data()));

        int64_t selectedFormat = formats[0];
        for (int64_t format : formats) {
            if (format == GL_RGBA8) {
                selectedFormat = format;
                break;
            }
        }

        eyeSwapchains.resize(configViews.size());
        for (size_t eye = 0; eye < configViews.size(); ++eye) {
            auto& cfg = configViews[eye];
            auto& swapchain = eyeSwapchains[eye];
            swapchain.width = static_cast<int32_t>(std::min(
                cfg.maxImageRectWidth,
                static_cast<uint32_t>(std::lround(
                    cfg.recommendedImageRectWidth * renderScalePercent / 100.0))));
            swapchain.height = static_cast<int32_t>(std::min(
                cfg.maxImageRectHeight,
                static_cast<uint32_t>(std::lround(
                    cfg.recommendedImageRectHeight * renderScalePercent / 100.0))));

            XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
            createInfo.format = selectedFormat;
            createInfo.sampleCount = cfg.recommendedSwapchainSampleCount;
            createInfo.width = swapchain.width;
            createInfo.height = swapchain.height;
            createInfo.faceCount = 1;
            createInfo.arraySize = 1;
            createInfo.mipCount = 1;
            XR_CHECK(xrCreateSwapchain(session, &createInfo, &swapchain.swapchain));

            uint32_t imageCount = 0;
            XR_CHECK(xrEnumerateSwapchainImages(swapchain.swapchain, 0, &imageCount, nullptr));
            swapchain.images.assign(imageCount, XrSwapchainImageOpenGLESKHR{XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
            XR_CHECK(xrEnumerateSwapchainImages(
                swapchain.swapchain,
                imageCount,
                &imageCount,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchain.images.data())));
        }
        return true;
    }

    bool createClockSwapchain() {
        uint32_t formatCount = 0;
        if (XR_FAILED(xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr)) ||
            formatCount == 0) {
            return false;
        }
        std::vector<int64_t> formats(formatCount);
        if (XR_FAILED(xrEnumerateSwapchainFormats(
                session, formatCount, &formatCount, formats.data()))) {
            return false;
        }
        int64_t selectedFormat = 0;
        for (int64_t format : formats) {
            if (format == GL_RGBA8) {
                selectedFormat = format;
                break;
            }
        }
        if (selectedFormat == 0) {
            LOGW("No RGBA8 OpenXR swapchain format for transparent activity overlay");
            return false;
        }

        XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        createInfo.format = selectedFormat;
        createInfo.sampleCount = 1;
        createInfo.width = clockSwapchain.width;
        createInfo.height = clockSwapchain.height;
        createInfo.faceCount = 1;
        createInfo.arraySize = 1;
        createInfo.mipCount = 1;
        if (xrFailed(
                xrCreateSwapchain(session, &createInfo, &clockSwapchain.swapchain),
                "xrCreateSwapchain(clock quad)")) {
            return false;
        }

        uint32_t imageCount = 0;
        if (xrFailed(
                xrEnumerateSwapchainImages(
                    clockSwapchain.swapchain, 0, &imageCount, nullptr),
                "xrEnumerateSwapchainImages(clock count)")) {
            return false;
        }
        clockSwapchain.images.assign(
            imageCount,
            XrSwapchainImageOpenGLESKHR{XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        if (xrFailed(
                xrEnumerateSwapchainImages(
                    clockSwapchain.swapchain,
                    imageCount,
                    &imageCount,
                    reinterpret_cast<XrSwapchainImageBaseHeader*>(
                        clockSwapchain.images.data())),
                "xrEnumerateSwapchainImages(clock)")) {
            return false;
        }
        return true;
    }

    bool createOverlayResources() {
        if (clockSwapchain.swapchain == XR_NULL_HANDLE) return false;
        const char* vertexSource = R"(#version 300 es
layout(location = 0) in vec3 aPosition;
void main() {
    gl_Position = vec4(aPosition, 1.0);
})";
        const char* fragmentSource = R"(#version 300 es
precision mediump float;
uniform vec4 uColor;
out vec4 fragColor;
void main() {
    fragColor = vec4(uColor.rgb * uColor.a, uColor.a);
})";
        GLuint vs = compileShader(GL_VERTEX_SHADER, vertexSource);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
        if (vs == 0 || fs == 0) {
            if (vs != 0) glDeleteShader(vs);
            if (fs != 0) glDeleteShader(fs);
            return false;
        }
        overlayProgram = glCreateProgram();
        glAttachShader(overlayProgram, vs);
        glAttachShader(overlayProgram, fs);
        glLinkProgram(overlayProgram);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint linked = GL_FALSE;
        glGetProgramiv(overlayProgram, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            char log[512] = {};
            glGetProgramInfoLog(overlayProgram, sizeof(log), nullptr, log);
            LOGE("Activity overlay program link failed: %s", log);
            glDeleteProgram(overlayProgram);
            overlayProgram = 0;
            return false;
        }
        overlayColorUniform = glGetUniformLocation(overlayProgram, "uColor");
        glGenBuffers(1, &overlayVbo);
        return overlayColorUniform >= 0;
    }

    static void appendQuad(
        std::vector<float>& vertices,
        float left,
        float bottom,
        float right,
        float top,
        float z) {
        const float quad[] = {
            left, bottom, z, right, bottom, z, right, top, z,
            left, bottom, z, right, top, z, left, top, z,
        };
        vertices.insert(vertices.end(), quad, quad + 18);
    }

    void drawOverlayGeometry(
        const std::vector<float>& vertices,
        float red,
        float green,
        float blue,
        float alpha) {
        if (vertices.empty()) return;
        glBindBuffer(GL_ARRAY_BUFFER, overlayVbo);
        glBufferData(
            GL_ARRAY_BUFFER,
            static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
            vertices.data(),
            GL_STREAM_DRAW);
        glUniform4f(overlayColorUniform, red, green, blue, alpha);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size() / 3));
        glDisableVertexAttribArray(0);
    }

    static void appendDigit(
        std::vector<float>& vertices,
        int digit,
        float x,
        float y,
        float z) {
        static constexpr uint8_t masks[10] = {
            0x3f, 0x06, 0x5b, 0x4f, 0x66,
            0x6d, 0x7d, 0x07, 0x7f, 0x6f,
        };
        constexpr float w = 0.32f;
        constexpr float h = 0.90f;
        constexpr float t = 0.055f;
        const float middle = y + h * 0.5f;
        const uint8_t mask = masks[digit < 0 || digit > 9 ? 0 : digit];
        if (mask & 0x01) appendQuad(vertices, x + t, y + h - t, x + w - t, y + h, z);
        if (mask & 0x02) appendQuad(vertices, x + w - t, middle + t * 0.5f, x + w, y + h - t, z);
        if (mask & 0x04) appendQuad(vertices, x + w - t, y + t, x + w, middle - t * 0.5f, z);
        if (mask & 0x08) appendQuad(vertices, x + t, y, x + w - t, y + t, z);
        if (mask & 0x10) appendQuad(vertices, x, y + t, x + t, middle - t * 0.5f, z);
        if (mask & 0x20) appendQuad(vertices, x, middle + t * 0.5f, x + t, y + h - t, z);
        if (mask & 0x40) appendQuad(vertices, x + t, middle - t * 0.5f, x + w - t, middle + t * 0.5f, z);
    }

    bool updateClockTexture() {
        const std::time_t now = std::time(nullptr);
        const int64_t minute = static_cast<int64_t>(now / 60);
        if (renderedClockMinute == minute) return true;

        uint32_t imageIndex = 0;
        XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (xrFailed(
                xrAcquireSwapchainImage(clockSwapchain.swapchain, &acquireInfo, &imageIndex),
                "xrAcquireSwapchainImage(clock)")) {
            return false;
        }
        XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        waitInfo.timeout = XR_INFINITE_DURATION;
        if (xrFailed(
                xrWaitSwapchainImage(clockSwapchain.swapchain, &waitInfo),
                "xrWaitSwapchainImage(clock)")) {
            XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(clockSwapchain.swapchain, &releaseInfo);
            return false;
        }

        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(
            GL_FRAMEBUFFER,
            GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D,
            clockSwapchain.images[imageIndex].image,
            0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            LOGW("Clock quad framebuffer incomplete");
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(clockSwapchain.swapchain, &releaseInfo);
            return false;
        }

        glViewport(0, 0, clockSwapchain.width, clockSwapchain.height);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(overlayProgram);

        std::vector<float> panel;
        panel.reserve(18);
        appendQuad(panel, -0.90f, -0.56f, 0.90f, 0.56f, 0.0f);
        drawOverlayGeometry(panel, 0.01f, 0.025f, 0.035f, 0.78f);

        std::tm local{};
        localtime_r(&now, &local);
        const int digits[4] = {
            local.tm_hour / 10,
            local.tm_hour % 10,
            local.tm_min / 10,
            local.tm_min % 10,
        };
        constexpr float digitW = 0.32f;
        constexpr float digitH = 0.90f;
        constexpr float gap = 0.055f;
        constexpr float colonW = 0.10f;
        constexpr float totalW = digitW * 4.0f + gap * 4.0f + colonW;
        const float startX = -totalW * 0.5f;
        const float startY = -digitH * 0.5f;
        std::vector<float> text;
        text.reserve(4 * 7 * 18 + 36);
        float x = startX;
        appendDigit(text, digits[0], x, startY, 0.0f);
        x += digitW + gap;
        appendDigit(text, digits[1], x, startY, 0.0f);
        x += digitW + gap;
        appendQuad(text, x + 0.025f, startY + 0.25f, x + 0.075f, startY + 0.32f, 0.0f);
        appendQuad(text, x + 0.025f, startY + 0.58f, x + 0.075f, startY + 0.65f, 0.0f);
        x += colonW + gap;
        appendDigit(text, digits[2], x, startY, 0.0f);
        x += digitW + gap;
        appendDigit(text, digits[3], x, startY, 0.0f);
        drawOverlayGeometry(text, 0.72f, 0.94f, 1.0f, 0.98f);

        glDisable(GL_BLEND);
        glUseProgram(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glFlush();

        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(clockSwapchain.swapchain, &releaseInfo);
        renderedClockMinute = minute;
        return true;
    }

    bool renderActivityOverlay(
        bool viewsValid,
        uint32_t viewCount,
        XrCompositionLayerQuad& clockLayer) {
        if (!overlayReady || !clockEnabled || clockSwapchain.swapchain == XR_NULL_HANDLE ||
            !viewsValid || viewCount == 0 ||
            !handInputs[0].active || !handInputs[0].gripValid ||
            !updateClockTexture()) {
            return false;
        }

        clockLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        clockLayer.space = appSpace;
        clockLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        clockLayer.subImage.swapchain = clockSwapchain.swapchain;
        clockLayer.subImage.imageRect.offset = {0, 0};
        clockLayer.subImage.imageRect.extent = {
            clockSwapchain.width, clockSwapchain.height};
        clockLayer.pose.orientation = views[0].pose.orientation;
        clockLayer.pose.position = handInputs[0].grip.position;
        clockLayer.pose.position.y += 0.22f;
        clockLayer.size = {0.22f, 0.09f};
        return true;
    }

    void notifySurfaceDestroyed(JNIEnv* env) {
        if (env == nullptr) return;
        jclass activityClass = env->GetObjectClass(activityRef);
        if (activityClass == nullptr) return;
        jmethodID method = env->GetMethodID(
            activityClass,
            "onNativeXrSurfaceDestroyed",
            "()V");
        env->DeleteLocalRef(activityClass);
        if (method == nullptr) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            LOGE("QuestVrActivity.onNativeXrSurfaceDestroyed not found");
            return;
        }
        env->CallVoidMethod(activityRef, method);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            LOGE("QuestVrActivity.onNativeXrSurfaceDestroyed threw");
        }
    }

    void pollEvents() {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(instance, &event) == XR_SUCCESS) {
            switch (event.type) {
                case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                    auto* changed = reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
                    sessionState = changed->state;
                    handleSessionState();
                    break;
                }
                case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                    running = false;
                    break;
                default:
                    break;
            }
            event = {XR_TYPE_EVENT_DATA_BUFFER};
        }
    }

    void handleSessionState() {
        switch (sessionState) {
            case XR_SESSION_STATE_READY: {
                XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
                beginInfo.primaryViewConfigurationType = kViewType;
                if (!xrFailed(xrBeginSession(session, &beginInfo), "xrBeginSession")) {
                    sessionRunning = true;
                }
                break;
            }
            case XR_SESSION_STATE_STOPPING:
                sessionRunning = false;
                xrEndSession(session);
                break;
            case XR_SESSION_STATE_EXITING:
            case XR_SESSION_STATE_LOSS_PENDING:
                running = false;
                break;
            default:
                break;
        }
    }

    uint32_t locateFrameViews(XrTime displayTime, bool& orientationValid) {
        orientationValid = false;
        if (appSpace == XR_NULL_HANDLE || views.empty()) return 0;
        XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
        locateInfo.viewConfigurationType = kViewType;
        locateInfo.displayTime = displayTime;
        locateInfo.space = appSpace;
        XrViewState viewState{XR_TYPE_VIEW_STATE};
        uint32_t viewCount = static_cast<uint32_t>(views.size());
        const XrResult result = xrLocateViews(
            session, &locateInfo, &viewState, viewCount, &viewCount, views.data());
        if (XR_FAILED(result)) return 0;
        orientationValid =
            (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
        return viewCount;
    }

    void renderFrame(JNIEnv* env) {
        XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frameState{XR_TYPE_FRAME_STATE};
        if (xrFailed(xrWaitFrame(session, &waitInfo, &frameState), "xrWaitFrame")) return;

        XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
        if (xrFailed(xrBeginFrame(session, &beginInfo), "xrBeginFrame")) return;

        bool viewsValid = false;
        const uint32_t viewCount = locateFrameViews(
            frameState.predictedDisplayTime, viewsValid);

        XrCompositionLayerQuad quadLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerQuad clockLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerProjection projectionLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        const XrCompositionLayerBaseHeader* submittedLayer = nullptr;
        std::array<const XrCompositionLayerBaseHeader*, 2> submittedLayers{};
        uint32_t submittedLayerCount = 0;

        syncInput(frameState.predictedDisplayTime);
        // Publish timing only after controller state is current. WAIT_FRAME on the
        // Windows side wakes on this callback, so this ordering avoids returning the
        // previous headset frame's input to the game.
        publishFrameState(frameState, viewCount);

        const bool stereoRendered = frameState.shouldRender && viewsValid &&
            renderStereoProjection(viewCount, projectionLayer, submittedLayer);
        if (stereoRendered) {
            // True per-eye stereo from the game's AHardwareBuffers; submittedLayer is set.
            if (stereoProjectionAnnounced) {
                if (!stereoMilestonePublished) {
                    publishDiagnostic(env, "Stereo projection active: game images displayed");
                    stereoMilestonePublished = true;
                }
            }
        } else if (frameState.shouldRender && theaterScreenEnabled &&
                   screenSwapchain.swapchain != XR_NULL_HANDLE) {
            renderScreenTexture();
            quadLayer.space = appSpace;
            quadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quadLayer.subImage.swapchain = screenSwapchain.swapchain;
            quadLayer.subImage.imageRect.offset = {0, 0};
            quadLayer.subImage.imageRect.extent = {screenSwapchain.width, screenSwapchain.height};
            quadLayer.pose.orientation.w = 1.0f;
            quadLayer.pose.position.x = 0.0f;
            quadLayer.pose.position.y = usingStageSpace ? 1.35f : 0.0f;
            quadLayer.pose.position.z = -2.2f;
            quadLayer.size.width = 2.6f;
            quadLayer.size.height = 2.6f * 9.0f / 16.0f;
            submittedLayer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quadLayer);
        } else if (frameState.shouldRender && theaterScreenEnabled && viewsValid &&
                   !eyeSwapchains.empty()) {
            const uint32_t fallbackViewCount = std::min(
                viewCount, static_cast<uint32_t>(eyeSwapchains.size()));
            if (fallbackViewCount > 0) {
                for (uint32_t eye = 0; eye < fallbackViewCount; ++eye) {
                    compositionViews[eye] = {
                        XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                    renderEye(eye);
                    compositionViews[eye].pose = views[eye].pose;
                    compositionViews[eye].fov = views[eye].fov;
                    compositionViews[eye].subImage.swapchain = eyeSwapchains[eye].swapchain;
                    compositionViews[eye].subImage.imageRect.offset = {0, 0};
                    compositionViews[eye].subImage.imageRect.extent = {
                        eyeSwapchains[eye].width, eyeSwapchains[eye].height};
                }

                projectionLayer.space = appSpace;
                projectionLayer.viewCount = fallbackViewCount;
                projectionLayer.views = compositionViews.data();
                submittedLayer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projectionLayer);
            }
        }

        if (submittedLayer != nullptr) submittedLayers[submittedLayerCount++] = submittedLayer;
        if (frameState.shouldRender &&
            renderActivityOverlay(viewsValid, viewCount, clockLayer)) {
            submittedLayers[submittedLayerCount++] =
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&clockLayer);
        }

        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = blendMode;
        endInfo.layerCount = submittedLayerCount;
        endInfo.layers = submittedLayerCount == 0 ? nullptr : submittedLayers.data();
        xrEndFrame(session, &endInfo);
    }

    // Publish predicted timing + full per-eye poses/FOV to the activity, which forwards
    // them to XrBridgeServer so the Windows game's xrWaitFrame/xrLocateViews are backed
    // by real Quest tracking data.
    void publishFrameState(const XrFrameState& frameState, uint32_t viewCount) {
        if (activityFrameStateMethod == nullptr || trackingArrayRef == nullptr ||
            appSpace == XR_NULL_HANDLE || views.empty()) {
            return;
        }

        if (viewCount == 0) return;

        JNIEnv* env = nullptr;
        if (javaVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr) {
            return;
        }

        float data[24] = {};
        for (uint32_t eye = 0; eye < 2; ++eye) {
            const XrView& view = views[eye < viewCount ? eye : viewCount - 1];
            float* d = data + eye * 11;
            d[0] = view.pose.orientation.x;
            d[1] = view.pose.orientation.y;
            d[2] = view.pose.orientation.z;
            d[3] = view.pose.orientation.w;
            d[4] = view.pose.position.x;
            d[5] = view.pose.position.y;
            d[6] = view.pose.position.z;
            d[7] = view.fov.angleLeft;
            d[8] = view.fov.angleRight;
            d[9] = view.fov.angleUp;
            d[10] = view.fov.angleDown;
        }
        data[22] = stageBounds.width;
        data[23] = stageBounds.height;
        env->SetFloatArrayRegion(trackingArrayRef, 0, 24, data);
        env->CallVoidMethod(
            activityRef,
            activityFrameStateMethod,
            static_cast<jlong>(frameState.predictedDisplayTime),
            static_cast<jlong>(frameState.predictedDisplayPeriod),
            static_cast<jboolean>(frameState.shouldRender),
            static_cast<jint>(sessionState),
            trackingArrayRef);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
    }

    void renderScreenTexture() {
        if (surfaceTextureRef == nullptr || surfaceTextureUpdateTexImage == nullptr || blitProgram == 0) {
            return;
        }

        JNIEnv* env = nullptr;
        if (javaVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr) {
            return;
        }
        env->CallVoidMethod(surfaceTextureRef, surfaceTextureUpdateTexImage);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            return;
        }

        uint32_t imageIndex = 0;
        XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (xrFailed(xrAcquireSwapchainImage(screenSwapchain.swapchain, &acquireInfo, &imageIndex), "xrAcquireSwapchainImage(screen)")) return;

        XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        waitInfo.timeout = XR_INFINITE_DURATION;
        if (xrFailed(xrWaitSwapchainImage(screenSwapchain.swapchain, &waitInfo), "xrWaitSwapchainImage(screen)")) return;

        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(
            GL_FRAMEBUFFER,
            GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D,
            screenSwapchain.images[imageIndex].image,
            0);

        glViewport(0, 0, screenSwapchain.width, screenSwapchain.height);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glUseProgram(blitProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, externalTexture);
        glBindVertexArray(blitVao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
        glUseProgram(0);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glFlush();

        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(screenSwapchain.swapchain, &releaseInfo);
    }

    void renderEye(uint32_t eye) {
        auto& swapchain = eyeSwapchains[eye];
        uint32_t imageIndex = 0;
        XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (xrFailed(xrAcquireSwapchainImage(swapchain.swapchain, &acquireInfo, &imageIndex), "xrAcquireSwapchainImage")) return;

        XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        waitInfo.timeout = XR_INFINITE_DURATION;
        if (xrFailed(xrWaitSwapchainImage(swapchain.swapchain, &waitInfo), "xrWaitSwapchainImage")) return;

        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(
            GL_FRAMEBUFFER,
            GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D,
            swapchain.images[imageIndex].image,
            0);

        glViewport(0, 0, swapchain.width, swapchain.height);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.005f, 0.006f, 0.008f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        drawTheaterScreen(swapchain.width, swapchain.height);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glFlush();

        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(swapchain.swapchain, &releaseInfo);
    }

    void clearRect(int x, int y, int width, int height, float r, float g, float b) {
        glScissor(x, y, width, height);
        glClearColor(r, g, b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    void drawTheaterScreen(int width, int height) {
        const int screenW = static_cast<int>(width * 0.72f);
        const int screenH = static_cast<int>(screenW * 9.0f / 16.0f);
        const int x = (width - screenW) / 2;
        const int y = (height - screenH) / 2;
        const int border = width > 1800 ? 8 : 5;

        glEnable(GL_SCISSOR_TEST);
        clearRect(x - border, y - border, screenW + border * 2, screenH + border * 2, 0.02f, 0.18f, 0.22f);
        clearRect(x, y, screenW, screenH, 0.015f, 0.017f, 0.022f);
        clearRect(x + border * 3, y + border * 3, screenW - border * 6, screenH - border * 6, 0.035f, 0.038f, 0.045f);
        glDisable(GL_SCISSOR_TEST);
    }

    void cleanup(JNIEnv* env) {
        gamenative::xr::stopTestProducer();
        frameTransport.stop();
        notifySurfaceDestroyed(env);
        if (sessionRunning && session != XR_NULL_HANDLE) {
            xrEndSession(session);
            sessionRunning = false;
        }
        if (surfaceTextureRef != nullptr && env != nullptr) {
            env->DeleteGlobalRef(surfaceTextureRef);
            surfaceTextureRef = nullptr;
            surfaceTextureUpdateTexImage = nullptr;
        }
        if (screenSwapchain.swapchain != XR_NULL_HANDLE) {
            xrDestroySwapchain(screenSwapchain.swapchain);
            screenSwapchain.swapchain = XR_NULL_HANDLE;
        }
        screenSwapchain.images.clear();
        for (auto& swapchain : eyeSwapchains) {
            if (swapchain.swapchain != XR_NULL_HANDLE) {
                xrDestroySwapchain(swapchain.swapchain);
                swapchain.swapchain = XR_NULL_HANDLE;
            }
        }
        if (clockSwapchain.swapchain != XR_NULL_HANDLE) {
            xrDestroySwapchain(clockSwapchain.swapchain);
            clockSwapchain.swapchain = XR_NULL_HANDLE;
        }
        clockSwapchain.images.clear();
        overlayReady = false;
        for (int hand = 0; hand < 2; ++hand) {
            if (gripSpaces[hand] != XR_NULL_HANDLE) {
                xrDestroySpace(gripSpaces[hand]);
                gripSpaces[hand] = XR_NULL_HANDLE;
            }
            if (aimSpaces[hand] != XR_NULL_HANDLE) {
                xrDestroySpace(aimSpaces[hand]);
                aimSpaces[hand] = XR_NULL_HANDLE;
            }
        }
        if (actionSet != XR_NULL_HANDLE) {
            xrDestroyActionSet(actionSet);  // destroys the actions it owns
            actionSet = XR_NULL_HANDLE;
        }
        inputReady = false;
        if (env != nullptr) {
            if (trackingArrayRef != nullptr) {
                env->DeleteGlobalRef(trackingArrayRef);
                trackingArrayRef = nullptr;
            }
            if (inputArrayRef != nullptr) {
                env->DeleteGlobalRef(inputArrayRef);
                inputArrayRef = nullptr;
            }
        }
        if (appSpace != XR_NULL_HANDLE) {
            xrDestroySpace(appSpace);
            appSpace = XR_NULL_HANDLE;
        }
        if (session != XR_NULL_HANDLE) {
            xrDestroySession(session);
            session = XR_NULL_HANDLE;
        }
        if (instance != XR_NULL_HANDLE) {
            xrDestroyInstance(instance);
            instance = XR_NULL_HANDLE;
        }
        if (framebuffer != 0) {
            glDeleteFramebuffers(1, &framebuffer);
            framebuffer = 0;
        }
        if (blitVbo != 0) {
            glDeleteBuffers(1, &blitVbo);
            blitVbo = 0;
        }
        if (blitVao != 0) {
            glDeleteVertexArrays(1, &blitVao);
            blitVao = 0;
        }
        if (blitProgram != 0) {
            glDeleteProgram(blitProgram);
            blitProgram = 0;
        }
        if (externalTexture != 0) {
            glDeleteTextures(1, &externalTexture);
            externalTexture = 0;
        }
        for (int eye = 0; eye < gamenative::xr::FrameTransport::kEyeCount; ++eye) {
            for (int image = 0;
                 image < gamenative::xr::FrameTransport::kMaxImages; ++image) {
                if (eyeImages[eye][image] != EGL_NO_IMAGE_KHR &&
                    eglDisplay != EGL_NO_DISPLAY) {
                    eglDestroyImageKHR(eglDisplay, eyeImages[eye][image]);
                    eyeImages[eye][image] = EGL_NO_IMAGE_KHR;
                }
                if (eyeTextures[eye][image] != 0) {
                    glDeleteTextures(1, &eyeTextures[eye][image]);
                    eyeTextures[eye][image] = 0;
                }
                if (eyeCpuMappings[eye][image] != nullptr) {
                    ::munmap(
                        eyeCpuMappings[eye][image],
                        eyeCpuMappingLengths[eye][image]);
                    eyeCpuMappings[eye][image] = nullptr;
                    eyeCpuMappingLengths[eye][image] = 0;
                    eyeCpuMappingRegistrations[eye][image] = 0;
                }
            }
        }
        if (blit2DVbo != 0) {
            glDeleteBuffers(1, &blit2DVbo);
            blit2DVbo = 0;
        }
        if (blit2DVao != 0) {
            glDeleteVertexArrays(1, &blit2DVao);
            blit2DVao = 0;
        }
        if (blit2DProgram != 0) {
            glDeleteProgram(blit2DProgram);
            blit2DProgram = 0;
        }
        if (overlayVbo != 0) {
            glDeleteBuffers(1, &overlayVbo);
            overlayVbo = 0;
        }
        if (overlayProgram != 0) {
            glDeleteProgram(overlayProgram);
            overlayProgram = 0;
        }
        if (eglDisplay != EGL_NO_DISPLAY) {
            eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (eglSurface != EGL_NO_SURFACE) eglDestroySurface(eglDisplay, eglSurface);
            if (eglContext != EGL_NO_CONTEXT) eglDestroyContext(eglDisplay, eglContext);
            eglTerminate(eglDisplay);
        }
        eglDisplay = EGL_NO_DISPLAY;
        eglSurface = EGL_NO_SURFACE;
        eglContext = EGL_NO_CONTEXT;
    }
};
} // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_app_gamenative_xr_QuestVrActivity_nativeStart(
    JNIEnv* env,
    jobject activity,
    jint renderScalePercent,
    jboolean theaterScreenEnabled,
    jboolean clockEnabled) {
    JavaVM* vm = nullptr;
    if (env->GetJavaVM(&vm) != JNI_OK || vm == nullptr) {
        LOGE("GetJavaVM failed");
        return 0;
    }
    jobject activityRef = env->NewGlobalRef(activity);
    auto* app = new QuestXrApp(
        vm,
        activityRef,
        static_cast<int>(renderScalePercent),
        theaterScreenEnabled == JNI_TRUE,
        clockEnabled == JNI_TRUE);
    app->start();
    return reinterpret_cast<jlong>(app);
}

extern "C" JNIEXPORT void JNICALL
Java_app_gamenative_xr_QuestVrActivity_nativeStop(JNIEnv*, jobject, jlong handle) {
    auto* app = reinterpret_cast<QuestXrApp*>(handle);
    delete app;
}

extern "C" JNIEXPORT void JNICALL
Java_app_gamenative_xr_QuestVrActivity_nativeHaptic(
    JNIEnv*, jobject, jlong handle, jint hand, jfloat amplitude, jlong durationNs, jfloat frequency) {
    auto* app = reinterpret_cast<QuestXrApp*>(handle);
    if (app != nullptr) {
        app->queueHaptic(hand, amplitude, durationNs, frequency);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_app_gamenative_xr_QuestVrActivity_nativeRequestExit(
    JNIEnv*, jobject, jlong handle) {
    auto* app = reinterpret_cast<QuestXrApp*>(handle);
    if (app != nullptr) {
        app->requestExit();
    }
}
