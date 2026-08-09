// GameNativeVR — zero-copy eye-buffer transport (Quest-compositor / consumer side).
//
// This is the data-plane half of the "true stereo" bridge described in
// docs/xr/true-stereo-bridge-design.md (milestone M1).
//
// The Wine Unix producer allocates swapchain images once and prefers one importable
// AHardwareBuffer per eye/image slot. It exchanges only image indices, sub-rects,
// and native fences per frame. dma-buf remains the compatibility fallback.
//
// This module owns the AF_UNIX listener and the imported AHardwareBuffer slots. It is
// deliberately decoupled from GL: receiving a handle needs no GL context, so it runs on
// its own thread. The render thread (quest_xr) imports the latest buffer into a texture
// itself, keeping all GL work on the GL thread.
//
// Until a producer connects, pollEye() returns an empty frame and quest_xr falls
// back to its flat-surface path.

#pragma once

#include <android/hardware_buffer.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace gamenative::xr {

// How an eye's GPU buffer was shared. HardwareBuffer is the preferred Android path;
// DmaBuf is retained for Vulkan stacks which cannot import AHardwareBuffer memory.
enum class BufferKind { None, HardwareBuffer, DmaBuf };

// A single eye's most-recently-presented buffer. All handles are owned by the transport;
// callers (the render thread) only read them and must NOT release/close them. `acquireFenceFd`
// is -1 when no fence was provided (see design doc §6 for the sync plan).
struct EyeFrame {
    static constexpr int kMaxPlanes = 4;
    BufferKind kind{BufferKind::None};

    // kind == HardwareBuffer: AHardwareBuffer_acquire'd on receipt.
    AHardwareBuffer* buffer{nullptr};
    // The Vulkan producer may copy a BGRA swapchain into the only portable Android
    // RGBA hardware-buffer format. In that case GL samples with R/B swizzled.
    bool swapRedBlue{false};

    // kind == DmaBuf: one to four dma-buf planes + DRM layout. Fds stay open for the
    // registered buffer's lifetime (EGL duplicates them on import).
    int planeCount{0};
    int dmabufFds[kMaxPlanes]{-1, -1, -1, -1};
    uint32_t fourcc{0};     // DRM FourCC (e.g. DRM_FORMAT_ABGR8888)
    uint32_t strides[kMaxPlanes]{0, 0, 0, 0}; // bytes
    uint32_t offsets[kMaxPlanes]{0, 0, 0, 0}; // bytes
    uint64_t modifier{0};   // DRM format modifier (DRM_FORMAT_MOD_LINEAR == 0)

    int32_t width{0};
    int32_t height{0};
    // OpenXR sub-image crop in top-left pixel coordinates. Zero dimensions mean
    // the full registered image for backward-compatible test producers.
    int32_t sourceX{0};
    int32_t sourceY{0};
    int32_t sourceWidth{0};
    int32_t sourceHeight{0};
    bool projectionValid{false};
    float projectionOrientation[4]{0.0f, 0.0f, 0.0f, 1.0f};
    float projectionPosition[3]{0.0f, 0.0f, 0.0f};
    float projectionFov[4]{-0.75f, 0.75f, 0.75f, -0.75f};
    int32_t imageIndex{0};
    int32_t acquireFenceFd{-1};
    uint64_t registrationSerial{0}; // Changes only when a slot is (re)registered.
    uint64_t serial{0};  // increments each time the producer presents this eye
};

class FrameTransport {
public:
    static constexpr int kEyeCount = 2;
    static constexpr int kMaxImages = 128;

    FrameTransport();
    ~FrameTransport();

    FrameTransport(const FrameTransport&) = delete;
    FrameTransport& operator=(const FrameTransport&) = delete;

    // Begin listening on `socketPath` (filesystem AF_UNIX path, e.g. /tmp/gamenative-xr.sock).
    // Safe to call once; subsequent calls are ignored. Never blocks.
    void start(const std::string& socketPath);

    // Stop the listener, drop the client, release any held buffers.
    void stop();

    // Snapshot the latest buffer for `eye` (0 = left, 1 = right). The returned EyeFrame's
    // `buffer` remains owned by the transport and stays valid until the next pollEye() for
    // the same eye or until stop(). Returns a frame with buffer==nullptr if nothing has
    // arrived yet. Cheap; intended to be called once per eye per rendered frame.
    EyeFrame pollEye(int eye);

    // Called by the compositor thread after all GL sampling commands for an input
    // image have been queued. Takes ownership of releaseFenceFd.
    void publishReleaseFence(int eye, int imageIndex, int releaseFenceFd);

    // Drop a claimed presentation that could not be imported or sampled. Since no
    // consumer GPU work references it, the producer may reuse the image immediately.
    void discardFrame(int eye, int imageIndex, uint64_t serial);

    // True once a producer has connected and presented at least one frame for both eyes.
    bool hasStereoContent() const;

private:
    void acceptLoop();
    void serviceClient(int clientFd);
    bool handleBufferLine(int clientFd, const std::string& line);
    bool handleDmabufLine(int clientFd, const std::string& line);
    bool handleFrameLine(int clientFd, const std::string& line);
    bool handleAcquireLine(int clientFd, const std::string& line);
    void storeEyeBuffer(int eye, AHardwareBuffer* ahb, int32_t w, int32_t h,
                        int32_t index, bool swapRedBlue);
    void storeEyeDmabuf(int eye, const EyeFrame& incoming);
    void releaseSlotLocked(int eye, int imageIndex);  // caller holds eyesMutex_
    void releaseEye(int eye);

    std::string socketPath_;
    std::atomic<bool> running_{false};
    std::atomic<int> listenFd_{-1};
    std::atomic<int> clientFd_{-1};
    std::thread acceptThread_;

    std::mutex eyesMutex_;
    std::condition_variable releaseCv_;
    EyeFrame buffers_[kEyeCount][kMaxImages];
    EyeFrame latest_[kEyeCount];
    bool latestClaimed_[kEyeCount]{false, false};
    int releaseFenceFds_[kEyeCount][kMaxImages];
    bool releasePending_[kEyeCount][kMaxImages]{};
    uint64_t nextSerial_{1};
};

}  // namespace gamenative::xr
