# OrbisWeb (Wisp): a WebKit browser for the PS4 (design)

> This is the project's working diary, kept version by version. The console logs, clips and promo videos it
> mentions (`logs/`, `promo/`) were never part of the repository.

Shown as **OrbisWeb** since v00.25 (the user's name for it; "Orbis" is the PS4's system). Wisp, the
working name, stays the code's name, the project's folder and `/data/Wisp`; title id `WISP00001`
(so v00.25 installs over Wisp and keeps its settings, downloads and extensions). Started 2026-11-17.

**The rename (v00.25):** every name the user sees says OrbisWeb: the home screen's title (param.sfo),
the package (`OrbisWeb-vXX.YY.pkg`), the menu (About / Restart / Leave OrbisWeb), the About page
(`orbisweb://about`, `about:orbisweb`; `wisp://about` and `about:wisp` still open it), Files' places
("OrbisWeb data", "OrbisWeb app"), messages, "Add to OrbisWeb" on extension stores,
`runtime.getBrowserInfo()`, and the first line of every log. **The icon and start picture** were made
in Blender (`art/orbisweb_art.py`, the scene saved as `art/orbisweb.blend`): the old Wisp emblem in 3D,
three glossy glowing strands (indigo, teal, light blue) braiding up out of a white-hot point of light
and fading at their tips, sparks, out-of-focus lights behind, bloom, "OrbisWeb" in white under it.
Cycles on the GPU in WSL (Blender 4.5 LTS, `~/tools/blender-4.5.14-linux-x64`, CUDA on the PC's RTX
5070 Ti); rendered at 1024 x 1024 and 3840 x 2160 (`art/render`), made `app/sce_sys/icon0.png`
(512 x 512) and `pic1.png` (1920 x 1080) with Pillow's Lanczos. Again:
`blender -b -P art/orbisweb_art.py -- --out art/render --samples 512 --blend art/orbisweb.blend`. The
About page shows the package's own icon (`orbisweb://about/icon.png` serves `/app0/sce_sys/icon0.png`).
A first design (a glass globe with a web inside and an orbit) was dropped: the user wanted the old one,
better.

## What the user asked for

- A lightweight, open-source WebKit.
- Check whether Opera's engine is open source.
- A new, modern browser for the PS4 that takes extensions (ad blockers, Chrome / Opera / Firefox
  extensions).
- WinePS4's file manager (Files) reused so downloaded files can be seen and opened.

## Engine: WPE WebKit 2.54.2

| Option | Verdict |
|---|---|
| **WPE WebKit** (Igalia, LGPL-2.1 / BSD) | **Chosen.** WebKit's port for embedded devices: no toolkit, small, current (2.54.2, 2026-10-23). Has the platform API (`WPEPlatform`) for a custom display/input backend and, new in 2.54, `WPEProcessManager`: the embedder starts WebKit's web and network processes itself (made for Android). Public API for user scripts in isolated worlds, script message handlers with replies, content filters (WebKit content blockers), custom URI schemes, downloads, cookies, website data, remote Web Inspector. Paints with Skia on the GPU. |
| WebKit PlayStation port (Sony, in WebKit's tree) | Built for Sony's SDK (its process launcher, Piglet GL, private libs); paints with Cairo on the CPU. PSChrome (2026-09/10, `~/pschrome-build`) ran it single-process: pages rendered but a full 1080p paint cost ~180 ms, GitHub 0.5-1.3 s. |
| Opera | **Not open source.** Presto (Opera's own engine until 2013) was proprietary; its 2017 source leak was taken down. Opera announced WebKit in 2013 but shipped Blink (Chromium); the Opera browser itself is closed, only its Chromium base is open. Opera's add-ons (`.nex`) are Chromium extensions in a CRX file, so loading Chrome extensions covers Opera's. |
| Chromium / CEF | Not WebKit, very large, many processes (the PS4 allows 7 per app). |
| Ladybird, Servo | Not WebKit; no extension support. |

WebKit's own WebExtensions code (Apple's, `ENABLE_WK_WEB_EXTENSIONS`) builds on GTK/WPE since late
2026 but only loads extensions: the GLib side is stubs (WebKit PR 78305, merged 2026-10-26;
`WebKitWebExtensionManager` PR 75929 still open: "loading extensions works but the extensions won't
work quite yet"). Wisp has its own extension layer (below); it can move to WebKit's when that works.

## Processes on the PS4

```
 Wisp (eboot.bin, UI process)          local process 1            local process 2
 +--------------------------------+   +--------------------+     +--------------------+
 | browser UI (CPU, VideoOut)     |   | WPENetworkProcess  |     | WPEWebProcess      |
 | WPE UIProcess (GLib main loop) |<->| libsoup + OpenSSL  |     | all tabs + exts'   |
 | WPEPlatform "ps4" backend      |   | cookies, cache,    |     | background pages   |
 | WPEProcessManager "ps4"        |<----------------------------->| Skia GL + Zink +   |
 | extensions runtime, Files      |   +--------------------+     | RADV (owns the GPU)|
 +--------------------------------+                              +--------------------+
```

- `WPEProcessManager` subclass: `sceSystemServiceAddLocalProcess("/app0/wisp-web.bin" or
  "wisp-net.bin")`, then the child's parent socket (`sceSystemServiceGetParentSocket`) carries a
  hello, the process type/id and WebKit's IPC socket (SCM_RIGHTS), WinePS4's local-process hand-off
  (`WinePS4/port/ps4_spawn_desk.c`). WebKit's IPC is a `SOCK_SEQPACKET` socketpair (probe it).
- **IPC back-pressure (v00.15, WebKit patch 0019).** The PS4's kernel (FreeBSD 9) gives a unix
  `SOCK_SEQPACKET` socket 8 KiB and shrinks the sender's room by what the receiver has not read yet;
  a send larger than what is left fails with `EMSGSIZE`, not `EAGAIN` (FreeBSD's own note in
  `uipc_usrreq.c`; this is from FreeBSD 9's source and v00.14's symptoms, measured on the console from
  v00.15's ui.log on). WebKit's GLib connection took that for an error and dropped the message, and
  release builds compile its error log out, so nothing said so. WebKit sends a page the next mouse
  event only when the last one is answered (the same for keys): one lost mouse event or answer, and
  no click reached the page again; a lost reload or key did nothing. That was v00.14's "the click did
  not land, the page cannot be refreshed" (no load after 8 s, the web process's loop running, no
  watchdog dump) and most likely v00.12's "the last character never goes, the page cannot be
  refreshed". Now the message waits 1 ms and goes again (those after it wait behind it), WebKit's
  socket pairs get 256 KiB of room (`SO_SNDBUF`/`SO_RCVBUF`), and a lost message or a receive error is
  logged (`ipc_log`). ui.log's first `ipc:` line measures it on the console: how many 4000-byte
  messages a pair whose receiver reads nothing takes with the default room and with 256 KiB, and the
  error that follows. (The OpenOrbis `sys/socket.h` has Linux's `MSG_*` values: `MSG_DONTWAIT` is
  FreeBSD's `MSG_WAITALL` and `MSG_NOSIGNAL` FreeBSD's `MSG_NBIO`; don't pass those flags.)
- **A page that does not answer (v00.15).** WebKit marks the web process unresponsive when an input
  event, a reload or a load has had no answer for 3 s; ui.log says when it stops and answers again.
  5 s after that (8 s without an answer), the UI asks the web process over the control socket
  (`WISP_MSG_DUMP`) for every thread's stack in `logs/web.hang.log` (`watchdog`), and Reload (F5,
  Ctrl+R, the button, the menu), a typed address or Home ends the web process and starts it again in
  that tab (`restart_hung_page`; the old process goes first: two would not fit in the shared pool).
  The other tabs then say "stopped working" (Cross reloads them).
- **v00.15 on .43 (three runs).** ui.log's `ipc:` line measured the kernel: a pair whose receiver reads
  nothing took 2 messages of 4000 bytes with the default 8192 bytes of room, then EMSGSIZE; with
  256 KiB, 65, then EMSGSIZE again (never EAGAIN). Searches, reloads, a site's pages and two downloads
  worked; the hung-page restart ran once (the web process stopped answering 101 s in, a typed address
  10 s later ended it, the new one was up in 354 ms and loaded it).
- **Cloudflare's check (v00.15 on .43, a download site).** Turnstile's frame
  (`challenges.cloudflare.com/.../turnstile/...`) ran on the one web process's main thread for 90 s
  and never passed; the main loop stopped for 8 s and more twice (hang dumps: the main thread in page
  JavaScript, a Worker's message handler and a timer with a garbage collection, all in the LLInt
  interpreter: no JIT). The browser froze because every tab shares that thread. What Turnstile saw:
  it called `console.debug/log/dir/dirxml/table/trace/warn/error` with objects and invisible
  `%c` styles (it looks for a console that formats them: Wisp printed every message to web.log),
  WebKit's developer extras on, an empty `navigator.platform` beside a Safari-on-macOS user agent,
  one processor (WTF had no way to count the PS4's), UTC whatever the console's zone, and no
  `HTMLVideoElement`, `AudioContext` or WebAssembly, which every Safari has. v00.16: page messages
  off by default (`console_log`), developer extras only with the inspector, `navigator.platform`
  from the user agent ("MacIntel", WebKit patch 0020), 7 processors (`WTF_numberOfProcessorCores`),
  the console's time zone (ICU's default zone and `TZ`, from Settings > Date and Time), and the media
  stand-ins kept out of the checks' own frames. Still missing for such checks: the media and Web Audio
  classes (WebKit built with GStreamer), WebAssembly, and the speed a JIT gives.
- **Cloudflare's check again (v00.18 on .43, the same site).** With media in (v00.17) it still never
  passed: two rounds about 100 s apart, the web process's main loop stopped for 9 s and more in each
  (page JavaScript in the LLInt). net.log: `brunhild.challenges.cloudflare.com` did not resolve,
  twice. That name has only AAAA records (IPv6), and the console resolves and connects over IPv4
  alone (every "IPv6 DNS error" in net.log), but Cloudflare's troubleshooting page says failed
  requests to `*.challenges.cloudflare.com` subdomains are part of Turnstile's normal run and do not
  block anyone (every IPv4-only network has them). What is left beside a Safari user agent:
  WebAssembly (v00.21: WebKit built with it, run by JavaScriptCore's in-place interpreter, IPInt,
  without a JIT: `scripts/build-webkit.sh`, menu: WebAssembly), WebGL (not built) and the speed of a
  JIT.
- **Cloudflare's check passes, slowly (v00.24-26; user: "it works, but it takes a lot of time").** v00.24 on
  .43: the site's check page came at 34 s and its `?__cf_chl_tk=` redirect (passed) at 170 s; the web
  main loop stopped 9 s at a time (45 s, 115 s) in page JavaScript on the LLInt, and `brunhild` failed
  to resolve in 20 ms (not the wait). v00.27: JavaScriptCore's JIT in the web process (below).
- **Media stand-ins (v00.14-15): never reached a page.** `new_tab()` added them to the tab's content
  manager, then the extensions' layer set that tab's scripts, which starts by removing every script.
  v00.16: the extensions' layer asks the browser for its own scripts each time
  (`ext::Browser::page_scripts`).
- One web process for every tab (WebKit's single-web-process configuration, no process swap on
  navigation, no site isolation): the PS4 gives an app 7 local processes and one GPU address space.
- **A second web process ended the app (v00.20 on .43).** Typing youtube.com on a tab at retroarch.com:
  YouTube's `Cross-Origin-Opener-Policy: same-origin-allow-popups` made WebKit switch browsing
  context groups (`triggerBrowsingContextGroupSwitchForNavigation`); `processForSite` passes over a
  process that runs service workers even in single-process mode, so it launched a second web process
  while the first lived; the PS4 refused it (`sceSystemServiceAddLocalProcess` 0x80aa001a, with
  ~101 MiB of the shared pool left and a 125 MiB web image to load), and WebKit's launcher answered
  with `g_error`, which ended the UI process and the app. WebKit patch 0023: a refused launch is a
  process that ended (its pages say "stopped working"); single-process `processForSite` keeps the one
  process whatever it runs; a COOP switch to an isolated group stays in it too. And service workers
  and shared workers are off (`service_workers`): WebKit runs a site's workers in a process of that
  site, so every site's workers would want a process of their own, and one process can hold only one
  site's worker connection (`SWContextManager::setConnection` stops the last).
- **v00.21 on .43: the workers were still on.** A WPE feature's identifier is its preference's name
  without "Enabled" (`ServiceWorkers`, `SharedWorker`); v00.21 compared `ServiceWorkersEnabled` and
  matched nothing. YouTube's service worker (registered in earlier runs) then asked for a process of
  its own at each visit: on GPU rendering the PS4 refused it 225 times in 200 s (0x80aa001a, 97-111 MiB
  left; the app lived on, patch 0023, but loads failed and the UI's main thread spent ~0.3 s a try), on
  CPU rendering (a 100 MiB image) it started with 7 MiB of the pool left and the YouTube load was
  interrupted. v00.22 finds them with `webkit_feature_list_find` and logs "service workers and shared
  workers off (2 of 2 features)"; and within 2 s of a refusal for room, Wisp does not ask the PS4
  again unless a process of that kind has ended (`wisp_launch_local`).
- The GPU address space goes to the app's first process; the UI draws on the CPU and gives it away
  (`ps4_release_gpu_space`, WinePS4 v00.71) so the web process can open RADV.
- Memory: the kit's heap in direct memory per process (4.5 GiB direct for the app); flexible memory
  (~1 GiB shared by the app's local processes) kept for code. JSC ran on the LLInt alone until
  v00.26; v00.27's web process has the JIT, its code in executable memory the kit's probe finds
  (system flexible memory on .90 and .43: "JavaScript JIT (v00.27)" below).
- **Out of the shared pool (v00.17).** The app's processes started with ~450 MiB of the flexible pool
  between them, and every program image, thread stack, shared buffer and WebKit page came out of it;
  the 4.5 GiB of direct memory held only the heaps. Now:
  - thread stacks come from each process's heap (the kit: `ps4_kit_set_thread_stacks_from_heap`; a
    16 KiB guard page under each, a stack freed 3 s after its thread ended; `stacks_in_heap=0` for the
    old way). v00.15's web process had 62 threads, 25 MiB of the pool.
  - WebKit's pages (JSC's structure heap, conservative roots and every other `OSAllocator` commit)
    come from direct memory: WebKit patch 0021 maps pages of 2 MiB direct-memory blocks where each
    commit is, finds a decommitted page's block with `sceKernelVirtualQuery`, unmaps it before giving
    it back (no page is ever mapped twice), keeps two empty blocks and gives the rest back to the
    kernel; flexible memory stays the fallback when direct memory runs out. A check before the first
    commit makes sure the kernel says where a mapped page is; `webkit_pages_direct=0` (environment
    `WISP_WEBKIT_PAGES=flexible`) for the old way. Tested on the PC against a fake kernel whose
    direct memory is a memfd (a page mapped twice would be the same memory twice): six threads of
    16 KiB structure-heap commits and reserve-and-release of any size, with every byte tagged and
    checked, in 256 MiB and in 24 MiB (running out) of direct memory. The pages in use count in the
    footprint WebKit's memory pressure handler reads, and each `memory:` line says them.
  - the heaps' caps: UI 768 MiB (the player's pictures, aria2's buffers, archives), web 1536 MiB, net
    256 MiB. (The kit made 1024 MiB at most until v00.19, whatever a process asked for: its sizes
    went 1024, 768 ... 128; now a larger `direct_max` is tried first.)
- **A bigger flexible pool: not this way (v00.19).** The pool is 448 MiB for the app's processes
  together (the kernel's default: the kit's crt has an all-null `_sceKernelMemParam`), while the app
  has ~4.5 GiB of direct memory. v00.19's eboot asked for 1 GiB in its process parameters
  (`_sceKernelMemParam`'s flexible memory size, the field the SDK fills): on .43 the app ended before
  its first line (no ui.log, no boot trace, no crash log; its imports were v00.18's). v00.20 asks for
  nothing again (`WISP_FLEXIBLE_MIB` 0 in `ui/main.cpp`; the code stays for a smaller try). (A first
  v00.19 build had its parameters `const`: internal linkage, `extern "C"` or not, so the crt's weak
  ones stayed; `nm` showed only the `W` symbol.)

## Graphics

- Web process: Mesa 26.0.8 (the kit's tree + Etterna's Zink branch: Zink on RADV, the fence-leak
  fix) built with **EGL (surfaceless platform) + GLES** as static libraries. WebKit's
  `PlatformDisplaySurfaceless`, Skia (Ganesh GL) paints tiles, TextureMapper composites.
- View transitions on CPU rendering (WebKit patch 0024, v00.22): a page drawn without compositing
  has no layers for a transition's pictures, and YouTube's `document.startViewTransition()` aborted the
  CPU web process (v00.21: `setActiveViewTransition` -> `enableCompositingMode` ->
  `graphicsLayerFactory`). Without compositing, the transition is skipped as for a hidden document (its
  update callback still runs) and no cross-document transition is set up.
- 2D canvases are drawn on the CPU (v00.19, `canvas_gpu=0`): v00.18's web process crashed on its main
  thread in Zink's `zink_resource_memory_barrier` under WebKit's accelerated canvas (kit crash log,
  "page stopped" on .43). The bug was Zink's, not the canvas's (v00.24: "GPU web process crash in
  Zink" under Media).
- GPU rendering (`gpu=1`, `wisp-web-gpu.bin`) never started on a console until v00.10: with no DRM
  device, Mesa's surfaceless EGL never tried Zink (it fell back to swrast, which is not built: "egl:
  failed to create dri2 screen" in every web.log), and Wisp disabled Kopper, the way Zink runs without
  DRM. Wisp's Mesa patch (`patches/mesa`) tries Zink through Kopper when no DRM device answers; Mesa's
  own threads ask for 2 MiB stacks there (c11 `thrd_create`), WebKit's get 512 KiB. web.log says how
  EGL started ("gpu: EGL ... on Zink", EGL's own lines with `gpu_log`). Without GL a web process
  draws on the CPU even when GPU rendering is on (WebKit patch 0013), so a failed start is CPU
  rendering, not a broken page.
- v00.10 on .43 (PS4 Pro): EGL on Zink on RADV started (0.25 s), but every web process aborted
  within seconds ("page stopped working"): 100 threads, 41 of them Mesa's at 2 MiB, emptied the
  shared pool a second after the start (JavaScriptCore's collector could not start a helper thread),
  and RADV's submission thread, started at the first submission, could not start either, so the GPU
  never saw a command ("GPU hang? waiting for 1, completed 0"). Mesa made 6 threads per Zink screen,
  4 per shader disk cache (three caches) and 2 per GL context (glthread and Gallium's threaded
  context; WebKit makes about ten contexts). v00.11: Mesa's threads 1 MiB, one thread per disk cache
  and for Zink's cache fetches, no glthread (`mesa_glthread=false`), the other threads 256 KiB again
  with WebKit's graphics threads (compositor, scrolling) at 1 MiB (patch 0015); RADV's submission
  thread checked, else submissions run on the caller's thread. Two web process ends within a minute
  with GPU rendering on set `gpu=0`: the next web process is the CPU one.
- v00.11 on .43: the GPU drew the first page (every submission completed, 32 threads instead of 100,
  EGL's start 8 MiB instead of 14), but both GPU web processes still aborted 15-40 s in, again in the
  collector's helper start (`ParallelHelperPool` -> `AutomaticThread::start` -> `Thread::create`);
  the CPU web process the fallback then started aborted at once: the page's surface takes shared
  memory only without `hardwareAccelerationEnabled`, which the GPU settings had on, so it asked for
  the GL display (`AcceleratedSurface::SwapChain`). v00.12: a web process without GL turns that,
  accelerated drawing, canvas acceleration and WebGL off too (patch 0016); the collector's helpers
  stay once started (patch 0017; each one that ended after 10 s idle was started again at the next
  collection); and every process logs the shared pool and what it holds of it (`memory:` lines:
  its flexible and shared mappings and its five largest by name, when the pool moves by 8 MiB and
  every 2 s under 48 MiB). Before v00.12 a single Google tab left ~25-45 MiB of ~170 in CPU mode
  as well: those lines are to say where it goes. The GPU web process has its own kit folder
  (`kit-web-gpu/`).
- v00.12 on .43 (CPU mode): the web process stopped answering while Google's search box was typed
  into (the last character never went, reloads and new tabs never started: no "load started" after
  7 s; the UI and network process went on). Its census said where the pool goes: the UI held 129
  MiB (image 89, its two frame buffers 14, threads at the main process's 2 MiB default), the web
  process 171-186 MiB (image 80, ICU's 30 MiB heap block in a flexible mapping, 14 MiB of frame
  buffers per tab, 2 MiB threads), the network process 114 (image 83, 2 MiB threads).
- v00.13: **rendering=hybrid by default** ("CPU + GPU", OPTIONS > Rendering cycles CPU + GPU, GPU,
  CPU): wisp-web-gpu.bin with `WEBKIT_SKIA_ENABLE_CPU_RENDERING=1`, Skia drawing the tiles on the
  CPU on `paint_threads` painting threads and Zink compositing, scrolling and animating them, as a
  browser does on a weak GPU; two web process ends within a minute set `rendering=cpu`. CPU
  rendering's whole-frame strips are off (`paint_strips`, default 1), the freeze's new suspect.
  ICU's archive in direct memory of its own (no flexible cost); thread stacks 256 KiB from the
  first thread on, the main process's 2 MiB default replaced (`ps4_kit_set_default_thread_stack`,
  kit change); a watchdog (`watchdog`): a main loop that has not run for 8 s gets every thread's
  stack written to `logs/<process>.hang.log`; the census counts thread stacks.
- v00.13 on .43 (hybrid): pages drew on the GPU and stayed up, but with three tabs the shared pool
  was 0-10 MiB for minutes and the app stopped answering ("Failed to allocate shared memory buffer";
  the web main thread did not run its loop for 8, 9 and 31 s). The census's mapping names
  (`anon:<caller>`, the caller's address with the image's 0x400000 base taken off, symbolized
  against the kept ELF) said what held the pool besides the three images (UI 89, web 106, network 83
  MiB): in the web process ICU's own mapping of its whole archive (30 MiB: at the first lookup that
  missed in Wisp's copy, ICU's `extendICUData` mapped the file from `ICU_DATA` as "extended data"),
  every font file a page used (Skia maps them: 23 MiB in 11), cached resources the network process
  sent as shared memory (17 MiB in 26), and two or three 7.5 MiB frame buffers per tab in both the web
  process and the UI (a hidden tab gave its spare buffers back 10-20 s after it was hidden, and the UI
  kept its last one mapped). YouTube stayed empty: its app script stopped at `HTMLVideoElement`
  (`ReferenceError`), which a WebKit built without media does not have. Hidden pages ran their
  timers and animations at full speed on the one web process's main thread: WPE leaves WebKit's
  hidden-page throttling off (GTK and Cocoa turn it on). The watchdog's dumps had every thread but the
  main one (the kit dumps the threads it watches, and it watched those started by `pthread_create`).
- v00.14: ICU's data directory is one without the archive once Wisp's copy is in, and ICU looks in
  packages only; Skia reads font files into the heap (direct memory) instead of mapping them; the
  network cache sends cached bodies inline (no shareable resources); a hidden page gives every frame
  buffer back a second after it is hidden, also those still with the UI as they return, in CPU mode
  too (patch 0018); the UI lets go of a buffer the web process destroyed and says a hidden tab's frames
  rendered at once; hidden pages' timers are aligned to a second and their CSS animations stop; a tab
  does not open with less than `tab_memory_floor` MiB left (a toast says so); `media_stubs`: stand-ins
  for `HTMLMediaElement`, `HTMLVideoElement`, `HTMLAudioElement` and `Audio` that say a browser that
  plays nothing (`canPlayType` "", `play()` refused), so pages that name them go on; the watchdog dumps
  the main thread too.
- CPU rendering (default): no compositing; WebKit's `NonCompositedFrameRenderer` paints only the
  damaged rectangles of each frame with Skia. v00.10 (WebKit patch 0014): the frame is recorded once
  (an `SkPicture`, as WebKit's composited CPU path records its tiles) and played back in horizontal
  strips, one on the web process's main thread and the others on painting threads (`paint_threads`,
  default 4): the main thread alone, one 1.6 GHz Jaguar core, was the limit. Smooth scrolling is off
  on the CPU (`smooth_scrolling`): each animation step is a whole new picture.
- Frames reach the UI as shared-memory buffers (`WPEBufferSHM`: the web process reads its frame back
  into shm when the display offers no DMA-BUF formats), with WebKit's damage rectangles. The UI copies
  only the damaged part into its own copy of the page, and composes the screen row by row: the page's
  damaged rows, the toolbar when it changed, and the rows the overlays and the pointer covered; each
  of the two VideoOut buffers gets only the rows it does not have yet. Composing follows the TV:
  right after each vblank (a thread waits on VideoOut's vblank event and wakes the main loop), or as
  soon as a new page frame comes while no flip is waiting; the main loop never waits for a flip
  (before v00.10 a 16 ms timer, not in step with the TV's 16.7 ms, waited for the last flip each
  time: uneven frames and late input). `vsync_pacing=0`: the old timer. The frame source runs at
  `G_PRIORITY_DEFAULT` since v00.15, the priority of WebKit's own work on the UI's loop (its messages
  from the web and network processes): GLib runs only the highest priority that is ready, and at
  `G_PRIORITY_HIGH` frames that took a whole vblank held WebKit's work back.
- WebGL (ANGLE on that EGL) and video (GStreamer + FFmpeg; the PS4's video decoder later) come after
  the first pages. Until then WebKit is built without media (`ENABLE_VIDEO=OFF`), and the
  `media_stubs` stand-ins keep pages that name the media classes going (YouTube's page, not its
  videos).

## Text in every script

- Pages: fontconfig (`data/fonts.conf`) over Liberation (Arial, Times New Roman and Courier New
  metrics), Roboto (system-ui) and Noto for every other script: 194 files, 77 MB in
  `/app0/wisp/fonts/noto` (`scripts/get-noto-fonts.sh` fetches them into the build tree once;
  OFL). Arabic, Hebrew, Syriac, Thaana, the Indic scripts, Thai, Lao, Khmer, Myanmar, Tibetan,
  Ethiopic, Armenian, Georgian, CJK (one collection, Japanese/Korean/Chinese faces), color emoji
  (CBDT; FreeType has PNG), symbols, math and the rest of Noto. The PS4's own fonts (`/preinst`, SST
  Arabic among them) are not readable by an app: Arabic showed as boxes through v00.09.
- WebKit's fallback asks fontconfig for every font sorted for the text's language and takes the
  first that has the characters: fonts.conf orders them (Arabic and Persian: Noto Naskh Arabic, then
  Noto Sans Arabic; Urdu: Nastaliq; Japanese/Korean/Chinese: their CJK face; emoji: Noto Color
  Emoji), then one list for every language. Checked on the PC with fontconfig's tools.
- WOFF2 web fonts: WebKit is built without the woff2 library and skipped every `format("woff2")`
  source (icon fonts showed their names or nothing); FreeType reads WOFF2 itself (brotli), so WebKit
  accepts it on the PS4 (patch 0013).
- The toolbar, Files and the menus (`ui/shell/ps4_canvas.cpp`, stb_truetype): fonts for the other
  scripts come from the same Noto set when a character first needs one; text with Arabic, Hebrew or
  Indic characters is laid out by HarfBuzz in ICU's bidirectional order (joined, right to left), at
  the Latin font's em size. Checked on the PC (`ps4_canvas.cpp` built for Linux with HarfBuzz and ICU).
- **Arabic marks (v00.19, WebKit patch 0022).** v00.18's screenshots: Arabic's vowel marks and dots
  sat at the wrong height and joins broke where the font steps a letter up or down. Two faults in
  WebKit's Skia text path: HarfBuzz's `y_offset` is up-positive and WebKit's
  glyph buffer is y-down (`ComplexTextControllerSkia.cpp` passed it unflipped), and
  `FontSkia.cpp`'s text blob used horizontal-only positions (`allocRunPosH`), dropping every
  vertical offset; now a run with any vertical offset gets full positions (`allocRunPos`).

## The browser UI (UI process, drawn on the CPU)

WinePS4's desktop look (`WinePS4/shell`: HENkaku's canvas, acrylic panels, the console's fonts):
tab strip, address bar with suggestions, back/forward/reload/home, extension buttons and popups,
downloads, menu, settings, history, bookmarks; DualShock 4 pointer (left stick), scroll (right
stick), USB keyboard and mouse, the PS4 keyboard (`sceImeDialog`) for text fields. Scrolling: up on
the stick or the wheel moves the page up (v00.19; through v00.18 both went the other way: WPE's
scroll events take WebKit's sign, positive up and left, and Wisp passed a downward delta as is).
The toolbar's address, Home and menu (the three dots) work on every tab, Files', Downloads' and the
player's too (v00.22; through v00.21 only a page's tab answered them: Home and a typed address there
open a new tab). About Wisp (the menu; `wisp://about`, `about:wisp`; v00.23): Wisp's version, its
maker (iHaiDeeZ, github.com/iHaiDeeZ), this console's drawing, JavaScript and memory, and what Wisp is
built on with their licenses, a page Wisp serves itself (`serve_wisp_page`, a URI scheme of its own).

The pointer is a desktop's (`ui/cursor.cpp`): an arrow, a hand over links, a text beam over text,
whatever CSS cursor the page asks for (`none` hides it), drawn smooth with a shadow at
`pointer_size` percent.

A USB mouse (`libSceMouse`, every mouse as one) works as on a computer: left click, right click (the
page's own menu: WebKit proposes the items, `ui/browser.cpp` draws them and activates their
actions), middle click (a link in a new tab), the side buttons (back, forward), the wheel (Shift:
sideways, Ctrl: zoom) and its tilt. A USB keyboard (`libSceIme`'s keyboard, the layout's characters
from the system) types into the page's fields itself: the system keyboard opens only without one.
Its keys go to WebKit with their X11 hardware codes (`KeyboardEvent.code`), and the address field is
typed in place (Ctrl+L, F6, Alt+D or a click; Ctrl+A/C/X/V with WPE's clipboard). Shortcuts: Ctrl+T,
Ctrl+W, Ctrl+Tab, Ctrl+Page Up/Down, Ctrl+1-9, Alt+Left/Right, Alt+Home, F5 / Ctrl+R
(Ctrl+Shift+R: no cache), Esc, F11, Ctrl + / - / 0; arrows, Enter and Esc in the menus.

**Files** = WinePS4's Files window (`shell/desktop.cpp` "files" + `shell/fileops.*`): opens on
Downloads (`/data/Wisp/Downloads`), browses `/data`, USB drives, `/mnt/usb*`; copy, move, rename,
delete with progress; opens pictures, text, PDFs, video/audio and web files in a tab (a binary file of
any other kind says OrbisWeb can't open it, v00.28); `.crx`, `.nex`, `.xpi` and `.zip` extensions
install from there. Since v00.16 it is a tab of its own (OPTIONS >
Files): another tab shown hides it, its tab shown again brings it back where it was; L1/R1, OPTIONS
and the toolbar stay the browser's while it is up; closing Files (Circle at This PS4, Ctrl+W) closes
its tab.

Files as Explorer (v00.17):
- **Archives** (`ui/files/archives.cpp`, libarchive: `scripts/build-archive.sh` into
  `~/wisp-build/archive` with zlib, bzip2, xz, zstd and lz4). An archive opens as a folder, as Windows
  opens a compressed folder: ZIP (and .jar, .apk, .cbz), RAR 4 and 5, 7-Zip, tar and its .gz, .bz2,
  .xz, .zst, .lz4 and .lzma forms, ISO 9660, CAB, LHA, cpio, ar, XAR, and a single compressed file
  (.gz, .bz2, .xz, .zst, .lz4, .lzma, .Z: one file inside). Its path goes on from the archive's
  (`/data/Wisp/Downloads/photos.zip/2024`); it is read-only there. Its list is read on a thread
  ("Reading photos.zip..." meanwhile). A file opened inside it is taken out to
  `/data/Wisp/tmp/opened` (emptied at the first open of a run) and opened as any file; Copy then
  Paste, a drag, or "Extract to <folder>" take entries out. On an archive: Extract All (a new folder
  named after it, then shown) and Extract here (a name taken there becomes "name (2)"). On anything
  else: Compress to ZIP file / Compress to 7z file (LZMA2 at 7-Zip's Normal), named after it. Sets
  open from any part: `name.part1.rar`..., `name.rar` + `.r00`..., `name.7z.001`... ZIP's passwords
  (ZipCrypto and AES) are asked for and kept for the run. RAR goes to RARLAB's UnRAR library
  (v00.27, `scripts/build-unrar.sh`): every RAR version, passwords for encrypted files and encrypted
  lists, sets of parts; libarchive decrypted no RAR at all. 7-Zip's encryption is still not supported
  (libarchive's) and says so. Names a ZIP holds without its UTF-8 flag, and RAR's older non-Unicode
  ones, are read in the DOS code page (CP437), as 7-Zip reads them. libarchive's build
  (`scripts/build-archive.sh`) compiles fork and spawn out and carries five PS4 patches (an ENOSYS
  `umask()`, a symlink check that changed folder and never came back, ...). An entry's path is cleaned
  before it is written (no leading
  `/`, no `..`): an archive can't write outside the folder; symbolic links and devices are left out.
  Only the decompressors linked in are registered: libarchive falls back on an outside program
  for the rest, and the PS4's `fork()` starts none. The UI process runs in the C.UTF-8 locale: names
  go through the locale's character set, and in the C locale libarchive's tar writer crashed on a
  non-ASCII name (tested on the PC: 68 checks on libarchive 3.8.11 - archives made by zip, 7z and
  bsdtar taken out and compared, chosen entries, Extract here twice, opening, ZIP passwords none /
  wrong / right, entries pointing outside, a single .gz, ZIP / 7z / tar.gz made here and checked by
  unzip, 7z and bsdtar, Cancel). v00.18-21 on the console: opening an archive in Files did nothing
  ("Opening : done"), nor did Extract All, Extract here, or a copy, paste or drag of the archive
  itself; only the Downloads page's open (the archive's path with a `/`) worked. `in_archive()` says
  yes for an archive's own path (its top, as a folder), and those four asked it of the chosen
  archive: they now ask `inside_archive()` (in one, not the archive itself), v00.22.
- **The Recycle Bin**: Delete moves things to their drive's bin (a rename: /data's is
  `Wisp data/Recycle Bin`, a USB drive's `.wisp-recycle` at its top), each with a record of where it
  was and when; the Recycle Bin place lists them by their own names with the date they were deleted;
  Restore puts them back (their folder made again if it went; "name (2)" if the name is taken),
  Delete there and Empty Recycle Bin delete for good. Shift+Delete and "Delete for good" skip it, and
  so does anything with no bin (outside /data and the USB drives) or holding one (asked first).
  Dropping onto the Recycle Bin deletes.
- **Jobs** (copies, moves, deletes, archives) have Cancel on their card: they stop at the next file
  or block, and what was being written is removed.
- New text document, Select none, Invert selection; "Date deleted" in the bin.
- A downloaded archive opened from Downloads opens in Files as a folder (a `.zip` that is a browser
  extension still installs).

**Downloads** (v00.17, `ui/dm`): a download manager in a tab of its own (OPTIONS > Downloads), on
aria2 (libaria2, the engine of Motrix; `scripts/build-aria2.sh` builds it into `~/wisp-build/aria2`)
running on a thread of its own. libaria2 is not thread-safe: its thread runs aria2's event loop one
turn at a time (`aria2::run(RUN_ONCE)`), applies the page's commands between turns and twice a second
copies every download's state into a snapshot the page draws. A page's download (WebKit's
`decide-destination`) is cancelled in WebKit and given to aria2 with the page's cookies for the link
(`webkit_cookie_manager_get_cookies`), the page as Referer and Wisp's user agent, as desktop download
managers take a browser's downloads: up to `dm_connections` (8) connections a file, `dm_parallel` (3)
files at once, resumed after a stop or a restart of Wisp (aria2's session file and control files in
`/data/Wisp/aria2`). BitTorrent: `.torrent` links and `magnet:` links (caught as navigations), DHT
over IPv4, no seeding unless `dm_seed_minutes`. What aria2 cannot fetch (`blob:` and `data:` links, an
extension's package) stays with WebKit's downloader, and a page's download a server refuses aria2
(HTTP 403/401/404 before a byte) goes back to it; those show in the same list. Finished downloads are
kept in a history file. The page: Cross pause / resume (open a finished one), Square removes,
Triangle adds a link (http, https, ftp, magnet, .torrent), buttons for each with a mouse. Since v00.29
the page names the folder downloads go to, and Change the folder chooses another (below).
`download_manager=0`: WebKit's downloader for everything, as before.

**Wisp Player** (v00.17, `ui/player`): video and music from Files or a finished download in a tab of
its own, on FFmpeg (`scripts/build-media.sh`: `~/wisp-build/media`) on the CPU: decoding on a thread
of its own (FFmpeg's frame and slice threads, `player_threads` 4) that also turns each picture into
the screen's format at its shown size (swscale), the sound on the PS4's audio out (48 kHz stereo,
1024-frame blocks) on another, whose clock the pictures follow (a late picture is skipped; many late
in a row and reference-less ones are not decoded until it catches up). Subtitles: the file's (text and
picture) and a `.srt` / `.ass` / `.vtt` of the same name beside it. Where a file was left is kept
(`/data/Wisp/player/resume.tsv`); at the end the folder's next file plays (`player_next`). Keys as
MPC-HC's where a console has them (Space, arrows, M, F, A, S, Home, Page Up/Down), the controller's
Cross / D-pad / Square / Triangle / Circle, a mouse's click, double click and wheel.

**Wisp Player as MPC-HC (v00.23).** The user asked for "a full functional home cinema app ... where you
can locate subtitle, choose server, add aspect ratio, and so on, similar to MPC-HC". MPC-HC itself is
DirectShow and Direct3D on Windows; its features are rebuilt on the player's FFmpeg instead:
- **The menu** (Square, the mouse's right button, the menu key): MPC-HC's right-click menu, cascading,
  made afresh each time it opens. Play / Pause, Stop, Open file, Recent files (`player/recent.txt`);
  Navigate (Go to a time, Chapters from the file, the folder's files); Play (Speed 0.25x-4x: the sound
  is resampled, so its pitch moves with it, as a tape; When it ends: the next file, again, stop,
  close, `player_after`); Audio (tracks, delay, a boost to 400% clipped at full scale, mute);
  Subtitles (tracks, Load subtitle file, Find subtitles online, Encoding, Delay, Size, Position, a
  box); Video (Aspect ratio 4:3 to 2.39:1, Video frame: fit / fill and crop / stretch / original size,
  Zoom 25-400%, Brightness / Contrast / Saturation through swscale's YUV conversion); Save image
  (a JPEG of the picture in `Downloads/Screenshots`); Information (MPC-HC's Statistics: container,
  codecs, sizes, frame rate, frames shown and late, buffers); Full screen; Close. Steps (+ / -) keep
  the menu open. MPC-HC's keys too (Ctrl+O, Ctrl+L, Ctrl+G, Ctrl+S, Ctrl+Up/Down/R, + / -, F1 / F2,
  numpad + / - / 5, Tab): `ui/player/player.h`.
- **The picture**: only the part that shows is scaled (the crop applied to the frame first:
  `av_frame_apply_cropping`), so fill and zoom cost no more than fit.
- **Subtitles from anywhere** (Load subtitle file): a picker in the player (the video's folder,
  Downloads, /data, USB drives), archives as folders (zip, rar, 7z: what subtitle sites give; read with
  libarchive). Files are read into UTF-8 first: UTF-8 (a BOM dropped), UTF-16 by its BOM, else the
  encoding chosen (the console's C library's iconv has Windows-1250 to 1258, ISO-8859-6, GB18030,
  Big5, Shift_JIS, EUC-KR); Automatic takes text that is not UTF-8 for Arabic (Windows-1256) when 30%
  and more of its letters are high bytes, else Western (Windows-1252). Another encoding reads the
  file again. FFmpeg parses the result (this build's demuxers: SubRip, ASS / SSA, WebVTT); ASS styles
  are not drawn (the text only, through the canvas's HarfBuzz and ICU bidi: Arabic joins and runs
  right to left).
- **"Choose server"**: MPC-HC's subtitle database servers. Find subtitles online opens the site chosen
  (OpenSubtitles.org, Podnapisi, SubDL, Addic7ed, or a web search) in a tab, searched for the video's
  name (dots and underscores as spaces, up to its year or episode, without release tags); what it
  downloads goes to Downloads, where Load subtitle file takes it, zip and all. (No site API: each
  wants its own key and account, and their sites change less often than their APIs.)
- **Not done**: opening a URL (the media FFmpeg is built without network protocols), pitch-kept speed
  (FFmpeg's atempo filter is not built, and the UI links no libavfilter), deinterlacing, ASS styling
  (libass is not built).

**Video and sound on pages** (v00.17): WebKit built with GStreamer (`USE_GSTREAMER_FULL`: GStreamer
1.26.11's libraries and plugins in one static library, `scripts/build-media.sh`) and FFmpeg 8.0.3's
decoders through gst-libav, all on the CPU: `<video>`, `<audio>`, Media Source (YouTube) and Web
Audio. No AV1 decoder (FFmpeg's is hardware-only); YouTube is held to H.264 at 720p30 or less
(`video_h264_only`, `video_max_height`), which Jaguar's four decoding threads keep up with. Sound:
`ps4audiosink` (`child/gst_ps4_audiosink.c`), a GstAudioSink on the PS4's audio out (48 kHz stereo,
256-frame blocks) registered above every other sink, so WebKit's `autoaudiosink` takes it. The web
process starts GStreamer itself after its fonts (AudioOut and UserService loaded first: a call into a
module that is not loaded ends a process without a word) and says so in web.log ("media: GStreamer
1.26.11 with N plugins"); GStreamer's warnings go to `logs/gstreamer.log` (`gst_log`). FFmpeg's
transform tables from 2048 points up are allocated when first used (16 MiB less in each image that
links FFmpeg). With media, the media stand-ins (`media_stubs`) do nothing.

**YouTube on v00.22 (.43): "too slow in loading, and has no audio".**
- **No sound:** WPE's own autoplay policy is `WEBKIT_AUTOPLAY_ALLOW_WITHOUT_SOUND`, and YouTube's
  player starts its video from a script after the click, not in the click: it played muted. Every
  view now gets website policies with autoplay allowed (`autoplay=allow|muted|deny`). The sink logs
  each port it opens and, as it closes, how many blocks it played and how many were silence (a page
  sending only silence was muted), so the next run tells a muted page from sound the PS4 dropped.
- **Slow:** the YouTube tab went to sleep a second after Downloads came up (the pool at its usual
  54 MiB under `tab_sleep_floor` 64, then a 677-820 MiB web heap over `tab_sleep_heap` 700) and loaded
  from the start each time it came back: three times in one run. Now 40 MiB and 1200 MiB, and the
  page shown last never sleeps for memory while Files, Downloads or the player is in front. The rest
  is JavaScript on the LLInt: YouTube's scripts held the web process's main thread for seconds at a
  time (hang dumps), and H.264 at 720p dropped frames for QoS while they ran.
- **Previews:** WebKit's progressive player buffers to `/var/tmp/WebKit-Media-XXXXXX`, which the PS4
  lacks: every preview on YouTube's home page stopped ("Could not create temp file"). WPE reads
  `WPE_SHELL_MEDIA_DISK_CACHE_PATH`: the web process sets it to `/data/Wisp/cache/media`.
- The GPU web process crashed again in `zink_resource_memory_barrier`, with 2D canvases on the CPU
  since v00.19: see the next paragraph (v00.24).

**GPU web process crash in Zink (v00.24).** The three crashes of the GPU web process since v00.18
(v00.18's accelerated canvas on the main thread, twice; v00.22 on a YouTube video, on the compositing
thread) are one bug: `zink_update_barriers` took a buffer from the context's barrier set
(`need_barriers`) after the buffer had been freed. `res->obj` (offset 0x108) was NULL, which
`zink_resource_destroy` writes just before it frees the resource, and the fault address 0x3d0 is
`obj->bo`; in the third crash the memory had been reused (0x473). Mesa 26's buffer release
(`pipe_context::resource_release`) lets a context's binds of a buffer hold no reference: a release
while the buffer is bound waits for the last unbind (`deleted`), the release takes the buffer out of
the barrier sets of the releasing context only, and `deleted` was a bool that stayed set, so a buffer
bound again after its release was released once more at its next unbind. WebKit makes about ten GL
contexts in one share group (a compositor's for each page, Skia's), and TextureMapper even keeps one
GL buffer static across all of them. Two of the crashes came seconds after the page stopped answering
(the main thread waiting for the compositor's scrolling-tree lock). Wisp's Mesa changes
(`patches/mesa/wisp-egl-uncommitted.patch`; `zink_context.c`, `zink_resource.c`, `zink_types.h`,
`u_upload_mgr.c`):
- `deleted` is cleared when the release it held back happens, so a later unbind does not release
  the buffer again;
- a buffer whose last reference goes while a context still has it bound stays until that unbind;
- every resource carries a live tag that `zink_resource_destroy` clears, and `zink_update_barriers`
  drops a freed resource from its set without reading it;
- `u_upload_mgr` no longer keeps a buffer it has released when mapping a new one fails.
web.log says when a guard catches one ("zink (Wisp): ...", the first 8 times, then every 64th, with
the code address that dropped the last reference; `gpu_log`), so a run tells which path frees them.
The upstream cause is not pinned down, so these are guards, not a fix of the cause. v00.24's run on .43
answered which path it is: the guard skipped 8 freed resources, each freed by `zink_sampler_view_destroy`
(a texture whose last reference was a sampler view's, while another context's barrier set still held it:
WebKit's contexts share textures), and the GPU web process did not crash.

The UI and network processes play nothing, but WebKit's code they keep reaches `gst_init`
(`GraphicsContext`'s vtable is live in both, and its `drawVideoFrame` takes a frame's picture through
GStreamer; `ImageDecoderGStreamer` too), and `gst_init` registers every plugin and with them FFmpeg's
decoders: v00.17's first build had the UI at 112 MiB and the network process at 102. Their links send
`gst_init` / `gst_init_check` to stubs (`common/no_media.cpp`, `--wrap`), which leaves ~0.7 MiB of
GStreamer in each (found with LLD 21's `--why-live=gst_init_check`; OpenOrbis' ld.lld is too old for
it). v00.17's images: UI 108.0 MiB (v00.16: 89; Wisp Player's FFmpeg, aria2 and its OpenSSL), web
98.9 (CPU) / 125.1 (GPU) MiB (v00.16: 80 / 106; GStreamer and FFmpeg), network 88.1 MiB (83).

**The memory meter** (v00.16, `memory_meter`) at the tab strip's right end: "RAM" is what the three
processes use (their heaps in direct memory, plus what they take of the shared flexible pool) out of
the app's memory in all (v00.19: direct memory plus the pool, ~4.9 GiB; v00.16-18 showed the pool's
448 MiB as the whole), the bar that share, then "pool N MiB free", amber under 96 MiB left and red
under 48 (the pool running out is what freezes the app).

**Sleeping tabs** (v00.16). Every tab's page lives in the one web process: its DOM, its scripts'
objects (each garbage collection walks them all, whatever tab they belong to), its timers. A tab
hidden `tab_sleep_after` seconds (600), or the longest-hidden one while the pool has less than
`tab_sleep_floor` MiB (40; 64 until v00.22) or the web process's heap more than `tab_sleep_heap` MiB
(1200; 700 until v00.22), sleeps, except for memory the page shown last while a tab with no page
(Files, Downloads, the player) is in front:
WebKit's session state (its history, with scroll positions and form fields) and its last picture go
to `/data/Wisp/run/tabs` (the picture on a thread of its own), and its web view goes, so its page closes
in the web process. Its tab shows a grey dot and a dimmer title. Shown again it wakes: a new view
with the same content manager gets the session state back and goes to the page it was on (the
network cache serves what it can), with the saved picture on the screen at once until the page
draws. A sleeping tab is a discarded one to extensions (removed, created again when it wakes).

**Closed tabs' memory (v00.25).** v00.24's run on .43 (20 minutes, YouTube and download sites): the
web process's heap only grew (158 MiB at 31 s, 615 at 3 minutes, 990 at 20 minutes, 1055 before a
restart) and never went down as tabs closed or slept (990 MiB before and after a tab closed at
1174 s). "Heap in use" is the kit's count of live allocations, not the allocator's footprint, so the
memory really was held. WebKit lets memory go when its `MemoryPressureHandler` says so, and on the PS4
it never did: `WebProcess::initializeWebProcess` starts the periodic memory monitor only
`#if !USE(SYSTEM_MALLOC)`, and this build uses the system malloc (the kit's heap); the UI process's
system monitor reads `/proc`, which the PS4 lacks. So no cache was ever pruned and no garbage
collection ever forced for memory, and a closed page's objects waited for JSC's own next full
collection, which a big heap puts off for long (`WindowProxy`'s "collect soon" only nudges JSC's
timer). WebKit patch 0025: the monitor runs on the PS4 (`memoryFootprint()` already read the kit's
heap through `wtf/ps4/MemoryPS4.cpp`); the app sets its thresholds through `WebKitMemoryPressureSettings`
on the web context: `memory_limit` 1536 MiB (the web process's heap), past 40% (614 MiB) the caches
let go of what no page uses, past 60% (922 MiB) also garbage collection, compiled code and the
back/forward cache; checked every 15 s; kill threshold 10x (never: WebKit's own default would end
the web process at 90% when no page is visible, and every tab shares it). And a page that leaves the
web process (a tab closed or asleep) is followed 2 s later (once for several) by a non-critical
release and a full collection on the next turn of the run loop (`memory_release`; web.log: "memory:
a page left the process ..."). The same run's other findings: the Zink guards (v00.24) caught 8 freed
resources in barrier sets, each freed by `zink_sampler_view_destroy` (a texture's last reference was
a sampler view's, in another context than the one whose set held it), and nothing crashed; YouTube's
sound played (one port 130 s: 24,130 blocks, 3 silent); the pages that stopped answering for 8-10 s
were running their scripts on the interpreter (event listeners, a Worker's messages, microtasks, XHR
callbacks: hang dumps), not stuck. The crash logs' net sections and the GPU web process's fifth are
older builds' (the kit's crash.log is never cleared).

**Closed tabs never closed (v00.26).** v00.25's run on .43 ("the pool still low, YouTube does not load
videos, the browser seems heavy, no audio on YouTube") had no "memory: a page left the process" line at
all, though two tabs closed and one slept: `WebProcess::removeWebPage` never ran, so no page ever left.
The UI made each tab's view with `g_object_new` and then `g_object_ref_sink`, a habit from GTK, where a
web view is a floating widget; WPE's `WebKitWebView` is a plain `GObject`, so the sink added a second
reference, and closing a tab (or putting it to sleep, or closing an extension's popup) dropped only one.
No closed tab's page ever went: it kept running its scripts, kept drawing (activate() only hides the
tabs it still has, so a closed active tab stayed "visible" to WebKit), and kept its two 7.1 MiB frame
buffers mapped in both processes (28 MiB of the pool each: the run's pool sat at 54 MiB with one tab on
show instead of 83). A closed YouTube tab still finished loading 0.4 s after it closed, and its players
and the reloaded page's held all 8 audio ports the PS4 gives an app; from then on every player's sink
failed ("sceAudioOutOpen: 0x80260005", port full), and WebKit ends a player whose sink fails: YouTube's
videos never started (players 9-18 in gstreamer.log), hence no video and no sound. v00.26: the views
keep the one reference `g_object_new` gave (popups too: the "create" signal's return passes through
`g_value_take_object` and the caller's copy, net zero, and WebKit keeps no reference of its own); a
closed tab's view is hidden before it goes; ui.log says "tab id N: its view is freed" when one really is
(memory_log). The audio sink now mixes every player into one port (child/gst_ps4_audiosink.c): each
sink queues 4 blocks (21 ms) as a voice, a mixer thread (priority 400) adds the voices block by block
into one port, opened when there is sound and closed after 3 s without; with no port to be had, the
mixer keeps the port's pace so players go on silently and get sound when one opens (a sink never fails
now). web.log: "audio: ... open (port N ...; every player's sound mixed into it)", "a player's sound
ended after X s: N blocks mixed". Frames, WebKit patch 0026: the UI maps a page's shared-memory buffer
only while it copies a frame of it (`AcceleratedBackingStore` keeps the handle and maps a duplicate per
frame; the WPEBuffer made for the frame goes with its mapping once the view has copied it), and the web
process starts each page's swap chain with one buffer instead of two (the UI gives a buffer back before
it says the frame is done, so the next frame finds it free; a second is still made when not). A page on
show costs the pool 7.1 MiB instead of 28.4.

**JavaScript JIT (v00.27).** Cloudflare's check passed but took minutes (above), and every heavy page
stalled the one web main thread: all JavaScript ran on the LLInt interpreter since v00.07 dropped the
JIT tiers for image size. Now the web process links a second WebKit build with JavaScriptCore's JIT and
DFG tiers (`WISP_WEBKIT_JIT=1 scripts/build-webkit.sh` into `build/webkit-jit`, packed by
`pack-webkit.sh` the same way; app/CMakeLists.txt links `wisp_web` and `wisp_web_gpu` from it, the UI and
network processes keep `webkit-wasm` without the JIT, so only the web image grows). The regular
expression and CSS selector JITs come with it. No FTL (B3 and Air: more image, and compile work a Jaguar
pays for), so no WebAssembly BBQ either (it depends on the FTL in 2.54): IPInt keeps running
WebAssembly. WebKit patch 0027: the JIT's pool is 64 MiB on the PS4 (x86-64's default is 1 GiB of
address space), because OSAllocatorPS4 maps an executable reservation whole through the kit's mmap (the
kit's probe picks the kind of memory this console runs generated code from: system flexible memory on
.90 (13.52) and .43 (14.00), outside the pool the app's processes share); polling VM traps (WebKit's
C++ sees FreeBSD, whose machine context gives a DFG build signal-based traps that would take SIGSEGV and
SIGBUS from the kit's crash log); 1 MiB stacks for the compiler threads; and WebAssembly's AddressType
conversions to B3 types built only with B3 (Sony's PlayStation port, the other JIT build without B3,
already left them out). The web process (child_main.cpp `jit_environment`) asks the kit's probe first:
no kind of memory runs code, or `jit=0` (menu: JavaScript JIT), and it starts on the LLInt as before
(`JSC_useJIT=false`); otherwise `JSC_jitMemoryReservationSize` from `jit_memory_mib` (64; 24 when the
only kind is the shared pool's) and `JSC_usePollingTraps=true`. web.log's "JavaScriptCore: ..." line
and the About page (`/data/Wisp/run/javascript.txt`) say which ran. As GPU rendering has, the JIT has
a fallback: two web process crashes within a minute with it on (on CPU rendering, or after GPU
rendering's own fallback) set `jit=0` for the next web process, with a toast saying so. Images
(loaded): web 100.2 -> 107.5 MiB, GPU web 126.5 -> 133.8 MiB with the JIT; UI 111.1 -> 111.4 MiB
(UnRAR); network 89.5 MiB as before.

**Encrypted RAR (v00.27).** User: "encrypted RAR/Zip files can't be extracted". On the PC, Files' own
code (archives.cpp + fileops.cpp) against Wisp's host libarchive (the PS4's options, OpenSSL): ZIPs with
ZipCrypto, AES-256 and AES-256 + LZMA asked for the password and came out identical to their source;
every encrypted RAR failed, data or list, RAR 4 or 5 ("Reading encrypted data is not currently
supported", "Prefix found", "Encryption is not supported", "RAR encryption support unavailable"), and so
did 7-Zip's AES (still so). RAR now goes to RARLAB's UnRAR 7.3.2 (its DLL API, built unmodified as a
static library: `scripts/build-unrar.sh`, the source downloaded with the user's go-ahead; license in the
package as `wisp/licenses/unrar.txt`). Files tests each entry (`RAR_TEST`) and writes the data UnRAR's
callback hands it where it puts the file, so names, "name (2)", USB rules, Cancel and the password
questions are those of the other formats; RAR 5's file copies and hard links come out as copies,
symbolic links stay out; a missing part of a set says which; dictionaries over 512 MiB are refused (the
UI heap is 768 MiB). UnRAR keeps its error state in one global, so one RAR is read at a time. RAR 4
has no password check: a wrong password shows as wrong checksums (of the first header in an encrypted
list, of the file otherwise), and Files reads those as a wrong password when one was given. ui.log now
says why a list or a job stopped ("files: ...: its list: ...", "files: taking files out of ... stopped:
..."). PC test (`tests/archives`: make-archives.sh on Windows with 7-Zip and WinRAR, then run.sh in WSL
with `rar`): 10 kinds of encrypted archives, a solid one with file copies, old
`.r00` parts, a missing part, a chosen subfolder, a file opened from inside, and Cancel in a 64 MiB file.

**RAR progress; files no page shows (v00.28).** User, on v00.27: an extraction "stuck at 1.54/6.34
GB". It was not stuck. Both files of a 6.8 GB RAR 5 with an encrypted list (5.6 GB and 1.0 GB) came out
whole: UnRAR checks each file's checksum, and the job ended with its "done" toast. Only the progress
stopped. v00.27 counted the archive's packed bytes, a file's share as packed x written / size in 64
bits. Inside a 5.6 GB file that product passes 2^63 after 1.536 GiB written; the value went negative,
and the card kept the last good one. The progress now counts unpacked bytes, with no multiplication:
- the total is the wanted files' sizes, read first from the list (each header has the whole size, also
  for a file whose data runs on into later parts);
- done is the files finished plus the bytes written of the one coming out.

The PC test now samples the job card while each RAR job runs. The progress must only move forward,
never pass the total, and end at it, and the total must be the source's bytes. It passes for every RAR
kind, a set of parts with a file across them, a solid archive with copies, a chosen subfolder, and the
64 MiB file.

Then a `.pkg` opened from Files went to a tab, and the network process crashed (gstvalue.c, fault
address 0x8). For a `file://` load, NetworkDataTaskSoup asks `MIMETypeRegistry::canShowMIMEType`. For a
type that is no image or document, that asks the media engines for their types, and their registry
scanner ran on a GStreamer that never started (gst_init is a stub in the UI and network processes:
`common/no_media.cpp`). Fixes:
- WebKit patch 0028: where GStreamer did not start, both scanners (regular and MSE) give no media types.
- Files no longer opens a binary file no page shows: none of the page and picture names, and a NUL byte
  in its first 4 KiB. It says "OrbisWeb can't open .pkg files".
- A `file://` response no page shows (an address typed) is ignored with a toast, instead of becoming a
  download of itself into Downloads.

Also: ui.log's "webkit:" line said "JavaScript by the LLInt interpreter" in v00.27 while web.log said
the JIT ran; it now says what the build and the menu give.

**Download folder (v00.29).** User: "make the place for download changable". Downloads always went to
`/data/Wisp/Downloads`. Now the Downloads page's second line says where they go, and its Change the
folder button opens a chooser over the page, built like the player's Open file:
- It starts at the folder downloads go to now. Its list holds the places (Downloads, /data, the USB
  drives) and a folder's folders, with Up a folder. Its foot has Select this folder (the folder shown),
  New folder and Cancel.
- Controller: Cross opens, Circle goes up (it closes at the places), Square selects, Triangle makes a
  folder, OPTIONS cancels. A mouse clicks; a USB keyboard has arrows, Enter, Backspace and Escape.
- A folder must be in /data or on a USB drive, outside OrbisWeb's own folders (Downloads and its
  subfolders excepted), and writable: a file is made in it and deleted.
- The choice is the setting `download_folder`, read through `wisp_download_folder()`
  (`common/wisp_settings.cpp`). aria2, WebKit's downloader, Files' Downloads place and the player
  (Save image's Screenshots, Open file's places) all follow it.
- Each aria2 download gets its folder as its own `dir` option, which aria2 keeps in its session file:
  an unfinished download goes on where it started after the folder changes, and so does a retry of a
  failed one while its folder is there. The global `dir` stays `/data/Wisp/Downloads`, for the session
  entries v00.28 and before left.
- While the folder chosen can't be had (its USB drive out), downloads go to `/data/Wisp/Downloads`. The
  page's line says so, and so does the "Downloading" toast. Nothing is made under `/mnt` for a drive
  that is out; a chosen folder that was deleted is made again when its drive is there.

**WebGL, the controller as a gamepad; no WebGPU (v00.30).** User: "add support to WebGPU and WebGL2,
and I want the controller to be recognized similar to PC when going to websites that can play games
with controller".
- **WebGL 1 and 2.** The web process's WebKit build (`webkit-jit`) is configured with `ENABLE_WEBGL`:
  ANGLE (`Source/ThirdParty/ANGLE`, its GL backend) on the GPU web process's own EGL, Mesa's Zink,
  through WebKit's textures path (`GraphicsContextGLTextureMapperANGLE`: no GBM or DMA-BUF here). WebGL 2
  is no separate switch: a context gets ES 3 whenever Zink gives it. Patch 0029:
  - ANGLE `dlopen`s `libEGL.so.1`, and the PS4 has no shared libraries. It now asks
    `wisp_angle_egl_lookup()`, which the GPU web process defines as Mesa's `eglGetProcAddress` (as
    libepoxy's `wisp_gl_lookup`). ANGLE's weak default finds nothing: the CPU web process has no WebGL,
    and it turns WebGL off itself anyway (patch 0016).
  - ANGLE's `SimpleMutex` used Linux futexes (`<linux/futex.h>`): `std::mutex` on the PS4.
  - Patch 0031: ANGLE's two `extern thread_local` variables (`gl::gCurrentValidContext`,
    `egl::gCurrentThread`) left weak undefined `_ZTH` (TLS init) symbols, which create-fself takes for
    system module imports ("missing module for symbol"). Declared `constinit`, clang reads them directly.
  - The UI and network processes keep their build without WebGL (no ANGLE in their images). What the
    processes send each other must then not depend on it. `WebPageCreationParameters`'
    `shouldRenderWebGLInGPUProcess` is sent in both builds. GPUConnectionToWebProcess's two WebGL
    messages (GPU process only) are left out of both builds: their numbers in `MessageName` would have
    shifted every later message by two.
  - Menu: WebGL (settings: `webgl`); off in CPU rendering. About shows which.
  - Zink already runs out of memory at times ("couldn't allocate memory: heap=4"): heavy WebGL pages may
    not get the GPU memory they ask for.
- **The controller as a PC gamepad.** WebKit's WPE platform asks the display for a gamepad manager
  (`create_gamepad_manager`) when a page starts reading gamepads, and lets it go when none does.
  `ui/platform.cpp` gives it the controller as one `WPEGamepad` named as Chrome on Windows names a
  DualShock 4: `Wireless Controller (STANDARD GAMEPAD Vendor: 054c Product: 09cc)` (settings:
  `gamepad_name`), with the standard mapping:
  - buttons 0-3 Cross, Circle, Square, Triangle; 4-5 L1, R1; 6-7 L2, R2 (analog); 8 the touch pad's
    button (the PS4 keeps Share); 9 OPTIONS; 10-11 L3, R3; 12-15 the D-pad. No 16: the PS button is the
    system's.
  - axes 0-1 the left stick, 2-3 the right one (4% dead zone for the resting jitter).
  - L2 and R2 as values from 0 to 1: WPE's button events are pressed or not, so patch 0030 adds
    `wpe_gamepad_button_value_event()` and PlatformGamepadWPE takes the value (WebCore: pressed over 0.1).
  - Vibration (dual-rumble) through `scePadSetVibration`, stopped after its time.
- **Game mode.** While a page has the controller, the controller drives nothing of the browser's: no
  pointer (a mouse still moves it), no Circle for Back, no OPTIONS menu, no L1 / R1 tabs
  (`shell::input_pad_to_ui(false)`); the browser sends the page nothing pressed while it has the
  controller itself. A page that starts reading gamepads gets the controller at once, with a toast.
  Holding the touch pad's button 1 s gives it back; the site is then remembered (settings:
  `gamepad_declined`) and asks again only when the button is held again. Another tab, a new page in
  the tab or no page reading gamepads any more also gives it back.
- **WebGPU: not done.** WebKit's WebGPU is Apple's Metal implementation (`Source/WebGPU`, left out of
  the WPE tarball); WPE has the bindings and their C API (`WebGPU.h`) and nothing under them. WebGPU here
  would mean porting a native WebGPU (Dawn, on Vulkan: the GPU web process's RADV) to the PS4, glue from
  WebKit's C API to it, and presenting its frames to WebKit's compositor, all in the one web process
  (the PS4 gives one process the GPU: no GPU process). A project of its own: `docs/WEBGPU.md` (pages have
  it from v00.40, below).

**One GPU device for the process, and where GPU memory goes (v00.47).** The end of v00.44's long Star Fox 64
session (`logs/v45-137/web.prev.log`, 33 MB): ~170 s into the game the direct memory ran out in 2 MiB allocations
in every heap (Zink's slabs: buffers and images up to 1 MiB are carved out of 2 MiB ones); the page slowed to
0.0-0.1 frames a second; then the 16 GiB GPU address window ran out too (123,000 failed `va_range_alloc`s); and the
web process crashed 40 minutes later in `st_upload_constants`, writing a draw's constants through NULL (the
uploader had no memory). The web heap stayed flat (~240 MiB), and nothing in the log said what held the GPU memory.
- **A GPU device per Vulkan instance, never torn down.** `ac_drm_device_initialize` made a new PS4 GPU device for
  every instance (WebKit's and ANGLE's Zink screens, Dawn's WebGPU devices: v00.45 made nine in one session, one or
  two per WebGL or WebGPU page), each with new GPU address windows (24 GiB below the GPU's 1 TiB limit: room for
  about 30), and `ac_drm_device_deinitialize` does nothing: a gone instance's device kept its quarantine of freed
  memory (up to 96 MiB) for good. Now the process has one device and RADV shares its winsys among the instances,
  as one render node's on Linux (web.log: "radv/ps4: another Vulkan instance shares the device").
- **Address ranges of failed allocations** went into the deferred queue behind the quarantine, which only moves
  on freed buffers' bytes; with no memory nothing was freed, and every failed attempt kept its 2 MiB range. They go
  back at once now (a range nothing was mapped into).
- **Slab entries behind a busy one.** Mesa reclaims freed slab entries oldest first and stops at the second that is
  still busy; one that stays busy (its batch never flushed: an idle context) holds back every entry freed after it,
  and their slabs. With more than 1024 entries waiting, all of them are walked, at most every 250 ms (`pb_slab.c`,
  PS4 only).
- **Lines that say where it goes**: radv/ps4's line every 1000 submissions adds "GPU memory N MiB in M buffers, Q MiB
  of it freed and in quarantine"; each Zink screen says every 10 s (`gpu_log`) "N MiB in M Vulkan allocations; S
  slabs (X MiB) ..., W of their entries freed and waiting for the GPU (P found past busy ones); C MiB of freed
  allocations cached". A long Star Fox run shows which of them grows.
- `st_upload_constants` with no upload buffer keeps the constants bound before (a wrong draw, not a crash).
- **On the console** (`logs/v47-137`, Mortal Kombat: Shaolin Monks, ~3 minutes; the user: "still sucks"): one GPU
  device for the run ("shares the device" once), GPU memory flat at ~330 MiB (94 MiB of it the quarantine), the JIT
  at 136 of 256 MiB with no compile failing; the WebGPU canvas 15-43 frames a second by scene. The game's thread
  ("WebCore: Worker") ran in 317 of 317 looks, nearly every sample in OMG code: the PS2 emulator's WebAssembly at
  its best tier, on one Jaguar core. That is the browser's ceiling for it; PS2 games belong in DawnStation
  (Shaolin Monks is one of its fixed games) and N64 games in native ports (Star Fox 64 and Bomberman 64 have
  N64Recomp ports, as Conker's on the PS4).

**A larger JIT region, OMG for large functions, a profiler that names JIT code (v00.46).** User, after v00.45:
"check". Logs and profiles: `logs/v45-137`.
- **Mortal Kombat** ran 19-30 frames a second until the JIT's code region failed it: from ~160 s 165 BBQ and 42 OMG
  compiles were "out of code memory" with 96 of 128 MiB in use (the region fragments: large functions find no
  room before it is full), and the functions they were for stayed in IPInt: ~1 frame a second. The region is
  256 MiB now (`jit_memory_mib`; when the system flexible memory has no 256 MiB, 128: "jit: 256 MiB of system
  flexible memory for compiled code is not there"), and OMG takes functions up to 256 KB (`wasm_omg_max`; 29-30 of
  its functions were too large for 100 KB, the largest 156 KB, Star Fox's 221 KB).
- **Star Fox 64**: the compositor's frames took 1.5-12 ms at ~60 a second (v00.44: ~20 ms at 40; patch 0053), and
  the page's frame grew from 7 to 60 ms over 5 minutes of play, with the main thread's share in JIT code going from
  42% to 67%: which functions, and at which tier, the profile couldn't say.
- **The profiler** (kit): JIT samples are named by the port (`ps4_jit_owner`, WebKit patch 0055: the WebAssembly
  function holding the address, from JSC's callee registry): "jit 0xTTFFFFFF", TT the tier (1 BBQ, 2 OMG, 3 OMG
  for OSR entry), the rest the function's index; each thread's report adds where it waited ("waiting in (N looks),
  from:" with the program addresses); a thread that ran in a window is looked at again every 2 rounds instead of
  every 50.

**Frames read back as BGRA and only where they changed, WebAssembly's tiers, compilers that give way (v00.45).**
User, after v00.44: Mortal Kombat "still slow", Star Fox 64 "still slow", "lets try to fix these games before going
back to the rest", "also utilize all the six PS4 usable cores". Logs and profiles: `logs/v44-137`.
- **What the frame lines said.** Mortal Kombat: the page drew 58-59 times a second at 0.5-3 ms each, the compositor
  ~21.5 ms a frame, the WebGPU canvas 4-17.5 frames a second; the profile had one "WebCore: Worker" running in every
  look and every other thread idle: the game is one thread of WebAssembly at 100% of a core (its PS2 runtime),
  most samples in JIT code, ~15-20% in IPInt (`ipint_i32_load_mem`, `ipint_br_if`, ...) and its tier-up checks
  minutes into a fight, its compiled code spread over 48 of the JIT's 64 MiB. Star Fox 64: the page drew 17-21
  times a second at 28-36 ms each (EmulatorJS runs the N64 core in the page's animation frame), and the compositor
  37-41 frames a second at ~20 ms each (two composites for each game frame), so it was busy ~80% of the time and
  the page waited on it.
- **The compositor's 20 ms**: after every composite `RenderTargetSHMImage::didRenderFrame` read the whole surface
  (1920x972, 7.5 MB) back with Skia's `readPixels`; the renderbuffer is RGBA8 and the shared buffer BGRA, so Skia
  converted every pixel on the CPU (its few profile samples: `SkRasterPipeline`, `GrConvertPixels`' buffers, malloc,
  memset). Patch 0053: GL reads back BGRA (Mesa converts on the GPU; the surface's origin is top-left) and only the
  part of this buffer that changed since it was last read back (every frame's change, as the platform is told it,
  goes into every target's record; a new target reads back everything). A GL error leaves it to Skia for good
  ("AcceleratedSurface (PS4): GL could not read the frame back as BGRA" in web.log); `WISP_SHM_READBACK=0` keeps
  Skia's way.
- **WebAssembly's tiers**: the JIT's code region is 128 MiB (`jit_memory_mib`; system flexible memory, as before),
  OMG takes functions up to JavaScriptCore's own default, 100 KB (`wasm_omg_max`; v00.42-v00.44 had 50 KB), and
  web.log has "wasm JIT:" lines when they move (patch 0054: functions BBQ and OMG compiled, failed, out of
  executable memory, those too large for OMG and the largest, the JIT's code in use).
- **All six cores**: the PS4 runs an app's threads by priority alone, with no turns among threads of one priority,
  so WebAssembly's six compiler threads and the JavaScript JIT's, at the default priority, could hold every core
  during a compile burst while the game's own thread waited. They run a step below the default now (720;
  `compiler_priority`; kit `ps4_kit_set_thread_priority_by_name`, applied when a thread names itself, which WebKit
  threads do since patch 0050): they use every core the game and the page leave free. A game whose own work is one
  thread (Mortal Kombat) is limited by that thread; the browser's part can only stay out of its way.
- **The profiler** looked at a busy thread ~3 times a second: each look waits 5 ms for a thread that never answers
  (Mesa's block signals), a dozen of them made a round ~0.3 s instead of 20 ms. A thread that misses twice is now
  looked at as a waiting one (kit).

**Closed games' memory, WebGPU canvases without a new texture a frame, what holds a game back (v00.44).** User,
after v00.43: Hot Pursuit 2 "black screen only"; Gold Miner "plays well now"; Midnight Club 3 "stuck while
reading files, page did not stop, the music can be heard, but then I got no picture error" (screenshot); Bomberman
64 "failed to load"; Star Fox 64 "loaded, it seems very very slow"; Mortal Kombat "runs in 6~10 fps, please fix
this issue, check what holds us back, utilize both CPU and GPU and optimize the work". Logs and the screenshot:
`logs/v43-137`.
- **The holder of a closed game's memory: its AudioWorklet.** Hot Pursuit 2's black screen: after a reload the
  new page's 1 GiB came on top of the old page's (WebKit's pages: 2051 MiB), and its 160 MiB vertex buffer and
  then its canvas texture got no direct memory (`VK_ERROR_OUT_OF_DEVICE_MEMORY`). The old page's game threads did
  end this time (patch 0046), but the crash dump 30 s after the tab closed listed two **AudioWorklet** threads
  alive, idle in their run loops (`AudioWorkletThread.cpp:77`), one per HP2 page. Upstream stops a worklet's
  thread only when its `BaseAudioContext` is destroyed, and a stopped context lives on while a node is still
  connected to its destination (each node holds the context, the connections hold the nodes). Apple's systems end
  a tab's process with the tab, so this never shows there; here the worklet's script world, holding the game's
  shared memory (its sound ring), stayed for good. Patch 0049: `BaseAudioContext::stop()` (the page torn down)
  stops the worklet's thread, which clears its script (`clearScript`: its VM and every object in it).
- **Midnight Club 3's 2 GiB stayed with no worker or worklet thread left** (the crash dump at 2958 s had none), so
  something else holds it; that run ended in Bomberman 64's 512 MiB being refused, and Star Fox's WebGL context
  failing to map a buffer (below). Patch 0052 says what still holds memory, 5 s and 30 s after a page leaves
  (web.log "memory (5 s after a page left): ..."): WebKit's pages in direct memory, the main thread's global
  objects (and those kept from C++), every buffer of 64 MiB or more its script heap still reaches with the page
  it belongs to (on show or gone), documents, worker and worklet threads, audio contexts. Memory held while the
  script heap reaches none of it is held from C++.
- **Midnight Club 3's size**: on WebKit it takes a fixed 2 GB shared memory and 40 game threads, and its web heap
  reached 1.26 GB of downloaded disc data: the PS4's direct memory ran out (the network process could not get an
  overflow heap: "largest free block 8 MiB", then a malloc crash; the web process's malloc crashed at 764 s). Its
  own low-memory profile (1 GB, 8 threads, lighter defaults; the one it runs on phones and TVs, where it "stayed
  black" otherwise) is chosen when `navigator.deviceMemory` says 2 or less, which WebKit doesn't have: page script
  `DEVICE_MEMORY` gives it (setting `device_memory`, 2 GB; about what the PS4 leaves a page).
- **Mortal Kombat's canvas**: `getCurrentTexture()` made a new Dawn texture every frame (a GPU allocation, its
  mapping and Dawn's lazy clear each frame, freed once the GPU was done); ~90 s in, direct memory was gone, the
  canvas went black, and the web process crashed later in Zink. Patch 0051 keeps one texture for a canvas (until
  its size or configuration changes or the page destroys it), clears it after each readback copy in the same
  submission, and drops one whose creation ran short of memory (an error scope) so the next frame tries again.
- **Zink's map without a staging buffer**: out of memory, `zink_buffer_map`'s wait-free write path went on with a
  NULL staging resource and read its `so_valid` (fault 0xf2): the Star Fox crash at 2958 s (a WebGL context's
  first subdata) and Mortal Kombat's at 3359 s (Skia's compositing upload). The map fails now (Mesa worktree,
  uncommitted).
- **The collector fallback misfired**: two "not answering" restarts in the run (Hot Pursuit 2's memory, the Zink
  fault) turned the concurrent collector off, so Star Fox 64 and Mortal Kombat ran with stop-the-world
  collections on the page's thread. Both fallbacks (two crashes within a minute, two hang restarts) are off by
  default now (setting `gc_fallback`): the start-time check already proves the collector can stop the threads, and
  no hang since v00.35 came from it.
- **What holds a game back**: the kit's hot-spot profiler runs in the web process and the UI (setting `profiler`,
  on for this test): every 10 s each busy thread's busiest code in `kit-web-gpu/profile.log` and
  `kit-ui/profile.log` (the last run's as `profile.prev.log`, kit change). WebKit's threads now have their names
  in the kit's dumps and profile (patch 0050: WTF passes them to `ps4_watch_thread`). Every 10 s web.log has a
  "frames:" line beside the stalls: how often the page drew (`WebPage::updateRendering`: its animation frames,
  style, layout, painting) and for how long, the compositor's frames and their time, and WebGPU canvases'
  frames presented, not read back (the GPU two behind), shown, textures made (`wisp_frame_counters`).
- Gold Miner plays with the keyboard (v00.43's `RUFFLE_FOCUS`); it first failed in the process Hot Pursuit 2 had
  emptied (no direct memory for Ruffle's 16 MiB), and played after the UI restarted the web process.

**Old games' memory freed, WebGPU's typed-array setBindGroup, WAV decoding, Ruffle's keys (v00.43).** User,
after v00.42: Crash Bash "worked, at the beginning there was no audio for some reason but later it worked"
(and a showcase video from its clip); Gold Miner "still does not accept keyboard input"; Hot Pursuit 2 "same
issue of Out of Memory"; Star Fox 64 "failed twice to load"; Zelda "loads perfectly"; Midnight Club 3 "Out of
Memory, heap problem?"; Bomberman 64 "failed, I think we have pattern here with 64 games"; Mortal Kombat
(its page) "page stopped working". Logs and the 10-minute clip: `logs/v42-137`.
- **One cause for Hot Pursuit 2, both N64 games and Midnight Club 3.** Every refusal was "WebAssembly (PS4):
  no direct memory for a memory of N MiB" (HP2's reload 1024, Star Fox and Bomberman 512, MC3 2048 down to 512).
  The 1906 s thread dump had Hot Pursuit 2's game threads (15 dumped, the same thread ids as at 859 s) still in
  `Atomics.wait`, 17 minutes after their tab had closed, and WebKit's page pool held 1026 MiB in use at 2074 s:
  the first HP2 page's memory never went. Patch 0043's 100 ms slices checked `vm.hasTerminationRequest()`, but a
  termination request (`VM::notifyNeedTermination`) only raises the NeedTermination trap bit; the flag is set
  when the thread handles its traps, which a thread parked in `Atomics.wait` never does. Patch 0046: the wait
  also ends on the trap bit (and sets the flag, as `VMTraps::handleTraps` does). The "64 games" pattern: N64
  emulators (EmulatorJS) ask for 512 MiB; the GBA one (Zelda) for much less.
- **The network process** mapped a 512 MiB overflow heap (direct memory) at 810 s, as Hot Pursuit 2 loaded
  (one request its 128 MiB heap could not take: its heap was ~10 MiB in use before and after), and kept it for
  the rest of the run. Its overflow heaps are 128 MiB at most now (`overflow_direct_max`), and the kit's line
  says which request overflowed (operation, size, caller: `for malloc(N) from elf 0x...`; kit change,
  uncommitted).
- **Mortal Kombat** (both tries): web process abort in `WebGPU::ComputePassEncoderImpl::setBindGroup` (the form
  with a Uint32Array of dynamic offsets): upstream reaches the Implementation layer only through its GPU process,
  which turns the array into a Vector, so the span overloads were `RELEASE_ASSERT_NOT_REACHED`. Patch 0047
  implements them for the compute pass, render pass and render bundle encoders.
- **Crash Bash's silent start**: its Web Audio output (not a suspended context: the destination rendered full
  rate) was silence for ~100 s from Adventure mode on, sound from then to the end. The game (Babylon.js 9, its own
  PS1-style sequencer) decodes each music bank's samples and its sound effects with `decodeAudioData` before they
  play, all WAV (`audio/sfx_NNNN.wav`, `music/bank_NNNN/...`). WebKit ran each file through a GStreamer pipeline
  on a thread of its own, one file after another on the context's one decoder thread. Patch 0048 reads WAV
  itself (integer 8/16/24/32-bit, float 32/64, extensible; resampled with
  `AudioBus::createBySampleRateConverting`), decodes on three more threads, and logs the first decodes and then
  every 32nd in web.log ("Web Audio (PS4): decodeAudioData #N ..."; `audio_log`). PC test of the reader:
  `tests/webaudio-wav/run.sh` in WSL (ASan/UBSan, 14 cases).
- **Gold Miner's keys**: the keys reached WebKit (USB keyboard plugged in this time; the browser's shortcuts take
  no arrow or Space key). Ruffle 0.6 (`web/src/lib.rs`) handles a keydown only while its player "has focus",
  set by a focusin on the player element; it listens once its WebAssembly instance is ready. The player's shadow
  tree (`delegatesFocus: true`) holds a hidden, focusable `<input id="virtual-keyboard">`, and the game page calls
  `player.focus()` 400 ms after making the player: on the PS4 Ruffle isn't ready then, the focus went into the
  player unheard, and each click after it ran Ruffle's `focus()` on an already-focused player, which per the
  HTML focusing steps fires nothing. Page script `RUFFLE_FOCUS` (setting `ruffle_focus`): on a player's first
  press, if the focus is already in it, it is taken out, and Ruffle's own capture-phase press handler focuses it
  again, now with a focusin. Not confirmed on the console yet: page script `KEY_LOG` (setting `key_log`, on for
  this test; needs `console_log`) logs each key and focus move as the page sees them ("OrbisWeb keys: ...").
- Hot Pursuit 2's first run logged "ZZDATA.BIN at N: Load failed; again in 0.25 s": its own range loader
  (`net.js`) retries for ~31 s; you reloaded within ~6 s.
- **Showcase**: a 4-minute 1080p video of the session (not in the repository: game footage), with the
  account dialogs (name, password, recovery code) and the silent stretch left out.

**The internal pool's leak found, texture uploads, the optimizing JITs, a reload's memory, the controller as a
keyboard (v00.42).** User, after v00.41: Midnight Club 3 "has audio running now, but the problem is loading the
game"; Gold Miner "clicks works now, but the issue is only keyboard input"; Star Fox "loading is slow, it is not
even playable"; Hot Pursuit 2 "same issue, out of memory"; Zelda "audio + controller working now"; Crash Bash
(a web game) "slow when loading, and the game ... very slow, please optimize it, and optimize the overall
browser". Logs and a 14-minute clip: `logs/v41-137`.
- **The leak** (v00.41's census): every steady loss of the internal pool was `pthread_rwlock_init`, ~62 bytes
  that never came back, from Zink's `resource_object_create` (its `copy_lock`): one per texture or buffer Zink
  makes. Star Fox's N64 renderer made them all the time (`GL_BindTexture`), WebKit's compositing and Skia's GPU
  drawing too. Mesa's `u_rwlock` is now a reader-writer lock on the kernel's futex on the PS4 (no kernel
  object). The census is off by default (setting `kobj_census`): its two pool queries per call showed in the
  watchdog's stacks.
- **Texture uploads**: the watchdog's dumps of Crash Bash's 13-27 s stalls (Babylon.js on WebGL 2) had the
  page's thread in WebGL `texImage2D` -> ANGLE -> `st_TexSubImage` -> `zink_image_map`, which made a new
  staging resource (a Vulkan buffer, its memory, its mapping) for every upload. A write-only map now takes its
  staging space from Zink's stream uploader, as a buffer map does.
- **The JITs**: WebAssembly ran on BBQ and JavaScript on DFG at most. OMG and the FTL are on by default now,
  their largest candidates left to the tiers below (`JSC_maximumOMGCandidateCost` 50000,
  `JSC_maximumFTLCandidateBytecodeCost` 20000); settings `wasm_jit`, `js_ftl`.
- **Hot Pursuit 2**: patch 0042 collected four times and the old 1 GiB stayed. The dump taken meanwhile had the
  old page's game threads alive, parked in `Atomics.wait` (`WaiterListManager::waitSyncImpl`) inside their
  workers' run loops: a terminating VM wakes its waiter without the waiter list's lock, so a waiter about to park
  missed it and slept for good, its shared memory held. Patch 0043: a synchronous wait parks 100 ms at a time and
  checks for termination. Patch 0044: one collection, then 250 ms waits for about 2 s (four full collections of a
  1.3 GiB heap had held the page 26 s, until the UI restarted the web process).
- **Midnight Club 3** loads further (its sound plays) and then said "No picture after 140 s"; its disc cache had
  stopped at QuotaExceededError: WPE gives each site a fixed 1000 MB. Patch 0045: 16 GiB a site.
- **Gold Miner** wants arrow keys, and the USB keyboard had been unplugged: the controller drove only the pointer
  (its D-pad goes back, forward and scrolls). L1 + L2 + R1 + R2 (or the touch pad) held on a page that reads no
  gamepads now makes the controller a keyboard: D-pad or left stick arrows, Cross Space, Circle Enter, Square Z,
  Triangle X, L2 Shift, R2 Ctrl, OPTIONS Esc (`keys_frame`); the same hold gives the pointer back.
- Not done: Crash Bash's asset pack fetch said "access control checks" (6 times) and Ruffle's blob URL load too;
  Midnight Club 3 may still draw nothing on WebGPU (it never logged a Dawn error).

**Clicks by pointer events, the controller for pages, page files, big downloads, a reload's memory; the
kernel-object census (v00.41).** User, after v00.40: Star Fox 64 "page stopped again ... loading was really slow
... no audio"; Zelda "controller is not functional even after pressing"; Super Mario Sunshine "backend operation
issue"; Hot Pursuit 2 "out of memory"; a Flash game (Gold Miner) "unable to interact ... even with mouse";
Midnight Club 3 "had error". Logs and two clips: `logs/v40-137`.
- **Gold Miner** (a Flash game page, Ruffle 0.6 on WebGL): the game ran and followed the pointer (its buttons lit on
  hover), but took no click.
  - The UI's mouse button events carried the button mask the GDK way: a press without its button, a release with
    it. WPE's own seats do the opposite (`WPEWaylandSeat.cpp`), and WebKit takes MouseEvent.buttons from it.
  - WebKit's chorded-button rule (`PointerCaptureController`) then made each release a pointermove, and the pointer
    stayed pressed: every later press was a pointermove too. DOM click events don't depend on it (links and buttons
    worked); pages that click by pointer events (Ruffle, canvas games) got nothing.
  - `browser.cpp` `page_pointer`: a press has its button, a release not.
- **Zelda** (EmulatorJS, GBA): its Control Settings listed no gamepad while the UI said "The page has the
  controller".
  - The UI sent the controller's events only while WebKit said it monitored input (`start_input_monitor`).
  - WebKit stops monitoring when a view goes inactive with no other active (a tab switch hides the old tab first),
    and starts again only when a view becomes active while a page already reads gamepads: a page that starts
    reading later gets nothing.
  - `platform.cpp` now sends them whenever a page reads gamepads (WebKit's gamepad listens all the time). ui.log
    says when WebKit starts and stops reading the controller, and whether it finds a page for its input when a
    page takes the controller.
- **Super Mario Sunshine** (truffled.lol): the full disc 404s; the page then streams the smaller one into the
  origin-private file system from a worker, through a synchronous access handle.
  - "Backend does not support this operation": `IPC::SharedFileHandle` had nothing off Apple's systems, so
    `createSyncAccessHandle()` always failed on WPE.
  - It also kept the file's lock: every later attempt said NoModificationAllowedError ("The object can not be
    modified").
  - WebKit patch 0040: the file goes to the web process as a descriptor (`UnixFileDescriptor`, as shared memory's
    does); the lock is released on failure.
- **Midnight Club 3** (a page with an Xbox static recompilation on WebGPU with 40 pool workers): the network
  process ended at `malloc(52634847)` in the disk cache's `store`.
  - A 52 MB response was buffered whole for the cache (up to an eighth of its capacity), then copied whole, in a
    128 MiB heap that could not grow: the kit said "no direct memory to spare for another overflow heap", the game
    held it.
  - The page's disc list then failed ("WebKit encountered an internal error"), and its storage too ("Connection is
    lost").
  - Patch 0041: on the PS4 a response is buffered for the disk cache up to 8 MiB; larger ones aren't cached.
- **Hot Pursuit 2**: WebGPU worked (60 pipelines in 2.8 s; the game booted with sound).
  - Two reads of its 3 GB data file failed ("Load failed"), and the page was reloaded.
  - The new page's 1 GiB WebAssembly memory was refused while the old page's 1 GiB was still held: "Out of memory".
  - Patch 0042: a refused WebAssembly memory gets a full synchronous collection and sweep, then another try, up to
    4 times over about a second.
- **Star Fox 64** (EmulatorJS, N64): the page stopped as on v00.37 ("mutex lock failed: Out of memory").
  - The internal pool emptied in a minute, ~220 KiB/s from when the game's sound started. Zelda's GBA core lost
    ~15 KiB/s, and every web process ~9 KiB/s, the rate of v00.37's 27-minute YouTube stop.
  - v00.38's GStreamer clock and v00.39's GLib locks are in (`gstsystemclock` waits on `_umtx_op`; the self-test
    gives GLib's locks 0 bytes), so something else makes kernel objects in a loop.
  - The kernel-object census (`child/kobj_census.cpp`, setting `kobj_census`):
    - wraps the web processes' pthread, semaphore, event queue and direct memory calls;
    - charges each call's change of the pool to its callers (return addresses found on the stack);
    - says in web.log which took it ("kobj:" lines, elf addresses).

**WebGPU for pages (v00.40).** User: "Yes, start WebGPU", after Hot Pursuit 2's page said the browser
had no WebGPU. Details and what's left: `docs/WEBGPU.md`.
- WebCore's own WebGPU (bindings, the abstract `InternalAPI` and its `Implementation` layer) now runs on
  Dawn (WebKit patch 0039). The Implementation layer was written for WebKit's fork of the C API; it was
  ported to Dawn's standard `webgpu.h`. `HAVE_WEBGPU_IMPLEMENTATION` is on for the web process's tree only
  (`scripts/build-webkit.sh`: `WISP_WEBKIT_WEBGPU`, on with the JIT tree; one full rebuild).
- Dawn runs in the GPU web process on the kit's RADV, beside Zink, as the self-test showed it could (v00.34).
  Its callbacks come on the page's thread: an event pump posts `wgpuInstanceProcessEvents` there while
  something waits.
- Canvases: there is no IOSurface or shared texture between Dawn and the compositor yet. A presented frame
  is copied into a mappable buffer, read back a frame or two later, and shown as an image on the canvas's
  compositing layer (at most two frames in flight; later ones are dropped while the GPU is behind).
- The CPU web process links the same WebKit without Dawn: do-nothing `wgpu*` functions
  (`app/src/child/webgpu_stubs.c`), and `wisp_webgpu_usable()` (WebKit's weak definition) says no, so no
  page there gets a GPU.
- Settings: WebGPU (Pages and JavaScript; on, with CPU + GPU or GPU drawing), WebGPU errors in the log
  (Diagnostics). About's WebGPU row says whether pages have it.

**GLib's locks on the kernel's futex (v00.39).** User: "also read why youtube page stopped" (a v00.37 run:
a YouTube playlist for 27 minutes, then "The page stopped working").
- web.log's pool lines fell steadily during playback, 13.7 MiB to 18 KiB at ~8.6 KiB a second, then GLib
  aborted: "Unexpected error from C library during 'pthread_cond_init': Out of memory" (SIGABRT, caught by
  the kit: the page stopped, the app stayed). The stack: GStreamer's multiqueue sending an event, GObject's
  `weak_ref_data_lock`, GLib's bit lock (`bit_lock_futex_wait`), `g_futex_wait`. GLib's PS4 build had no
  futexes (it looks for `<linux/futex.h>`), so its bit locks emulate one: every wait on a contended bit lock
  makes and destroys a GCond, and every GMutex and GCond is a pthread object (made on first use, destroyed
  by `g_*_clear`). Made and destroyed in a loop, those used up the pool, as GStreamer's clock entries did.
- `scripts/patch-glib-futex.py` (run by build-deps.sh): `gthreadprivate.h` defines HAVE_FUTEX and
  `g_futex_simple` for `__PS4__` on libkernel's `_umtx_op` (a timed wait reports ETIMEDOUT by the clock, the
  kernel library's errno numbers being FreeBSD's), and `g_cond_wait_until` gets a PS4 branch. GLib then uses
  its native GMutex and GCond (plain integers, Linux's code) and real futex bit locks in all three
  processes; GRecMutex and GRWLock stay pthread objects. Reinstalling GLib's headers changed no content: the
  15 regenerated ones got their old times back (`touch -r` from a copy), so WebKit didn't rebuild.

**Web Audio's clock, closing the app, page storage, credentialless isolation (v00.38).** User, after v00.37:
"Page stopped working for star fox 64, audio also does not seem to work for it", "the app also crashed upon
closing", Hot Pursuit 2 "does not detect WEBGPU", Zelda: The Minish Cap (a GBA emulator page)
"did not load", Super Mario Sunshine (truffled.lol) "gave me cross origin isolated" (screenshots).
- **Star Fox**: v00.37's futex check passed and Mesa no longer took the pool, but web.log's new lines showed it
  draining ~200 KiB a second from the moment the game's sound started (13.4 MiB at the start, out at 127 s),
  and only ~0.5 MiB a minute during YouTube's sound. WebKit's Web Audio pipeline is `webkitwebaudiosrc` (128
  frames a buffer, 375 a second) `! clocksync ! audioconvert ! audioresample ! queue ! sink`, and clocksync
  waits on a new clock entry for every buffer; GStreamer's POSIX entries make a condition variable (with an
  attribute object, on the monotonic clock) and a mutex for each and destroy them after: ~550 bytes of the
  pool were never given back each time. The crash: `pthread_cond_init` failed in `ensure_entry_initialized`,
  g_error, SIGTRAP, which the kit now caught (page "stopped working", the app stayed). The sound itself
  reached the PS4 (-16 to -29 dBFS for 80 s), but only 40 s after the page loaded. Fix: GStreamer's
  `gstsystemclock.c` gets PS4 entries (`scripts/patch-gst-clock.py`, run by build-media.sh): a lock word and
  a condition word on libkernel's `_umtx_op`, no kernel object. The web process now also measures, as it
  starts, what making and destroying each kind of kernel object leaves taken (web.log "memory: the PS4's
  internal memory each kernel object kept after it was destroyed"; memory_log).
- **The crash on closing**: the PS4 suspends an app before it closes it, and waits for every process that
  used the GPU to call `sceGnmSubmitDone` after its last submission. The klog: "Suspend pid=282 phase3 is
  postponed", "Can't suspend pid:282. submitDone() is not called", exception 0xa0d0c00a
  `CPU_FAULT_SUBMITDONE_TIMEOUT_IN_SUSPEND_ASYNC` (an AppCrash with a core dump). The kit's RADV layer
  (`ac_ps4_drm.c`) called it after a flip or at most every 4 ms, so a burst's last submission could go
  without one, and an idle page submits nothing more. Now the submission thread makes the owed call 2 ms
  after the last submission when it runs out of work (every submission makes it when there is no thread).
- **Pages' storage** (Hot Pursuit 2: "undefined is not an object (evaluating
  'navigator.storage.getDirectory')"; Mario Sunshine: "Origin-private file system unavailable"): WebKit has
  `StorageAPI`, `StorageAPIEstimate`, `FileSystem` (getDirectory), `AccessHandle` (the synchronous file
  handles Emscripten uses in workers), `FileSystemWritableStream` and `FileSystemHandleSerialization` on for
  Apple's systems only. The UI turns them on (setting `opfs`); the files stay in the network process's
  website data.
- **Cross-origin isolation** (Mario Sunshine sends `Cross-Origin-Opener-Policy: same-origin` and
  `Cross-Origin-Embedder-Policy: credentialless`): WebKit 2.54 decides `crossOriginIsolated` per page from
  them (our single web process doesn't matter), but parses only `require-corp`. WebKit patch 0037 takes
  `credentialless` as require-corp (setting `coep_credentialless`, env WISP_COEP_CREDENTIALLESS in all three
  processes): cross-origin no-cors files then need a CORP header.
- **Zelda's "Network Error"**: EmulatorJS's message for a ROM or BIOS download that failed or got an HTTP
  error; the site sends a Cloudflare check to anything but a browser, so the PC couldn't see its files. WebKit
  patch 0038 logs a page's failed subresources (HTTP 400+, failed loads) with their address in web.log
  (setting `load_log`), so the next try names the file.
- **WebGPU** (Hot Pursuit 2, "WebGPU draws it"): not in pages yet. Only the Dawn self-test exists (v00.31,
  passed on the Pro); pages need `docs/WEBGPU.md` steps 2-4.

**Star Fox 64's crash: the PS4's pool for thread objects (v00.37).** User, after v00.36: "star fox caused it to
crash".
- **What the logs said**: Star Fox played with sound for about 30 s, then web.log had eight `[ScePthread/System]
  Internal Memory is running out.` lines and `[GStreamer ERROR] pthread_mutex_init returned 12`; 12 s later the
  UI saw the network and web processes end and the app was gone. No crash.log anywhere. The PS4's kernel log
  had the rest (GoldHEN's klog, port 3232: its buffer still held the whole run when read afterwards): the web
  process took SIGTRAP ("breakpoint instruction fault", thread `pool-8`), the PS4 made a core dump (AppCrash)
  and its crash reporter closed the app (KillApp: network, web, UI). The backtrace (absolute addresses: minus
  0x400000 and 1, against `oelf/wisp_web_gpu-v00.36.elf`): WebKit's Web Audio pipeline, GStreamer's `clocksync`
  waiting on the clock, `gstsystemclock.c` `init_entry`: `pthread_mutex_init` failed (ENOMEM), `g_error`, and
  GLib ends a process with `G_BREAKPOINT()` (int3).
- **Why the pool ran out**: the PS4's kernel library makes every pthread mutex, condition variable, attribute
  object and thread from a small internal pool in each process (`sceKernelInternalMemoryGetAvailableSize`).
  The kit's Mesa turned futexes off on the PS4 in October only because OpenOrbis has no `<sys/umtx.h>` ("skip
  FreeBSD-only APIs"), so every `simple_mtx` was a pthread mutex and every `util_queue_fence` a mutex and a
  condition variable: Zink's programs, its pipeline cache entries (one per pipeline variant), shaders, buffer
  objects, resource objects, the threaded context's render-pass records, GL buffers' ranges. Several are never
  destroyed in that configuration (upstream builds it with futexes only; the kit already fixed one, the tc
  fence lock, "leaked every frame"). A WebGL N64 emulator makes thousands. GStreamer's clock, which makes a
  mutex and a condition variable for every buffer it waits on, only met the empty pool first. (v00.34's and
  v00.35's "Internal Memory" closes also came during Star Fox: the large allocations fixed then were real, but
  this pool is probably what ran out each time.)
- **Fixes**: Mesa (Wisp's worktree, uncommitted, `patches/mesa/`): `util/futex.h` says the PS4 has futexes and
  `util/futex.c` waits and wakes through libkernel's `_umtx_op` (FreeBSD's `UMTX_OP_WAIT_UINT_PRIVATE` 15 and
  `UMTX_OP_WAKE_PRIVATE` 16; Mesa's absolute monotonic deadline is turned relative, which FreeBSD 9 and later
  both read): a lock is a 32-bit word, no kernel object. The GPU web process checks it before Mesa starts
  (web.log `futex (Mesa's locks, on the kernel's umtx): works ...`). Kit: the crash handler takes SIGTRAP too
  (GLib's `g_error`, WebKit's `CRASH_WITH_INFO`): such a crash ends only its own process, with a crash.log, and
  the page "stopped working" instead of the PS4 closing the app. Every process logs its internal pool (`memory:
  N KiB of the PS4's internal memory left in this process`: at the start, on 512 KiB moves, every 2 s under
  2 MiB with ": LOW"; setting `memory_log`).
- When the app vanishes with no crash.log, read the klog's buffer at once: the PS4's own crash report (signal,
  thread name, backtrace) is in it.

**Arabic's dots, games' memory in place, page fullscreen, the address list (v00.36).** User, after v00.35:
"Audio Works", "Arabic still not looking well, check the screen shot", "Fix FullScreen of videos, it does not
hide the browser elements", "add drop down or list menu of addresses used before", "Find why the browser died
after successfully loading star fox 64" (and a YouTube Short made from the console's clips: `promo/`).
- **Arabic**: the screenshot (YouTube in Arabic) had every dot off: those above a letter a line too high,
  those below inside the letter ("سنة" as "سه"). Noto's Arabic fonts draw dots as separate glyphs that GPOS
  moves onto their letters (Noto Naskh: dot above at -428/1000 em onto a medial noon). Patch 0022 turned
  HarfBuzz's y offsets over as it stored them, but `ComplexTextController::advance()` turns each paint
  advance over itself ("Increasing y points down"): turned twice. Checked on the PC: HarfBuzz's shaping of the
  shipped fonts through a model of `advance()`, `FontCascade::layoutComplexText` and FontSkia's positions,
  both directions (scratchpad `hbtest/sim.py`). WebKit patch 0035: offsets stored as they come; the glyph
  buffer's initial advance turned (left-to-right runs whose first glyph moves).
- **The app closing again** (v00.35, no crash log, network process first): the same
  `[ScePthread/System] Internal Memory is running out` as v00.34. The emulator's 512 MiB memory was direct
  memory now, and its grow to 758 MiB was refused cleanly ("no direct memory for a buffer of 758 MiB"); then a
  YouTube video, the GPU driver's "did not fit", and the kernel's pool emptied: WebKit's page commits fell back
  to the system flexible pool once direct memory was gone. WebKit patch 0036: no flexible memory for WebKit's
  pages while direct memory works (a commit fails and the web process ends, not the app); non-shared
  WebAssembly memories reserve their maximum (2 GiB when none) and grow in place (`wtf_ps4_large_commit_next`,
  a handle count per memory in `BufferMemoryHandle.cpp`: 512 -> 758 MiB needs 246 more, not 1270 at once);
  the system heap's large allocations come cleared. The UI's heap 768 -> 512 MiB and the network process's
  256 -> 128 MiB: every heap takes its direct memory whole at the start (+384 MiB for pages and the GPU).
- **Video fullscreen**: WebKit says `enter-fullscreen` before it asks the view's toplevel, which only noted
  the state: the toolbar stayed and the page kept 1920x972. Now the toolbar goes (`page_fullscreen`, as F11)
  and comes back on `leave-fullscreen`; Circle, Esc, F11, the menu, another tab, the tab closing or its web
  process ending take the page out (`platform_leave_fullscreen` unfullscreens the toplevel).
- **The address list**: under the address field, `pages::suggestions` (bookmarks first, then the history,
  newest first, one entry an address; what is typed matched in the address or the title, an address that
  starts with it first). A USB keyboard's typing filters it, Up/Down choose a row, Enter goes there; with the
  controller, Cross on the field opens it (its first row opens the PS4's keyboard; Triangle still opens the
  keyboard at once, and with nothing to list Cross does too); a click on a row goes there.
- The UI's audio port opens with the system's user (255) first: the console user's opens failed every time
  ("sceMbusAddHandleByUserId 0x20000007") and 255's played.

**Sound from the UI, games' memory, the WebGPU test's crash (v00.35).** User, after v00.34: "pages
sometime stops working even settings can stop working", "audio test have no audio", "webgpu test is
conducted" (it passed), "still Star Fox 64 not loading in the browser", "and app then crashed".
- **Settings stopped working** right after the WebGPU test: Dawn's device (the test's second VkDevice beside
  Zink's) was destroyed at the test's end, and the kit's Vulkan layer (`ps4-kit/gpu/ps4_vk_wsi.cpp`) kept
  ONE device's submit pointers, cleared when "the" device went: Zink's next `vkQueueSubmit` called address
  0 on its flush thread (kit crash.log: rip 0, `[rsp]` = the layer's `QueueSubmit`). The layer now keeps
  every live device and its submits stay while any device lives (kit change, uncommitted like the rest).
- **The app closed itself** two minutes into Star Fox 64 (all three processes at once, no crash log):
  web.log filled with `[ScePthread/System] Internal Memory is running out.` and `pthread_cond_init returned
  12`. The emulator's 512 MiB WebAssembly memory (not shared: bmalloc's `SystemHeap::memalignLarge`)
  had come from the kit's `mmap`, which takes large anonymous mappings from the system flexible memory
  pool, where the kernel keeps its own objects (the kit's own notes say the same of a 512 MiB heap). WebKit
  patch 0034: those allocations are direct memory from WTF's pool (`wtf_ps4_large_allocate`), and only
  direct memory, so a game asking too much gets a RangeError instead of the PS4 ending the app; growable
  buffers' commits (`wtf_ps4_commit`) are direct memory only too. A grow of such a memory (512 -> 759 MiB)
  copies into a new one: both at once (1.27 GiB of the ~2 GiB of direct memory the heaps leave).
- **Star Fox 64 didn't start** because EmulatorJS 4.2.3 builds its settings menu at game start with
  `MediaRecorder.isTypeSupported()`, unchecked; this WebKit has no MediaRecorder ("Failed to start game
  ReferenceError: Can't find variable: MediaRecorder"). A stand-in (`media_stubs`): no type supported,
  the constructor refuses (only the emulator's "Start Screen Recording" needs it).
- **No sound** (also the v00.34 sound test: 276 of its 285 blocks had sound in them, the port took them
  all, nothing was heard). Every page's sound since v00.26 went to a port the web process opened, a local
  process of the app; nothing shows it was ever heard from one. Pages' sound now plays from the UI process
  (`ui/audio_out.cpp`): the web process's mixer writes its mixed blocks into a ring in shared memory
  (`common/wisp_audio.h`, `SHM_ANON`, sent as START's second descriptor) and keeps at most 6 blocks (32 ms)
  ahead of the UI's audio thread, which plays them on a port opened with the console's user (the UI's user
  service has one; the web process's had none, so it used the system's), mixed with any other web
  process's and the Settings page's own tone. The sink's delay counts the ring. If the UI takes nothing for
  200 ms the web process keeps the port's time without it. `audio_out=web` brings back the old way.
  Settings > Sound test has two tones: the page's (Web Audio) and OrbisWeb's own (no page in it), and
  which one is heard says where the sound stops; ui.log and web.log both have `audio:` lines.

**WebAssembly's JIT, sound, YouTube's ads, the controller back (v00.34).** User, after v00.33: "the games
both of them did not load, and youtube did not have audio, and so the rest of websites", "find a method to
return to using controller after being utilized by the games ... like pressing all R + L buttons", "make
sure all video elements in youtube are present ... i can't skip videos, the button is not appearing".
- **The kart game** got past v00.33's fixes (threads, shared memory) and then ran its start on IPInt, the
  interpreter: the hang dumps (this build's, symbolized) had the main thread in IPInt's frames 12 s and
  more, until a reload restarted the web process. WebAssembly's JIT is now in the web process's build
  (`scripts/build-webkit.sh`: `WASM_JIT_SWITCH`, `ENABLE_WEBASSEMBLY_BBQJIT`/`OMGJIT` and the FTL they are
  built on: BBQ includes B3's headers, OMG is B3). At run time BBQ (`wasm_jit`: bbq; omg adds OMG, whose
  B3 compiles take hundreds of MiB and seconds of a Jaguar core for a game's large functions; off); the FTL
  stays off for JavaScript (`js_ftl`). Cost: the GPU web process's image is 153 MB (text 148.7 MB), 20 MB
  more out of the pool the app's processes share (v00.33 left 79-93 MiB of it while browsing). The "not
  answering" toast now says to wait.
- **Star Fox 64** (an N64 emulator page on WebGL) took the GPU web process down in Zink:
  `zink_resource_image_barrier` read `res->obj` NULL (fault 0x3c0), from `zink_update_barriers`, past the
  freed-resource guard there (the resource was live, its object gone). The guard now also skips a
  resource without an object, and the image and memory barriers return for one (Wisp's uncommitted Mesa
  changes, `patches/mesa/wisp-egl-uncommitted.patch`; web.log says so through `zink (Wisp): ...`).
- **No sound** (YouTube in the GPU web process): the port opened ("audio: the PS4's audio out open")
  when the video started and never closed, so blocks kept going to it; v00.32's CPU web process had played
  real sound (5131 blocks, 9 silent). Not found from the logs. The sink now says more: each player's start
  with its pipeline's name, the port's state when it opens (`sceAudioOutGetPortState`: where it plays, its
  volume), and every 10 s while it plays the blocks it took against its rate and the loudness of the
  output and of each player (`audio_log`). Its volume is set to 0 dB on both channels instead of assumed.
  Settings has a Sound test (a second of 440 Hz through Web Audio).
- **YouTube's Skip button**: EasyList blocks the requests YouTube's player makes while an ad plays (its
  stats and pings), not the ad's video. A script on YouTube (with ad blocking on; `youtube_ad_skip`) takes
  an ad's video to its end while the player says an ad is showing (and its overlay is there, or the video
  is under three minutes), and presses a Skip button whenever one shows.
- **The controller back**: L1 + L2 + R1 + R2 held half a second switches it between the page and the
  browser, like the touch pad's button held a second; the page doesn't see the four shoulders while the
  hold that switched lasts. The toasts and About name both.

**WebAssembly SIMD and threads, smoother pages (v00.33).** User: "the game does not load for some
reason find why, also add webassembly SIMD", "the browser is not optimized yet for the ps4, sometimes it
has hinges and dips while browsing, please optimize it", "i also now just started game but nothing
started", "please I want a browser that acts like chrome or any android browser that can do all of this".
- **SIMD was off after all.** v00.32's Ruffle (on a Flash game page) still logged "Some WebAssembly extensions
  are NOT available". Of the five it tests (bulk memory, SIMD, saturating conversions, sign extension,
  reference types) JavaScriptCore can lack only SIMD, which it drops on an x86-64 CPU whose CPUID check
  finds no AVX (`Options.cpp`). WebKit patch 0033 takes AVX as present on the PS4 (the whole program is
  built for the Jaguar, AVX and all) and logs once what CPUID said (web.log: "JavaScriptCore (PS4): CPUID
  1 ECX ...").
- **The kart game** (olivers.tools/kart: an Emscripten threads build with COOP + COEP headers;
  `crossOriginIsolated` picks its threads engine; 6 workers, a 384 MiB shared memory with a 4 GiB
  maximum, WebGL 2) stopped at "WebAssembly.Module doesn't parse at byte 6404: shared memory is not
  enabled".
  - JavaScriptCore allows shared memories only with its fault signal handler, which stays off here (the
    kit's crash log owns SIGSEGV and SIGBUS). Patch 0033 allows them on the PS4 while SharedArrayBuffer is
    on, bounds-checked. Such a memory is checked against its maximum, so an access between its size and
    its maximum faults (a crash, not a WebAssembly trap): only a module reading past the end of its own
    memory does that.
  - A growable memory (a shared memory, a resizable or growable ArrayBuffer) was its whole maximum in
    memory from the start (bmalloc's large allocation; the PS4 backs every mapping at once): 4 GiB for the
    kart game. Now it is a reservation (`OSAllocator::tryReserveUncommitted`) whose pages get direct
    memory as it grows (`wtf_ps4_commit`, OSAllocatorPS4.cpp; none left is an out-of-memory error) and
    give it back as a resizable buffer shrinks.
  - SharedArrayBuffer in every page (`JSC_useSharedArrayBuffer`; settings `wasm_threads`): Emscripten's
    threads code names `SharedArrayBuffer` (`buffer instanceof SharedArrayBuffer`), and WebKit gives it to
    a process, not a page. Chrome gives it only to cross-origin isolated pages because each of those gets
    a process of its own; one process serves every page here, so the headers would guard nothing.
    `crossOriginIsolated` itself stays per page (the main frame's COOP + COEP), as WebKit has it.
  - It needs WebGL 2 too: GPU drawing back (below).
- **GPU drawing back.** The console had `rendering=cpu` since v00.31's two crashes (the bmalloc ones
  v00.32 fixed): CPU drawing paints, composites and scrolls every frame on the CPU, and has no WebGL. The
  automatic fallbacks now last one run (`fallback_<key>=1` marks one; `restore_fallbacks` at start), as a
  desktop browser tries its GPU again at each start, and a console with an older version's
  `rendering=cpu` is taken back once (`fallbacks_v33`), with a toast.
- **Hitches.**
  - CPU drawing (above) was the largest part.
  - JavaScriptCore's concurrent collector (`concurrent_gc`, automatic): the plain one stops the page's
    thread for each whole collection (400-500 MiB heaps on game pages). The concurrent one stops a thread
    with a signal (WTF: the thread's handler posts a semaphore and waits in sigsuspend for the same
    signal; the semaphores and signals are the PS4 kernel library's, its struct layouts FreeBSD's). The
    web process tries those steps on a thread of its own at start, with deadlines and nothing that can
    hang, and turns the collector on when they work (web.log "gc:"). The fallback ladder now takes it away
    first, for the run: when the web process ends twice within a minute, or stops answering twice.
  - The stall log (`stall_log`): a 50 ms tick on the web process's main loop; every 10 s with stalls, a
    line with how many were 0.3 s or longer, the longest and the time in all (web.log "stalls:"): the
    next round's numbers.
- **Restored downloads.** aria2 brings back the last run's unfinished downloads; those failing again were
  toasted at every start without a name ("Download failed:  (failed)", five times on .137). Now named
  from aria2's address and logged, not toasted.
- **Not yet; each needs WebKit work.** Service workers (WebKit runs a site's in a process of its own:
  coi-serviceworker sites and PWAs need them); WebAssembly's BBQ JIT (WebKit's CMake ties it to the FTL:
  games run on the IPInt interpreter); WebGPU for pages (`docs/WEBGPU.md`).

**Settings, History, Bookmarks, Cookies; the game's "GPU error" (v00.32).** User: "add also History
page, Bookmarks, Cookies, put all settings in one page, and add WebAssembly SIMD, also when the game
tried to load I got some gpu error".
- **The "GPU error"** (v00.31 on .137, a Midnight Club 3 emulator page) was no GPU fault.
  - The page's `new WebAssembly.Memory` crashed the web process twice (`0xbbadbeef` in
    `bmalloc::api::tryLargeZeroedMemalignVirtual`, from `JSC::Wasm::Memory::tryCreate`). bmalloc zeroes a
    large allocation by mapping anonymous memory `MAP_FIXED` over it (`vmZeroAndPurge`); the memory was
    the kit's direct memory and the PS4 refused that mapping, so the release assertion after it fired.
    WebKit patch 0032: zeroed in place with `memset` on the PS4.
  - The two crashes within a minute then made the browser switch to CPU drawing ("Drawing with the GPU
    stopped the page twice"), and with it WebGL off. Now a crash whose kit crash log says fault address
    `0xbbadbeef` (WebKit's own stop) doesn't count against GPU drawing.
  - ANGLE's `EGL_BAD_MATCH ... share list notification strategy mismatch` in web.log is its first try (a
    robust context Mesa won't share with WebKit's); it falls back to a normal one: WebGL works.
- **WebAssembly SIMD** seemed already on (v00.33: it was not; JavaScriptCore's AVX check left it off):
  WebKit's `WasmSIMD` feature is on by default, JavaScriptCore keeps it on an x86-64 CPU with AVX, and its in-place interpreter runs SIMD
  (its handlers are offlineasm vector instructions, x86-64 too). Now explicit: Settings' WebAssembly
  SIMD and relaxed SIMD switches (`wasm_simd`, `wasm_relaxed_simd`), and a live check on the Settings page
  (`WebAssembly.validate` of SIMD and relaxed-SIMD modules, SharedArrayBuffer), what a game's check sees.
- **One Settings page** (`orbisweb://settings`, OPTIONS > Settings; `ui/pages.cpp`):
  - every setting the app reads, in groups (General, Privacy, Pages and JavaScript, Drawing and memory,
    Controller, Downloads, Player, Diagnostics), each with when it applies: at once, to pages loaded from
    then on, or after a restart (a Restart button then);
  - the download folder (it opens the Downloads tab's chooser), the controller's declined sites, the
    WebGPU test, Clear browsing data.
  - The OPTIONS menu kept only actions: New tab, Close tab, Reload, Full screen, Add bookmark (Remove
    bookmark when the page is one), Bookmarks, History, Downloads, Files, Extensions, Settings, About,
    Restart, Leave.
- **History** (`orbisweb://history`): every page a tab commits (http and https), its title when it comes,
  in `/data/Wisp/history.tsv` (5000 entries, saved 2 s after a change); by day, searchable, an entry
  removed, all cleared.
- **Bookmarks** (`orbisweb://bookmarks`, `/data/Wisp/bookmarks.tsv`): OPTIONS > Add bookmark, or an
  address typed on the page; removed one by one.
- **Cookies and site data** (`orbisweb://cookies`): WebKit's website data manager and cookie manager. It
  lists each site, what it keeps (cookies with their names, storage, databases, cache, service worker),
  and has a Remove for each and for all (asked first); also the cookie policy (`cookies`: allow all, block
  third-party (WebKit's default), block all), applied at start and at once.
- The pages act through their own links and forms with a token made at each start: a web page linking
  to `orbisweb://cookies?do=clear` gets the page shown, nothing done. After an action the page puts its
  address back without it (`history.replaceState`), so a reload doesn't act again.

**WebGPU, first step (v00.31).** Dawn builds for the PS4 (`scripts/build-dawn.sh`, `patches/dawn/`) and
is linked into the GPU web process (4.9 MiB more image so far). Menu: WebGPU test (it restarts
OrbisWeb). At that start the first GPU web process runs Dawn beside Zink: an adapter, a device, a
compute shader and a rendered triangle, checked. The result goes to a toast, About and
`/data/Wisp/run/webgpu-test.txt`. Pages get no WebGPU yet (`docs/WEBGPU.md`).

## Extensions

Formats: Chrome `.crx` (CRX2/CRX3 header + zip), Opera `.nex` (CRX), Firefox `.xpi` (zip), plain
`.zip` or a folder. Installed from Files, or from the stores' pages: the Chrome Web Store
(`clients2.google.com/service/update2/crx?...&x=id%3D<id>%26uc`), Opera add-ons and Mozilla AMO get
an "Add to Wisp" button.

Built on WPE's public API:

| Extension part | WPE mechanism |
|---|---|
| `chrome-extension://<id>/` pages | `webkit_web_context_register_uri_scheme` (files from the unpacked extension) |
| background page / MV3 service worker | a hidden `WebKitWebView` running it (event page) |
| content scripts, `scripting`, `tabs.executeScript` | `WebKitUserScript` in a per-extension `WebKitScriptWorld`, `webkit_web_view_evaluate_javascript` |
| `chrome.*` / `browser.*` APIs | a JS shim in extension pages and content scripts, calls go to the UI process by script message handlers with replies |
| `declarativeNetRequest`, built-in ad blocking | rules compiled to WebKit content rule lists (`WebKitUserContentFilterStore`) |
| `webRequest` (observing; blocking later) | the web-process extension's `send-request` signal |
| popups, options pages | small `WebKitWebView` panels in the UI |

First targets: uBlock Origin Lite (MV3, declarativeNetRequest + scripting), AdGuard MV3, Dark
Reader, SponsorBlock, Bitwarden-style popups. Built-in ad blocking from EasyList / EasyPrivacy works
without any extension.

## Dependencies (static, kit toolchain, `~/wisp-build/sysroot`)

zlib, libpng, OpenSSL (kit sysroot); brotli, expat, libffi, pcre2, GLib, libpsl, nghttp2, SQLite,
libsoup 3, glib-networking (OpenSSL backend, static module), libxml2, libtasn1, libgpg-error,
libgcrypt, FreeType (+brotli: WOFF2), HarfBuzz (+ICU), ICU, Fontconfig, libjpeg-turbo, libwebp,
libxkbcommon (keysyms only), libepoxy (no dlopen), Mesa (EGL + GLES + Zink + RADV). Since v00.17
(own prefixes): GStreamer 1.26.11 + FFmpeg 8.0.3 + libogg, libvorbis, libtheora, opus
(`~/wisp-build/media`), aria2 1.37.0 (`~/wisp-build/aria2`), libarchive 3.8.11 + bzip2, xz, zstd, lz4
(`~/wisp-build/archive`). Off: ATK, XSLT, spellcheck, speech, WebDriver, AVIF, JPEG XL, LCMS,
journald, sysprof, libbacktrace, DRM/GBM, Wayland, libwpe (legacy API), WebRTC, WebCodecs, MediaStream.

PSChrome's hardware-verified OpenOrbis pitfalls apply (`~/.claude/projects/J--PSCHROME/memory`):
musl's Linux-only calls (eventfd, pipe2, accept4, getrandom, getifaddrs, the resolver, ...) must be
off in every configure; load system modules before calling them; create-fself segment rules.

## Console layout

`/data/Wisp/`: `profile/` (cookies, storage, cache), `Downloads/` (where downloads go unless another
folder is chosen: `download_folder`), `extensions/`, `logs/`
(ui.log, net.log, web.log and the three runs before: .prev.log, .prev2.log, .prev3.log), `kit-ui/`,
`kit-net/`, `kit-web/` (the kit's crash logs and boot traces), `run/state` (the UI's first lines say
when the last run did not end through the menu: closed from the PS4's menu, crashed or stopped),
`settings.txt`. The menu's Restart Wisp ends the web and network processes and has the system start
`eboot.bin` again (as WinePS4 restarts): the GPU setting takes effect without leaving Wisp.

`settings.txt` keys (`key=value`, one per line; most are also in the menu):

| Key | Default | |
|---|---|---|
| `rendering` | hybrid | `hybrid`: the CPU draws the page in tiles, the GPU (Zink on RADV, `wisp-web-gpu.bin`) composites, scrolls and animates; `gpu`: Skia draws on the GPU too; `cpu`: Skia on the CPU, no compositing (`wisp-web.bin`, no Mesa). (`gpu` 0/1 before v00.13 no longer decides.) |
| `paint_strips` | 1 | CPU rendering: the strips each frame is played back in (1: on the main thread alone; 4 was v00.10-12's default) |
| `watchdog` | 1 | a main loop that has not run for 8 s, or a page that has not answered for 8 s: every thread's stack in `logs/<process>.hang.log` |
| `restart_hung_page` | 1 | Reload, a typed address or Home on a page that has not answered for 8 s ends the web process and starts it again (0: they wait like any other request) |
| `ipc_log` | 1 | WebKit's IPC lines in the logs: its sockets' room, messages that wait for room on a busy socket, a message lost; and ui.log's socket test |
| `console_log` | 0 | the pages' console messages (script errors) in web.log (menu: Page messages in the log); off by default since v00.16: each message is turned into text on the page's thread, and Cloudflare's Turnstile takes that for an open console |
| `adblock` | 0 | built-in ad blocking (EasyList + EasyPrivacy) |
| `concurrent_gc` | auto | JavaScriptCore's concurrent collector (it suspends the JavaScript thread by signal): `auto` on when the web process's check of that at start passes (v00.33), `1` on unchecked, `0` off |
| `gc_fallback` | 0 | two crashes within a minute, or two restarts of a page that stopped answering, turn the concurrent collector off for the rest of the run (v00.33's rung); off by default since v00.44 (it misfired on memory and GPU ends) |
| `memory_log` | 1 | every 30 s per process in ui.log: heap in use and the flexible memory left; the flexible memory left on each tab's opened/closed line; each process's `memory:` lines (what it holds of the shared pool) |
| `inspector` | 0 | the remote Web Inspector on ports 9222/9223; WebKit's developer extras only with it (v00.16) |
| `user-agent` | Safari 18 on macOS | the user agent string |
| `pointer_size` | 160 | the mouse pointer's size in percent of a desktop's (80-400) |
| `paint_threads` | 4 | CPU + GPU: the painting threads that draw the tiles |
| `smooth_scrolling` | 0 on the CPU, 1 on the GPU | WebKit's animated scrolling |
| `vsync_pacing` | 1 | the screen composed after each vblank (0: the old 16 ms timer); without a vblank for 50 ms a frame comes anyway |
| `partial_frames` | 1 | only the changed parts of page frames copied and composed (0: whole frames, as before v00.10) |
| `gpu_log` | 1 | GPU rendering: EGL's own start-up lines in web.log, and the Zink guards' notes (v00.24: a freed buffer caught, with the code address that freed it) |
| `tab_memory_floor` | 40 | MiB of the shared pool a new tab needs left; with less, a toast instead of a tab (0: no check) |
| `media_stubs` | 1 | stand-ins for the media element classes a WebKit without media lacks, so page scripts that name them go on (never in Cloudflare's, reCAPTCHA's or hCaptcha's frames); nothing with media (v00.17 on) |
| `gst_log` | 2 | GStreamer's own log level in `logs/gstreamer.log` (0: none, 2: warnings, 4: much more) |
| `canvas_gpu` | 0 | 2D canvases drawn on the GPU (WebKit's accelerated canvas); off since v00.19: v00.18's web process crashed in Zink (`zink_resource_memory_barrier`, the main thread, a canvas) |
| `service_workers` | 0 | service workers and shared workers (WebKit's `ServiceWorkersEnabled`, `SharedWorkerEnabled`); off since v00.21: a site's workers run in a web process of that site's own, which the PS4 cannot start beside the first (v00.20 ended at YouTube). On, other sites' workers ask for processes the PS4 refuses (their pages go on) or that empty the shared pool |
| `wasm` | 1 | WebAssembly in pages (menu: WebAssembly; a WebKit built with it, v00.21 on: JavaScriptCore's in-place interpreter, bounds-checked memories, no fault handler) |
| `jit` | 1 | JavaScriptCore's JIT in the web process (menu: JavaScript JIT; v00.27, the web process's WebKit build with the JIT and DFG tiers); 0, or a console where no executable memory runs code: the LLInt interpreter, as up to v00.26 |
| `jit_memory_mib` | 256 | MiB of executable memory for the JIT's code, mapped whole at the first page's scripts (24 when the only kind the console runs code from is the shared pool's; 128 when the system flexible memory has no room for it); 64/128/256/384 (v00.46; 128 in v00.45) |
| `stacks_in_heap` | 1 | thread stacks from each process's heap in direct memory, not the shared flexible pool (0: the pool, as before v00.17) |
| `webkit_pages_direct` | 1 | WebKit's page commits in direct memory (WebKit patch 0021; 0: the shared flexible pool, as before v00.17) |
| `cookies` | `no_third_party` | cookies sites may keep: `always`, `no_third_party`, `never` (Settings > Privacy, the Cookies page; v00.32) |
| `wasm_simd` | 1 | WebAssembly SIMD (WebKit's WasmSIMD feature; JavaScriptCore keeps it on a CPU with AVX); v00.32 |
| `wasm_relaxed_simd` | 1 | WebAssembly relaxed SIMD (WasmRelaxedSIMD); v00.32 |
| `wasm_jit` | omg | WebAssembly's JIT in the web process (v00.34, a build with it): `omg` BBQ then OMG (default since v00.42), `bbq` BBQ, `off` IPInt alone |
| `wasm_omg_max` | 256000 | the largest WebAssembly function (bytes) OMG optimizes; larger ones stay on BBQ; 50000/100000/256000/512000 (v00.46; 100000 in v00.45, 50000 in v00.42-v00.44) |
| `compiler_priority` | 720 | WebKit's compiler threads (WebAssembly's and the JavaScript JIT's) at this priority, below the default 700 so they use the cores pages leave free; 0: the default (v00.45) |
| `js_ftl` | 1 | JavaScriptCore's FTL tier for JavaScript (built in with the WebAssembly JIT; v00.34; on by default since v00.42) |
| `webgpu` | 1 | WebGPU for pages (v00.40; with CPU + GPU or GPU drawing) |
| `webgpu_log` | 1 | Dawn's uncaught errors and lost devices in web.log (v00.40) |
| `kobj_census` | 0 | The kernel-object census in web.log ("kobj:" lines; v00.41, off by default since v00.42) |
| `key_log` | 1 | page script logging the keys and focus moves pages see ("OrbisWeb keys: ..." page messages; needs `console_log`); v00.43, on for Gold Miner's test |
| `ruffle_focus` | 1 | page script: on a Ruffle player's first press, the focus taken out of it if a page's early `focus()` put it there unheard, so Ruffle's own handler focuses it with a focusin and takes keys (v00.43) |
| `device_memory` | 2 | page script: `navigator.deviceMemory` in GB (WebKit has none); games that read it pick their light mode at 2 or less (Midnight Club 3); 0 leaves it out (v00.44) |
| `youtube_ad_skip` | 1 | with ad blocking on, YouTube's ads taken to their end and Skip pressed (v00.34) |
| `audio_log` | 1 | the audio sink's lines in web.log: players' starts, the port's state, every 10 s its pace and loudness (v00.34); the UI's port lines in ui.log (v00.35) |
| `audio_out` | ui | where pages' sound plays: `ui` the UI process (the web process's mixer writes a ring the UI plays; v00.35), `web` a port the web process opens (v00.26-v00.34) |
| `wasm_threads` | 1 | WebAssembly threads: SharedArrayBuffer in every page and shared WebAssembly memories, bounds-checked (WebKit patch 0033); v00.33 |
| `stall_log` | 1 | the web process's main loop stalls (0.3 s or longer) in web.log, a line at most every 10 s (v00.33), and a "frames:" line: the page's rendering updates, the compositor's frames, WebGPU canvases' frames (v00.44) |
| `profiler` | 1 | the kit's hot-spot profiler in the web process and the UI: every 10 s each busy thread's busiest code in `kit-web-gpu/profile.log`, `kit-ui/profile.log` (v00.44, on for Mortal Kombat's test) |
| `fallback_rendering`, `fallback_jit`, `fallback_concurrent_gc` | (none) | 1: the key beside it was set by an automatic fallback; the next start takes it out (v00.33) |
| `fallbacks_v33` | (none) | 1 once v00.33 took back an older version's lasting `rendering=cpu` |
| `webgpu_selftest` | 0 | 1: the next start's GPU web process runs the WebGPU self-test (menu: WebGPU test sets it and restarts; cleared at that start) |
| `webgl` | 1 | WebGL 1 and 2 in pages (menu: WebGL; v00.30): ANGLE on Zink in the GPU web process; off in CPU rendering |
| `gamepad_name` | `Wireless Controller (STANDARD GAMEPAD Vendor: 054c Product: 09cc)` | the controller's name as pages read it (`Gamepad.id`, v00.30) |
| `gamepad_declined` | (none) | sites (host names, space-separated) the user took the controller back from: they get it only when the touch pad's button is held |
| `download_folder` | (none) | where downloads go (Downloads > Change the folder, v00.29): a folder in /data or on a USB drive; none, or the one chosen not there (its USB drive out): `/data/Wisp/Downloads` |
| `download_manager` | 1 | aria2 takes a page's downloads, links and magnet links (0: WebKit's downloader for everything) |
| `dm_connections` | 8 | the download manager's connections a file (1-16) |
| `dm_parallel` | 3 | files it downloads at once |
| `dm_seed_minutes` | 0 | minutes a finished torrent is seeded |
| `dm_log` | 1 | aria2's own log in `logs/aria2.log` (0: errors only) |
| `player_threads` | 4 | FFmpeg's decoding threads for a video |
| `player_next` | 1 | at a file's end, the folder's next one plays |
| `player_volume` | 100 | the player's volume (it keeps the last one set) |
| `memory_meter` | 1 | the memory meter at the tab strip's right end: RAM of the three processes, the shared pool's use and what is left |
| `memory_limit` | 1536 | MiB: WebKit's memory monitor in the web process lets caches go past 40% of it and also collects garbage past 60% (v00.25, WebKit patch 0025) |
| `memory_release` | 1 | a closed or sleeping tab's page: its caches released and a full garbage collection a moment after (v00.25) |
| `tab_sleep_after` | 600 | seconds a tab must be hidden before it sleeps (0: never for that) |
| `tab_sleep_floor` | 40 | MiB of the shared pool: under it the longest-hidden tab sleeps (0: never for that; 64 until v00.22) |
| `tab_sleep_heap` | 1200 | MiB of the web process's heap: over it the longest-hidden tab sleeps (0: never for that; 700 until v00.22) |
| `autoplay` | allow | media a page starts by itself: `allow` (with sound), `muted` (WPE's own default: YouTube played without sound), `deny` |
| `player_after` | 0 | at a file's end: 0 the folder's next file, 1 again, 2 stop there, 3 close the player (`player_next=0` was 3) |
| `player_sub_encoding` | 0 | how a subtitles file that is not UTF-8 is read (0 Automatic: Arabic Windows-1256 or Western Windows-1252; the menu's list) |
| `player_sub_size` / `player_sub_lift` / `player_sub_box` | 100 / 0 / 0 | the subtitles' size in percent, pixels higher, a dark box behind them |
| `player_boost` | 100 | the player's volume boost in percent (to 400, clipped) |

The flexible memory pool is shared by the app and its local processes: each process's program
image (about 100-120 MiB each now), stacks and shared memory come out of it. When it runs out the
PS4 does not fail the allocation: the app freezes, with nothing in any log.

What came out of it in v00.06 (.51): the UI and network processes left ~170 MiB, each web process
took ~125 MiB of that just to start, and the 35-50 MiB left ran out within seconds of a page: frame
buffers failed ("Failed to allocate shared memory buffer") and the web process aborted when its
garbage collector could not start a helper thread (`Thread::create`'s RELEASE_ASSERT). v00.07's diet:
- WebKit without JIT tiers (the LLInt interpreter; WebAssembly off): 9 MiB less in each image
- the web process without Mesa for CPU rendering (`wisp-web.bin`; `wisp-web-gpu.bin` has it): ~23 MiB
- thread stacks: 256 KiB at least (the kit's 2 MiB is for GPU shader compilers; `wisp-web-gpu.bin`
  keeps it), JavaScript threads 512 KiB as on Sony's port (WebKit patch)
- frames: two shared-memory buffers instead of four (each 7.5 MiB, mapped in the web process and the
  UI); the UI copies each frame into its own heap (direct memory) and gives the buffer back at once
- no big static buffers: Files' 4 MiB copy buffer and the kit sampler's 1 MiB are heap allocations
  while in use
A thread the system refuses to start is logged in the kit's boot trace (`kit-*/boot-trace.log`).

ICU's data archive (31 MiB) is read by each process into direct memory of its own and handed to ICU
(`common/wisp_icu.cpp`): ICU's own loader failed silently on the console and WebKit stopped at its
first text break iterator (v00.05). Read, not mapped: a file mapping comes out of the shared flexible
pool. Then ICU's data directory is pointed at one without the archive and ICU reads packages only:
otherwise, at the first item a lookup missed, ICU mapped the whole file again as its "extended data"
(30 MiB of the pool in v00.13's web process). ui.log, net.log and web.log say how long the read took,
whether a break iterator opens and what ICU's own loader met. About 22 MiB of the archive are display
names (zones, currencies, units, languages, regions): a filtered archive would save most of that.

What else maps memory out of the pool, and what Wisp does instead (v00.14): Skia's font files (read
into the heap: WebKit patch 0018), the network cache's shareable resources (sent inline), frame
buffers of hidden tabs (given back a second after a tab is hidden). The `memory:` lines name each
anonymous mapping `anon:<address>`: the address of the code that made it plus the image's base
(0x400000, `kit-*/boot-trace.log`'s first line); `llvm-symbolizer --obj=~/wisp-build/oelf/<binary>-vXX.YY.elf`
on the address minus 0x400000 says what it is.

Certificates are checked against the console's clock (glib-networking's OpenSSL backend, Mozilla's
roots in `/app0/wisp/cacert.pem`). A PS4 that lost its clock reads 1 January 1970 (the .43 console,
v00.08: its FTP server stamped new files 1970-01-01 00:03), and every site's certificate is then
"not valid yet" (`G_TLS_CERTIFICATE_NOT_ACTIVATED`, 0x4). Wisp replaces WebKit's error page
("Unacceptable TLS certificate") with its own: the reasons in words, the certificate's dates, the
console's clock, and when the clock is earlier than the day Wisp was built, how to set it (Settings >
System > Date and Time). Each log's first line has the console's clock.

## Phases

1. **Engine**: dependencies, Mesa EGL, WPE WebKit cross-built; probe pkg (GLib + TLS fetch, local
   processes with SEQPACKET IPC, Zink EGL readback).
2. **First page**: one tab on the console (UI + network + web process, SHM frames, pad pointer).
3. **Browser**: tabs, address bar, history, bookmarks, downloads, Files, settings, PS4 keyboard.
4. **Extensions**: install/manage, content scripts, storage, runtime, tabs, DNR, popups;
   built-in ad blocking.
5. **Media and speed**: GStreamer + FFmpeg (YouTube), JSC JIT, WebGL, zero-copy frames.
