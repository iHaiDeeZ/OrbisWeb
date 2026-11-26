# WebGPU for OrbisWeb: research and plan

Started 2026-11-24, after v00.30 (WebGL 1 and 2). User: "add support to WebGPU", then yes to this plan
and to the download.

## Progress

- **2026-11-24, step 1 under way (v00.31).** Dawn and its 8 dependencies were downloaded (40 MB) into
  `~/wisp-build/src/dawn`, a git tree on branch `wisp` (base commit, then the PS4 commits, exported to
  `patches/dawn/`). `scripts/build-dawn.sh` builds one static library, `libwebgpu_dawn.a` (Dawn, Tint and
  Abseil; SPIRV-Tools isn't needed: Tint writes SPIR-V itself and validation is off). Patch 0001:
  - the PS4 as a platform: POSIX, not Linux (no DMA-BUF or sync-file code);
  - Vulkan from the kit's `ps4kit_vk_get_instance_proc_addr` (no libvulkan to load);
  - no executable or module path, as on Fuchsia.
  Everything else compiled for the PS4 unchanged.
- The GPU web process links it and has a **self-test** (`app/src/child/webgpu_selftest.cpp`): the menu's
  WebGPU test restarts OrbisWeb; at that start the UI marks the first GPU web process (`WispStart`
  flag), which 5 s in, beside Zink, makes a Vulkan adapter and a device, runs a WGSL compute shader and
  renders a triangle, both read back and checked. Each step goes to `/data/Wisp/run/webgpu-test.txt` as
  it ends; a toast and the About page give the result.
- **v00.34: the self-test passed on the Pro** (adapter, device beside Zink's, compute, triangle).
- **2026-11-25, step 2 started** (user: "Yes, start WebGPU", after Hot Pursuit 2's page said "This
  browser has no WebGPU"). What WebKit 2.54.2 needs, found in its source:
  - The page process makes `navigator.gpu` through `WebChromeClient::createGPUForWebGPU()`. Our builds have
    ENABLE_GPU_PROCESS off, so it calls `WebCore::WebGPU::create()` in the page process itself: the place
    for Dawn.
  - The DOM layer (`Modules/WebGPU/GPU*.cpp`, bindings) is always compiled; `WebGPUEnabled` (runtime
    setting, a WebKitFeature) turns `navigator.gpu` on. `HAVE(WEBGPU_IMPLEMENTATION)` (PlatformHave.h,
    Cocoa only) compiles the Implementation layer and the page-process code that creates it (Navigator,
    WorkerNavigator, Chrome, ChromeClient virtuals, OffscreenCanvas, WebChromeClient, WebWorkerClient):
    none of it is serialized between processes, so only the web tree needs it. `ENABLE(WEBGPU)` guards
    only WebXR's WebGPU parts and stays off.
  - The Implementation layer (63 files, ~5,300 lines of .cpp) calls WebKit's own fork of the C API: 82 of
    the 191 functions it calls don't exist in Dawn (renamed since, or WebKit-only: `IsValid`, XR,
    IOSurface swap chains, `...WithBlock`). Dawn follows today's standard `webgpu.h`: `WGPUStringView`
    labels, callback-info structs (mode + two userdata), futures.
  - `GPUBuffer` copies a mapped range into a JavaScript ArrayBuffer and writes it back on unmap through
    `Buffer::copyFrom`, which the non-Swift implementation leaves as RELEASE_ASSERT_NOT_REACHED.
  - `GPUCanvasContext::create()` returns null off Cocoa, and Apple's context (`GPUCanvasContextCocoa.mm`)
    isn't in the tarball; the interfaces it uses (`PresentationContext`: configure, getCurrentTexture,
    present; `CompositorIntegration`: prepareForDisplay, `paintCompositedResultsToCanvas(ImageBuffer&)`)
    fit a CPU read-back.
- **The route taken:**
  1. Port the Implementation layer to Dawn's C API (same classes, so the DOM layer is untouched): WebKit's
     blocks become CompletionHandlers in heap boxes passed as userdata; callbacks use
     `WGPUCallbackMode_AllowProcessEvents`, and the GPU object pumps `wgpuInstanceProcessEvents` on the
     main run loop while anything waits (WebKit wants its callbacks there); `copyFrom` writes into Dawn's
     mapped range; the `IsValid` checks become null checks.
  2. The CPU web binary (no Mesa, no Dawn) links the same WebKit: it gets do-nothing `wgpu*` stubs, and
     `WebGPU::create()` asks the app (`wisp_webgpu_usable`) before making anything.
  3. `HAVE_WEBGPU_IMPLEMENTATION` for the web tree only (a full rebuild of that tree, once).
  4. A WPE `GPUCanvasContext`: Dawn textures of its own as the canvas's buffers; at the frame's end the
     current one is copied into a mappable buffer, read back and painted into the canvas's ImageBuffer, which
     WebKit then composites like a 2D canvas. Zero-copy later, if Zink can use Dawn's memory.
  5. The UI turns the `WebGPU` feature on when the GPU web process draws (setting `webgpu`).
- **v00.40: WebGPU for pages, first build** (2026-11-25; steps 1-5 above; on the Pro .137, untested). WebKit
  patch 0039 (`f0a5ab71`):
  - **The Implementation layer on Dawn's `webgpu.h`.** `WebGPUDawn.h` holds the glue:
    - string views both ways;
    - CompletionHandlers boxed as callback userdata (`PendingCallback`);
    - `DawnEventPump`: while a callback is owed, a helper thread posts `wgpuInstanceProcessEvents` to the
      thread that asked (WebKit's schedule-work function), at most every 2 ms; it stops when nothing is owed.
  - **Other parts:**
    - `Buffer::copyFrom` writes into Dawn's mapped range;
    - a device's spent objects (an encoder after `finish`, a pass after `end`) are cached invalid objects;
    - errors a page doesn't catch go to web.log ("WebGPU (Dawn): ...": the first 40, then every 500th;
      `WISP_WEBGPU_LOG`, setting `webgpu_log`); a lost device always does. The page's console doesn't get
      them: `GPUDevice` listens on Apple's systems only.
  - **The switch.** `WebGPU::create()` asks `wisp_webgpu_usable()`. WebKit's weak definition says no
    (the CPU web process); the GPU web process's `child_main.cpp` answers with the setting `webgpu`.
  - **Canvases** (`GPUCanvasContextWisp` in `GPUCanvasContext.cpp`, `PresentationContextImpl`):
    - the canvas's textures are Dawn textures of its size (CopySrc added to the page's usage);
    - at the rendering update after the page drew, `present()` copies the frame into a mappable buffer (rows
      aligned to 256 bytes);
    - at most two frames are read back at once: a frame presented while two are owed is dropped;
    - a mapped frame becomes a Skia image, which the canvas's layer shows
      (`GraphicsLayerContentsDisplayDelegateCoordinated`, `CoordinatedPlatformLayerBufferNativeImage`) and
      `paintCompositedResultsToCanvas` draws (drawImage, toDataURL).
  - **Not done:**
    - external textures (video into WebGPU: `importExternalTexture` gives null);
    - XR;
    - `writeTimestamp` (a no-op, as Dawn has no such call; passes' `timestampWrites` go to Dawn).
  - **First tests on the Pro (v00.40):**
    - Hot Pursuit 2 got WebGPU ("WebGPU ready (amd amd)", 60 pipelines in 2.8 s) and booted
      with sound; its trouble was elsewhere: failed reads of its data file, then a reload's memory (v00.41,
      WebKit patch 0042).
    - Midnight Club 3 runs on WebGPU too; its network process ended before it drew
      (v00.41, patch 0041).
    - Mortal Kombat (a PS2 emulator page) ended the web process twice (v00.42):
      `setBindGroup(index, group, Uint32Array, start, length)` reached the Implementation layer's span overload,
      which upstream leaves `RELEASE_ASSERT_NOT_REACHED` (Apple's GPU process turns the array into a Vector
      first). Patch 0047 implements it for compute passes, render passes and render bundles. Other unreachable
      ones left: `DeviceImpl::updateExternalTexture` (Cocoa video only) and the XR binding.
    - Mortal Kombat then ran (6-10 fps, v00.43) until direct memory ran out ~90 s in: `getCurrentTexture()` made a
      new Dawn texture every frame. Patch 0051 keeps one texture a canvas (until its size or configuration changes
      or the page destroys it) and clears it after each readback copy, in the same submission; a texture whose
      creation ran short of memory is not kept (error scope). web.log's "frames:" lines count canvas frames
      presented, not read back, shown and textures made (patch 0050's counters).
  - **A risk to watch:** Tint compiles on the thread that calls Dawn, the page's main thread, whose stack is
    2 MiB (web.log's first memory line); the self-test gave Tint 8 MiB ("Tint's compiler recurses"). A
    crash deep in Tint on a big shader would point there.
  - **App side:**
    - the CPU web binary links do-nothing `wgpu*` functions (`app/src/child/webgpu_stubs.c`, generated by
      `scripts/gen-webgpu-stubs.py` from Dawn's header);
    - `app/CMakeLists.txt` finds WebGPU in the web tree's flags (`WISP_HAVE_WEBGPU`);
    - Settings: WebGPU (Pages and JavaScript; needs CPU + GPU or GPU drawing) and WebGPU errors in the log
      (Diagnostics);
    - About's WebGPU row says which, with the last self-test.

## What WPE WebKit 2.54.2 has

- **No WebGPU implementation.** WebKit's WebGPU is `Source/WebGPU` (WebGPU.framework, on Metal, in
  Objective-C++ and Swift). The WPE tarball has only its headers: WebKit's C API `WebGPU.h` (218
  functions, a fork of the standard webgpu.h) and `WebGPUExt.h` (26). `ENABLE_WEBGPU` is on for Apple's
  ports only.
- **The whole Web API.** WebCore's `Modules/WebGPU` has the IDL bindings, the abstract `InternalAPI`, and
  an `Implementation` layer (31 files in `Sources.txt`) that turns them into calls of that C API.
- **An in-process path.** Without a GPU process, `WebChromeClient::createGPUForWebGPU()` calls
  `WebCore::WebGPU::create()` (`WebGPUCreateImpl.cpp`): `navigator.gpu` from the C API in the web process
  itself. Everything is under `HAVE(WEBGPU_IMPLEMENTATION)`, which `PlatformHave.h` sets for Cocoa only.
- **Apple-only parts of the Implementation layer:**
  - callbacks are Clang blocks (`BlockPtr`, `^{}`);
  - the compositor integration and presentation context use IOSurfaces;
  - `WebGPUCreateImpl` weak-links WebGPU.framework;
  - external textures from video come from CVPixelBuffers;
  - XR.
- **No canvas.** `GPUCanvasContext::create()` returns null outside Cocoa, so `canvas.getContext("webgpu")`
  gives nothing. Apple's `GPUCanvasContextCocoa.mm` isn't in the tarball (only its header).
- **Upstream has none for Linux.** In 2026 the GTK and WPE ports even turned their GPU process off at
  runtime (WebKit bug 307065). gpuweb's implementation-status wiki lists Linux only for Chromium (Dawn)
  and Firefox (wgpu).

## The route: Dawn on the kit's RADV, in the GPU web process

Dawn is Chromium's WebGPU, in C++, with a Vulkan backend. The PS4 gives the GPU to one process
(`ps4-vfork-exit-gpu-vm`), so Dawn runs in the GPU web process (`wisp-web-gpu.bin`), as WebGL's ANGLE
does, not in a GPU process.

1. **Dawn for the PS4 (first milestone).**
   - Release v20261120.121236 (21 Nov 2026), built with CMake: static, Vulkan backend only, plus Tint
     (its WGSL to SPIR-V compiler); no GL, D3D, Metal, X11, Wayland, samples or tests.
   - Vulkan comes through the kit's `ps4kit_vk_get_instance_proc_addr`, as Zink's does: no libvulkan to
     dlopen.
   - A self-test in the GPU web process behind a setting, written to web.log:
     - the adapter's info, limits and features on GFX7;
     - a WGSL compute shader;
     - a triangle rendered into a texture and read back.
   - **Risk: a second Vulkan device next to Zink's in one process.** Each RADV device reserves its own
     GPU address windows (`ac_ps4_drm.c`: GPU address = CPU address), but some of that file's state is
     per process: `ps4_last_device`, the held and released address ranges, the fault capture. The
     self-test answers this before anything else; the kit gets patches if needed.
2. **WebKit (the web process's build only).**
   - `ENABLE_WEBGPU` and `HAVE_WEBGPU_IMPLEMENTATION` for that build. Check `MessageName` and the
     serialized structs for WebGPU guards, as v00.30 did for WebGL: the UI and network processes keep a
     build without it.
   - The Implementation layer onto Dawn. Preferred: port its 31 files from WebKit's C API to Dawn's
     standard webgpu.h (callbacks instead of blocks; the standard header is close to WebKit's fork).
     The alternative: compile it as it is with `-fblocks` and a BlocksRuntime, and write WebKit's C API
     (244 functions) over Dawn.
3. **The canvas.** A `GPUCanvasContext` for WPE: `configure`, `getCurrentTexture`, presenting at the
   frame's end. Frames reach WebKit's compositor (TextureMapper, GL on Zink) at first by read-back: Dawn
   copies the texture into a mapped buffer and the compositor uploads it. Later maybe without a copy, if
   RADV's external host memory lets Zink use Dawn's memory (one address space on the PS4).
4. **Then:**
   - a WebGPU switch in the menu, an About row, a setting (v00.40);
   - tests with the WebGPU samples (webgpu.github.io/webgpu-samples);
   - the memory cost (Dawn, Tint and SPIRV-Tools: an estimated 15-30 MB more image for the GPU web
     process, in the pool the app's processes share).

## To download (only with the user's go-ahead)

- **Dawn v20261120.121236**, GitHub google/dawn (the official source is dawn.googlesource.com): the
  release's source archive, tens of MB. The whole repository with its history is 534 MiB and isn't needed.
- **Its pinned dependencies** at the revisions its DEPS file names, one revision each (no history): abseil-cpp,
  SPIRV-Tools, SPIRV-Headers, Vulkan-Headers, Vulkan-Utility-Libraries, jinja2, markupsafe and
  webgpu-headers. Together with Dawn, about 100 MB.
- **Not fetched:** webgpu-cts (large), vulkan-loader, the validation layers, glslang, DXC, GLFW, protobuf,
  benchmark and googletest.
