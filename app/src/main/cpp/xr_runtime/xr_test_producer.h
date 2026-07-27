// GameNativeVR — test producer for the zero-copy eye-buffer transport.
//
// This is a stand-in for the real Wine unix-side producer (gamenative_openxr.so). It
// allocates two AHardwareBuffers (one per eye), writes a static per-eye test pattern, and
// drives the AF_UNIX protocol exactly as the real producer will: share each handle ONCE via
// AHardwareBuffer_sendHandleToUnixSocket, then emit a tiny FRAME line per frame.
//
// Purpose: validate the consumer (xr_transport) + quest_xr stereo compositor end-to-end on a
// real Quest WITHOUT needing Wine/Box64/Vortek. When GAMENATIVE_XR_TEST_PRODUCER=1, quest_xr
// starts this against its own socket (AF_UNIX loopback). The real producer reuses the same
// wire protocol; only the buffer *contents* change (game GPU render instead of a test fill).

#pragma once

#include <string>

namespace gamenative::xr {

// Spawn a background thread that connects to `socketPath` and feeds two test eye buffers.
// No-op if already started. Connection is retried briefly so it can start right after the
// transport begins listening.
void startTestProducer(const std::string& socketPath, int width, int height);

// Stop the producer thread and release its buffers.
void stopTestProducer();

}  // namespace gamenative::xr
