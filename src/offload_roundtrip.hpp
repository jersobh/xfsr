#pragma once

#include "frame_transfer.hpp"
#include "fsr_pass.hpp"

// Fused dGPU → iGPU(FSR) → dGPU offload round trip.
//
// The legacy path chained two independent FrameTransfer pipelines with a
// blocking iGPU FSR submit in between (upload; FSR + host wait; download).
// That host wait serialises the whole round trip: measured ≈4 ms/frame on a
// GTX 1650 (PCIe 3 x8) + Vega 10, i.e. the ~245 FPS ceiling.
//
// This drives the same buffers/fences as a 3-stage software pipeline, one
// submit per device per stage, and only host-waits on fences that are a full
// frame old (normally already signalled):
//
//   iteration f:  B  iGPU  frame f−1: fwd buf → image, EASU/RCAS, out → back buf
//                 C  dGPU  frame f−2: back buf → display image (present source)
//                 A  dGPU  frame f  : app image → fwd buf (waits app semaphores)
//
// Slot k = frame % kSlots owns: fwd host buf/iGPU image (FrameTransfer `fwd`),
// back host buf/dGPU image (FrameTransfer `back`). Fence roles:
//   fwd.slots[k].srcFence  — A (dGPU download) done
//   back.slots[k].srcFence — B (iGPU fused upload+FSR+download) done
//   back.slots[k].dstFence — C (dGPU upload) done
// Pipeline state lives in the FrameTransfers (framesProduced / pendingUpload),
// so FrameTransfer::resetPipeline() (resize/destroy) resets this too.
struct OffloadRoundTrip {
    // Returns the display-sized dGPU image (SHADER_READ_ONLY) to present this
    // frame. Adds 2 frames of latency in steady state (legacy path: up to 4).
    static const GpuImage& submit(FrameTransfer& fwd, FrameTransfer& back, FsrPass& fsr,
                                  const GpuImage& appImage, uint32_t waitSemCount,
                                  const VkSemaphore* waitSems,
                                  const VkPipelineStageFlags* waitStages,
                                  VkImageLayout appLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    // XFSR_RT_LEGACY=1 → old chained path (A/B benchmarking).
    static bool legacyRequested();
};
