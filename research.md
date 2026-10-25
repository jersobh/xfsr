# CrossVulkan Offload Bottleneck Research & Analysis

## Executive Summary
In CrossVulkan dual-GPU offload mode, the workload involves:
- **dGPU (NVIDIA GTX 1650 Mobile)** rendering at reduced resolution (e.g. 960×540, ~2 MB).
- **iGPU (AMD Radeon Vega 10 Mobile)** running AMD FSR 1.0 (EASU + RCAS up to 1080p, ~8 MB).
- **dGPU WSI Present**: Presenting the final 1080p image back on the dGPU's swapchain.

**Observed Symptom:**
- Same-GPU FSR: ~585–775 FPS at 960×540.
- Offload FSR: ~245 FPS ceiling on identical load.

---

## 1. Hardware & System Topology
- **dGPU**: NVIDIA GeForce GTX 1650 Mobile (TU117M), driver 570.86.16 / 615.78.
  - PCIe Link: 8.0 GT/s (Gen 3) x8 width (~6.3 GB/s practical bandwidth).
  - Dedicated transfer queue family available (Queue Family 1, Copy Engine).
- **iGPU**: AMD Radeon Vega 10 Mobile (Picasso/Raven 2), RADV driver.
  - Graphics queue only (Queue Family 0).
- **Inter-GPU Constraints**:
  - `VK_EXT_external_memory_dma_buf` between NVIDIA and RADV causes `VK_ERROR_DEVICE_LOST` ("Not enough memory for command submission" on RADV).
  - One-way iGPU present is blocked on NVIDIA PRIME setups where the display server runs on the dGPU.
  - Hence, **shared_host** (`VK_EXT_external_memory_host`) round-trip transfer is mandatory.

---

## 2. Microbenchmark Findings (`xfer_probe`)
We built a dedicated microbenchmark measuring each hop and pipeline configuration across the physical devices:

### A. Per-Hop Measurements (Serialized: Submit + Host Wait)
| Stage | Operation | Size | GPU Exec Time | Wall Time (w/ Host Wait) | Effective Bandwidth |
|---|---|---|---|---|---|
| **Hop 1** | NV D2H (dGPU Render -> Host) [Gfx Queue] | 1.98 MB | 0.455 ms | 0.931 ms | 2.08 GB/s (4.25 GB/s GPU) |
| **Hop 1b** | NV D2H (dGPU Render -> Host) [Copy Engine] | 1.98 MB | 0.466 ms | 0.558 ms | 3.47 GB/s |
| **Hop 2** | AMD H2D (Host -> iGPU Image) | 1.98 MB | 0.296 ms | 0.640 ms | 3.02 GB/s |
| **Hop 3** | AMD D2H (iGPU FSR Out -> Host) | 7.91 MB | 0.893 ms | 1.284 ms | 6.01 GB/s |
| **Hops 2+3**| AMD Fused Up + Down in **Single Submit** | 9.89 MB | 0.803 ms | 1.224 ms | 7.89 GB/s |
| **Hop 4** | NV H2D (Host -> dGPU Image) [Gfx Queue] | 7.91 MB | 1.267 ms | 1.353 ms | 5.71 GB/s |

### B. Pipeline Concurrency
- **Sum of all 4 serialized hops (no FSR)**: ~4.2 ms GPU execution / 7.3 ms wall time.
  - $1000 / 4.2 \text{ ms} \approx 238 \text{ FPS}$.
- **NV Duplex**: Concurrent D2H(copy) and H2D(gfx) across different slots takes only **1.54 ms - 3.55 ms**.
- **Simulated 3-Slot Pipeline**:
  - Pipelined with separate submits + host waits: **3.97 ms (252 FPS)** — exactly matching the ~245 FPS observed bottleneck!
  - Pipelined with **fused iGPU submit (1 submit, no host wait)**: **1.79 ms (558 FPS)**.

---

## 3. Root Cause of the 245 FPS Ceiling
The previous implementation in `xfsr_layer.cpp` chained two independent `FrameTransfer` instances with a synchronous iGPU compute dispatch:
```cpp
// Legacy path:
dd->xfer.transferPipeline(...); // Download dGPU -> Host, Upload Host -> iGPU
dd->igpu.resetAndBegin();
ps->fsr.record(dd->igpu.cmd, onIgpu);
dd->igpu.endSubmitWait();       // <-- HOST STALL on CPU/Host thread every frame!
dd->xferBack.transferPipeline(...); // Download iGPU -> Host, Upload Host -> dGPU
```
1. **CPU Host Synchronization Stall**:
   `dd->igpu.endSubmitWait()` forced the CPU thread to block until the iGPU finished FSR compute. This prevented the CPU from kicking off next-frame transfers or pipelining work.
2. **Double Round-Trip Serialization**:
   Rather than executing as a unified 3-stage pipeline (Download $N$, FSR $N-1$, Upload $N-2$), each frame executed multiple submits with CPU-side fence waits. The round-trip wall-clock time added up to ~4.0 ms per frame, creating an artificial ceiling of $1000 / 4.0 \text{ ms} \approx 250 \text{ FPS}$.

---

## 4. Solution: Fused 3-Stage Asynchronous Pipeline (`OffloadRoundTrip`)
To remove the bottleneck without requiring dma-buf or one-way present, we implemented a fused multi-slot pipeline:
1. **Unified Slot Resources**:
   Reuses existing `FrameTransfer` (fwd and back) 3-slot ring buffers, pinned host memory, and fences.
2. **Single Fused iGPU Submission**:
   For frame $N-1$, upload $N-1$, FSR compute, and download $N-1$ are recorded into a **single command buffer** and submitted once to the iGPU graphics queue.
3. **No Per-Frame Host Stalls**:
   Only fences from older frames ($N-2$ or $N-3$) are waited on to ensure memory reuse. Because those operations were scheduled in prior iterations, the fences are almost always already signaled, eliminating CPU idle bubbles.
4. **Queue-Ordered dGPU Present**:
   The back-haul upload into dGPU memory runs on the same dGPU queue as the present blit, ensuring zero CPU stalls between back-haul upload and final swapchain present.
5. **Fallback Toggle**:
   Kept `XFSR_RT_LEGACY=1` for A/B testing and non-host-import backends (e.g. memcpy).

---

## 5. Measured Benchmark Results

Testing on the host system (NVIDIA GeForce GTX 1650 Mobile PCIe 3.0 x8 + AMD Radeon Vega 10 Mobile, 1920×1080 display, complexity 128):

### A. Full Benchmark Suite Results
| Test Configuration | avg FPS | best FPS | Extent | Notes |
|---|---|---|---|---|
| **vk x11/win baseline** | 759.07 | 773.42 | 1920x1080 | Full-res native render |
| **vk x11/win same-GPU perf** | 749.01 | 773.18 | 960x540 | Same-GPU FSR1 (EASU + RCAS) |
| **vk x11/win same-GPU perf --no-rcas** | 741.51 | 773.01 | 960x540 | Same-GPU FSR1 (EASU only) |
| **vk x11/win legacy offload** | ~245.00 | ~250.00 | 960x540 | Double transfer + host CPU wait |
| **vk x11/win NEW fused offload** | **351.42** | **389.49** | 960x540 | **+43.4% (+106.4 FPS)** |
| **vk x11/win NEW fused offload --no-rcas** | **345.22** | **378.59** | 960x540 | **+40.9% (+100.2 FPS)** |
|---|---|---|---|---|
| **vk x11/fs baseline** | 768.71 | 778.05 | 1920x1080 | Full-res fullscreen |
| **vk x11/fs same-GPU perf** | 729.55 | 777.37 | 960x540 | Same-GPU FSR1 |
| **vk x11/fs NEW fused offload** | **344.67** | **391.04** | 960x540 | **+40.7% (+99.7 FPS)** |
| **vk x11/fs NEW fused offload --no-rcas** | **378.97** | **407.05** | 960x540 | **+54.7% (+134.0 FPS)** |

---

## 6. Architecture & Mechanisms Summary
1. **CPU Host Synchronization Eliminated**:
   - The legacy bottleneck was not Vega 10 compute capacity or PCIe bandwidth alone.
   - It was the per-frame `dd->igpu.endSubmitWait()` blocking CPU execution between the forward transfer, the FSR compute pass, and the back-haul transfer.
   - By structuring the pipeline as an asynchronous 3-stage sliding window, CPU-side fence waits only touch operations scheduled in prior iterations ($N-2$, $N-3$) which have already retired.
2. **Fused iGPU Execution**:
   - `OffloadRoundTrip::submit` fuses forward host-buffer upload, EASU/RCAS compute dispatch, and back-haul download staging into a single command buffer submission per frame on the iGPU.
3. **Queue Serialization on dGPU**:
   - The back-haul host-to-device copy into the display image precedes the swapchain present blit on the dGPU's graphics queue, allowing the hardware command processor to overlap transfers with rendering without host synchronization.

---

## 7. How to Run & Verify
- **Standard offload (uses fused pipelined path)**:
  ```bash
  xfsr-run --preset performance --offload <app>
  ```
- **A/B comparison with legacy offload**:
  ```bash
  XFSR_RT_LEGACY=1 xfsr-run --preset performance --offload <app>
  ```


