#include "xr_test_producer.h"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#define LOG_TAG "GameNativeVR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace gamenative::xr {
namespace {

std::atomic<bool> g_running{false};
std::thread g_thread;

bool readLine(int fd, std::string& out) {
    out.clear();
    char ch = 0;
    while (true) {
        ssize_t got = recv(fd, &ch, 1, 0);
        if (got == 0) return false;
        if (got < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (ch == '\n') return true;
        if (out.size() < 512) out.push_back(ch);
    }
}

bool writeAll(int fd, const std::string& s) {
    size_t sent = 0;
    while (sent < s.size()) {
        ssize_t n = send(fd, s.data() + sent, s.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool expectOk(int fd) {
    std::string line;
    return readLine(fd, line) && line.rfind("OK", 0) == 0;
}

AHardwareBuffer* allocateEye(int width, int height, uint8_t r, uint8_t g, uint8_t b) {
    AHardwareBuffer_Desc desc{};
    desc.width = static_cast<uint32_t>(width);
    desc.height = static_cast<uint32_t>(height);
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;

    AHardwareBuffer* buf = nullptr;
    if (AHardwareBuffer_allocate(&desc, &buf) != 0 || buf == nullptr) {
        LOGE("test producer: AHardwareBuffer_allocate failed");
        return nullptr;
    }

    void* addr = nullptr;
    if (AHardwareBuffer_lock(buf, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &addr) != 0 || addr == nullptr) {
        LOGW("test producer: lock failed; buffer left uninitialized");
        return buf;
    }
    AHardwareBuffer_Desc actual{};
    AHardwareBuffer_describe(buf, &actual);
    const uint32_t strideBytes = actual.stride * 4;  // stride is in pixels for RGBA8
    auto* base = static_cast<uint8_t*>(addr);
    for (uint32_t y = 0; y < actual.height; ++y) {
        uint8_t* row = base + static_cast<size_t>(y) * strideBytes;
        // Vertical gradient so orientation/flip is obvious in-headset.
        const uint8_t shade = static_cast<uint8_t>(40 + (y * 180) / actual.height);
        for (uint32_t x = 0; x < actual.width; ++x) {
            row[x * 4 + 0] = static_cast<uint8_t>((r * shade) / 255);
            row[x * 4 + 1] = static_cast<uint8_t>((g * shade) / 255);
            row[x * 4 + 2] = static_cast<uint8_t>((b * shade) / 255);
            row[x * 4 + 3] = 255;
        }
    }
    AHardwareBuffer_unlock(buf, nullptr);
    return buf;
}

// Mirrors xr_transport's address handling: leading '@' = abstract namespace.
socklen_t fillUnixAddr(sockaddr_un& addr, const std::string& path) {
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (!path.empty() && path[0] == '@') {
        size_t nameLen = path.size() - 1;
        if (nameLen > sizeof(addr.sun_path) - 1) nameLen = sizeof(addr.sun_path) - 1;
        addr.sun_path[0] = '\0';
        std::memcpy(addr.sun_path + 1, path.data() + 1, nameLen);
        return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + nameLen);
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    return static_cast<socklen_t>(sizeof(addr));
}

int connectWithRetry(const std::string& socketPath) {
    for (int attempt = 0; attempt < 50 && g_running.load(); ++attempt) {
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        sockaddr_un addr{};
        socklen_t addrLen = fillUnixAddr(addr, socketPath);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), addrLen) == 0) {
            return fd;
        }
        ::close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return -1;
}

bool shareEye(int fd, int eye, AHardwareBuffer* buf, int width, int height) {
    char line[128];
    int n = std::snprintf(line, sizeof(line), "BUFFER eye=%d index=0 w=%d h=%d\n", eye, width, height);
    if (!writeAll(fd, std::string(line, n))) return false;
    if (!expectOk(fd)) return false;  // consumer ready, send the handle now
    if (AHardwareBuffer_sendHandleToUnixSocket(buf, fd) != 0) {
        LOGE("test producer: sendHandleToUnixSocket failed for eye %d", eye);
        return false;
    }
    return expectOk(fd);  // "OK stored"
}

void run(std::string socketPath, int width, int height) {
    AHardwareBuffer* left = allocateEye(width, height, 220, 40, 40);   // reddish
    AHardwareBuffer* right = allocateEye(width, height, 40, 80, 230);  // bluish
    if (left == nullptr || right == nullptr) {
        if (left) AHardwareBuffer_release(left);
        if (right) AHardwareBuffer_release(right);
        return;
    }

    int fd = connectWithRetry(socketPath);
    if (fd < 0) {
        LOGE("test producer: could not connect to %s", socketPath.c_str());
        AHardwareBuffer_release(left);
        AHardwareBuffer_release(right);
        return;
    }

    bool ok = writeAll(fd, "HELLO\n") && expectOk(fd) &&
              shareEye(fd, 0, left, width, height) &&
              shareEye(fd, 1, right, width, height);
    if (!ok) {
        LOGE("test producer: handshake/share failed");
    } else {
        LOGI("test producer: shared both eye buffers, presenting test pattern");
        while (g_running.load()) {
            if (!writeAll(fd, "FRAME eye=0 index=0\n") || !expectOk(fd)) break;
            if (!writeAll(fd, "FRAME eye=1 index=0\n") || !expectOk(fd)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(13));  // ~72 Hz
        }
    }

    writeAll(fd, "BYE\n");
    ::close(fd);
    AHardwareBuffer_release(left);
    AHardwareBuffer_release(right);
    LOGI("test producer: stopped");
}

}  // namespace

void startTestProducer(const std::string& socketPath, int width, int height) {
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true)) return;
    g_thread = std::thread(run, socketPath, width, height);
}

void stopTestProducer() {
    if (!g_running.exchange(false)) return;
    if (g_thread.joinable()) g_thread.join();
}

}  // namespace gamenative::xr
