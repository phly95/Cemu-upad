# AGENTS.md - Cemu Vulkan DRC/Gamepad Streaming

## Project Context

This is a fork of the Cemu Wii U emulator with added Vulkan-based video streaming.
The streaming feature encodes rendered frames via GStreamer and sends them over UDP/RTP.
Two independent streams run simultaneously:

- **TV stream** (port 5000): captures the TV swapchain image after compositing
- **DRC/gamepad stream** (port 5001): captures the Wii U GamePad texture directly

Both use the same `VulkanFrameStreamer` class, instantiated separately as `m_frameStreamer` (TV) and `m_frameStreamerDRC` (DRC).

## Critical Constraint: Do NOT Reference OpenGL Streaming

The OpenGL streaming path (`GLFrameStreamer`) is **unpolished, untested, and unreliable**.
Never use it as a design reference, never compare against it, never port patterns from it.
If OpenGL code appears to contradict Vulkan best practices or Azahar, ignore the OpenGL code.

The only reference implementation to consult is **Azahar** at:
```
/home/philip/Games/azahar-upad
```

Key Azahar files:
- `src/video_core/renderer_vulkan/vk_frame_streamer.h` / `.cpp` - FrameStreamer class
- `src/video_core/renderer_vulkan/renderer_vulkan.cpp` - SwapBuffers streaming loop
- `src/video_core/renderer_vulkan/vk_scheduler.cpp` - Scheduler::Finish() synchronization

## Test Machine Hardware

This is a **hybrid graphics laptop**:
- **iGPU**: Intel HD Graphics 630 (KBL GT2) at `/dev/dri/renderD128` (vendor 0x8086)
- **dGPU**: AMD Radeon RX Vega M GL Graphics at `/dev/dri/renderD129` (vendor 0x1002)
- **Kernel**: 6.17.x (Tuxedo)
- **Mesa**: 26.1.x
- **GStreamer**: 1.24.x
- **Display**: Wayland (not headless, not X11)

Vulkan renders on the AMD dGPU (GPU1 in vulkaninfo).

## Key Source Files

### Streaming core
| File | Purpose |
|------|---------|
| `src/Cafe/HW/Latte/Renderer/Vulkan/VulkanFrameStreamer.h` | Streamer class: frame resources, GStreamer pipeline, DMA-BUF export |
| `src/Cafe/HW/Latte/Renderer/Vulkan/VulkanFrameStreamer.cpp` | Full implementation: image creation, memory allocation, blit, push, GStreamer init |
| `src/Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.cpp` | DRC blit in `DrawBackbufferQuad()`, TV blit after render pass, `SwapBuffers()` sync+push |
| `src/Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h` | `m_frameStreamer`, `m_frameStreamerDRC`, `m_streamBlitRecorded`, `m_streamDRCBlitRecorded` |

### Configuration
| File | Purpose |
|------|---------|
| `src/config/CemuConfig.h:84-91` | `StreamingEncoder` enum: Auto=0, VAAPI=1, VAAPI_LowPower=2, x264=3, OpenH264=4 |
| `src/config/CemuConfig.h` | `streaming_encoder`, `streaming_bitrate`, `streaming_qp`, `streaming_gpu_device`, etc. |
| `~/.config/Cemu/settings.xml` | `<Streaming>` section with `<Encoder>`, `<GPUDevice>`, `<DRCEnabled>`, `<DRCTargetPort>` |

### Build
| File | Purpose |
|------|---------|
| `src/Cafe/HW/Latte/Core/LatteRenderTarget.cpp:829-864` | `LatteRenderTarget_getScreenImageArea()` - computes imageX/imageY as **destination viewport coordinates** |
| `src/Cafe/HW/Latte/Core/LatteRenderTarget.cpp:866-920` | `LatteRenderTarget_copyToBackbuffer()` - calls `DrawBackbufferQuad()` |

## Build Instructions

```bash
cd /home/philip/Games/Cemu/build
ninja -j$(nproc)
```

The binary is output to:
```
/home/philip/Games/Cemu/bin/Cemu_relwithdebinfo
```

To force a rebuild of changed files:
```bash
touch src/Cafe/HW/Latte/Renderer/Vulkan/VulkanFrameStreamer.cpp
cd build && ninja -j$(nproc)
```

After rebuilding, the Cemu binary at `bin/Cemu_relwithdebinfo` will be updated. Timestamps can be checked with `ls -la bin/Cemu_relwithdebinfo`.

## Running Tests

Cemu is a wxWidgets GUI application. It requires a display. On this machine it runs on Wayland.

### Launching Cemu with a game

```bash
DISPLAY=:1 XDG_RUNTIME_DIR=/run/user/$(id -u) \
  /home/philip/Games/Cemu/bin/Cemu_relwithdebinfo \
  -g "/home/philip/Downloads/Super Mario Maker [AMAE01]/code/Block.rpx" \
  > /tmp/cemu-stdout.log 2>&1 &
```

**IMPORTANT**: Cemu must be launched in a fully detached process. Use `setsid`:
```bash
setsid bash -c 'DISPLAY=:1 XDG_RUNTIME_DIR=/run/user/$(id -u) /home/philip/Games/Cemu/bin/Cemu_relwithdebinfo -g "/home/philip/Downloads/Super Mario Maker [AMAE01]/code/Block.rpx" > /tmp/cemu-stdout.log 2>&1' &
```

Without `setsid`, the Cemu process dies when the shell session ends.

### Configuring streaming before launch

Edit `~/.config/Cemu/settings.xml`. The `<Streaming>` section:
```xml
<Streaming>
    <Enabled>true</Enabled>
    <Encoder>3</Encoder>         <!-- 0=Auto, 1=VAAPI, 2=VAAPI_LowPower, 3=x264, 4=OpenH264 -->
    <Bitrate>4000</Bitrate>
    <QP>22</QP>
    <GPUDevice></GPUDevice>      <!-- empty=auto, or /dev/dri/renderD129 for AMD -->
    <TargetIP>127.0.0.1</TargetIP>
    <TargetPort>5000</TargetPort>
    <DRCEnabled>true</DRCEnabled>
    <DRCTargetPort>5001</DRCTargetPort>
</Streaming>
```

To change config for a test:
```bash
cat ~/.config/Cemu/settings.xml | \
  sed 's|<Encoder>[0-9]*</Encoder>|<Encoder>3</Encoder>|' | \
  sed 's|<GPUDevice>[^<]*</GPUDevice>|<GPUDevice></GPUDevice>|' \
  > /tmp/settings_new.xml && cp /tmp/settings_new.xml ~/.config/Cemu/settings.xml
```

### Receiver pipelines

**Basic receiver (display)**:
```bash
DISPLAY=:1 XDG_RUNTIME_DIR=/run/user/$(id -u) \
  gst-launch-1.0 udpsrc port=5001 caps="application/x-rtp, media=video, clock-rate=90000, encoding-name=H264, payload=96" \
  ! rtph264depay ! h264parse ! decodebin ! videoconvert ! autovideosink sync=false
```

**FPS measurement with display**:
```bash
DISPLAY=:1 XDG_RUNTIME_DIR=/run/user/$(id -u) \
  gst-launch-1.0 -v udpsrc port=5001 caps="application/x-rtp, media=video, clock-rate=90000, encoding-name=H264, payload=96" \
  ! rtpjitterbuffer latency=0 drop-on-latency=true \
  ! rtph264depay ! h264parse ! decodebin ! videoconvert \
  ! fpsdisplaysink video-sink=waylandsink text-overlay=false sync=false fps-update-interval=1000
```

**FPS measurement without display (headless benchmark)**:
Same but replace `video-sink=waylandsink` with `video-sink=fakesink`.

**IMPORTANT jitter buffer note**: The property is `drop-on-latency=true`, NOT `drop-on-late`. The latter does not exist and will cause `erroneous pipeline`.

**MKV capture**:
```bash
gst-launch-1.0 -e udpsrc port=5001 caps="application/x-rtp, media=video, clock-rate=90000, encoding-name=H264, payload=96" \
  ! rtph264depay ! h264parse ! matroskamux ! filesink location=cemu-drc-test.mkv
```

### Killing processes

**Do NOT use `pkill` without a tool-level timeout** - it can hang the terminal.

Use the bash tool's `timeout` parameter (e.g. `timeout: 3000`) for any kill command.
Or use `kill $(ps aux | grep ... | awk '{print $2}')` and verify separately.

```bash
kill $(ps aux | grep "Cemu_relwith" | grep -v grep | awk '{print $2}') 2>/dev/null
# then verify:
ps aux | grep "Cemu_relwith" | grep -v grep
```

## Architecture: How Streaming Works

### Frame lifecycle

1. **Render**: Cemu renders the Wii U game to Vulkan textures
2. **Blit**: In `DrawBackbufferQuad()`:
   - **DRC**: Blits directly from the game texture (before swapchain) using `vkCmdBlitImage` from `(0,0)` to `(baseW,baseH)` into a LINEAR image
   - **TV**: Blits the swapchain image after the render pass into the streamer's LINEAR image
3. **Copy to staging**: After the blit, `vkCmdCopyImageToBuffer` copies from the LINEAR VkImage to a HOST_CACHED VkBuffer (runs on GPU DMA engine)
4. **Sync**: In `SwapBuffers()`: `SubmitCommandBuffer()` + `vkWaitForFences()` (GPU must finish the blit + copy)
5. **CPU read**: `memcpy` from the persistently mapped HOST_CACHED staging buffer to a system-memory GstBuffer (fast, write-back cached reads)
6. **Push**: `g_signal_emit_by_name(appsrc, "push-buffer", buf)` sends to GStreamer pipeline
7. **Encode**: GStreamer pipeline: `appsrc -> queue -> videoconvert -> encoder -> h264parse -> rtph264pay -> udpsink`

### Key dimensions

- **TV streamer**: 1280x720 (swapchain size), stride=5120 (1280*4, tight)
- **DRC streamer**: 854x480 (Wii U GamePad), stride=3584 (Vulkan rowPitch, **NOT** 3416)
- **Frame resources**: 3 per streamer (triple-buffered), NUM_FRAMES=3
- **Image format**: VK_FORMAT_R8G8B8A8_UNORM, LINEAR tiling, TRANSFER_DST | SAMPLED

### Memory allocation

Memory type selection prefers `DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT` (memory type 3 on this AMD GPU). This is used for the LINEAR VkImage for DMA-BUF export.

**Critical**: On AMD/RADV, `HOST_VISIBLE | HOST_COHERENT` memory is **write-combining** for CPU reads. CPU reads from this memory are extremely slow. LINEAR VkImages cannot use `HOST_CACHED` memory (driver restriction on `memoryTypeBits`).

**Staging buffer**: Each `FrameResources` includes a `VkBuffer` staging buffer allocated from `HOST_VISIBLE | HOST_COHERENT | HOST_CACHED` memory (type 5 on this AMD GPU). VkBuffers CAN use HOST_CACHED memory when VkImages cannot. The GPU copies the LINEAR image to the staging buffer via `vkCmdCopyImageToBuffer`, then the CPU reads from write-back cached memory. This is the key to 60 FPS streaming.

AMD GPU memory types (vulkaninfo):
| Type | Flags | Usage |
|------|-------|-------|
| 0 | DEVICE_LOCAL | VkImage (optimal/linear) |
| 3 | DEVICE_LOCAL \| HOST_VISIBLE \| HOST_COHERENT | VkImage (write-combining CPU reads, slow) |
| 5 | HOST_VISIBLE \| HOST_COHERENT \| HOST_CACHED | VkBuffer (write-back cached CPU reads, fast) |

### DRC source rectangle (IMPORTANT)

`imageX`, `imageY`, `imageWidth`, `imageHeight` in `DrawBackbufferQuad()` are **destination viewport coordinates** (where to draw on the swapchain window), NOT source texture coordinates.

The DRC blit always captures the full base texture from `(0, 0)` to `(baseW, baseH)` clamped to streamer dimensions. Do NOT use `imageX`/`imageY` as source offsets.

## Known Issues and Fixes Applied

### Stride mismatch (PRIMARY CORRUPTION FIX)

Vulkan LINEAR images on AMD GPUs have rowPitch > width*4 due to alignment:
- 854 * 4 = 3416 (expected tight stride)
- Vulkan rowPitch = 3584 (actual, 256-byte aligned)

Without `GstVideoMeta`, GStreamer assumes tight packing and produces blue/corrupted images with horizontal scan lines.

**Fix**: `gst_buffer_add_video_meta_full()` is called in `PushFrame()` with the actual stride. This tells GStreamer the real row pitch.

### GST_VAAPI_DRM_DEVICE init order

On hybrid AMD/Intel laptops, the VAAPI plugin reads `GST_VAAPI_DRM_DEVICE` at element creation time. If `gst_init()` is called first, VAAPI may auto-select the wrong GPU.

**Fix**: Set `GST_VAAPI_DRM_DEVICE` BEFORE calling `gst_init()`. Auto-detect the DRM render node matching the Vulkan physical device's vendor ID by scanning `/sys/class/drm/renderD*/device/vendor`.

### VAAPI Low Power on AMD

`vah264lpenc` (VAAPI Low Power) is not supported on AMD GPUs. `vainfo` on renderD129 shows no `VAEntrypointEncSliceLP` entries.

**Fix**: When encoder is `VAAPI_LowPower` and the DRM device is AMD (vendor 0x1002), fall back to normal `vaapih264enc`.

### Log throttling

Previous implementation logged every frame (~6 log lines/frame = 34K lines for a short session). This itself can cause choppiness.

**Fix**: 
- Initialization logs: always logged
- `STRIDE MISMATCH`: logged once per frame resource (3x total for NUM_FRAMES=3), not per frame
- `GstVideoMeta` added silently (no per-frame log)
- `PushFrame` timing: logged every 300 frames, or when push takes > 5ms, or on error
- No per-frame RecordBlit, SwapBuffers, or fence wait logs

## Reference Test Results (2026-07-06, after staging buffer fix)

| Test | Encoder | Sink | Avg FPS | Rendered (30s) | Dropped |
|------|---------|------|---------|----------------|---------|
| x264 DRC + fakesink | x264enc | fakesink | 60.00 | ~1800 | 0 |
| x264 DRC + display | x264enc | waylandsink | ~58 | ~1740 | 23 |

Key observations:
- **Sender pipeline** (DRC 854x480, x264enc): all stages at ~59 FPS (appsrc → queue → videoconvert → encoder → payloader → udpsink)
- **Zero drops** at the leaky queue (previously dropped ~75% of frames)
- PushFrame uses HOST_CACHED staging buffer: `vkCmdCopyImageToBuffer` → CPU memcpy from cached memory
- Receiver fakesink achieves 60 FPS with zero drops
- Waylandsink achieves ~58 FPS with minimal drops (Wayland compositor timing)

### Previous results (before staging buffer fix, for reference)

| Test | Encoder | Sink | Avg FPS | Rendered (65s) | Dropped |
|------|---------|------|---------|----------------|---------|
| x264 + display | x264enc | waylandsink | 14.02 | 888 | 0 |
| x264 + fakesink | x264enc | fakesink | 13.90 | 867 | 0 |
| VAAPI AMD + display | vaapih264enc | waylandsink | 12.89 | 819 | 68 |
| VAAPI AMD + fakesink | vaapih264enc | fakesink | 14.07 | 872 | 0 |

The ~14 FPS bottleneck was caused by GStreamer `videoconvert` reading from write-combining (WC) Vulkan memory. Fixed by adding HOST_CACHED staging VkBuffer.

## GStreamer Warning During Plugin Scan

```
(gst-plugin-scanner:XXXX): CRITICAL: _dma_fmt_to_dma_drm_fmts: assertion 'fmt != GST_VIDEO_FORMAT_UNKNOWN' failed
```

This appears during GStreamer initialization and is a known VAAPI plugin issue. It does not affect streaming functionality.

## Uncommitted Changes

As of 2026-07-06, the following files have uncommitted patches:

```
 M src/Cafe/HW/Latte/Renderer/Vulkan/VulkanFrameStreamer.cpp   (+419/-76)
 M src/Cafe/HW/Latte/Renderer/Vulkan/VulkanFrameStreamer.h    (+34/-1)
 M src/Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.cpp       (+87/-15)
 M src/Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h         (+4/-0)
```

Changes include:
1. GstVideoMeta stride fix for non-tight DMA-BUF row pitch
2. GST_VAAPI_DRM_DEVICE set before gst_init()
3. VAAPI LowPower AMD fallback guard
4. DRC source rectangle fix (destination coords, not source offsets)
5. Comprehensive log throttling (init-only + every-300-frames)
6. Physical device name/vendor logging at streamer creation
7. **HOST_CACHED staging VkBuffer** for 60 FPS streaming (GPU copy + CPU cached read)
8. Sender-side FPS counters (DrawBackbufferQuad, RecordBlit, PushFrame, appsrc push)
9. GStreamer pad-probe FPS counters at all pipeline stages
10. Named pipeline elements for probe attachment

Review packs:
- `cemu-vulkan-drc-next-review-pack.zip` - latest with receiver FPS results
- `cemu-vulkan-drc-review-pack.zip` - earlier version with Azahar comparison
