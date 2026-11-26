# Building Wisp

Everything builds in WSL (Ubuntu) with the ps4-kit toolchain (clang 21 + libc++ 21 on the OpenOrbis C
runtime). `~/wisp` is a link to this folder; the build tree is `~/wisp-build`.

```bash
bash "/mnt/e/PS4 Projects/Wisp/scripts/setup-tree.sh"   # ~/wisp link, cross files, poison library
bash ~/wisp/scripts/build-deps.sh                        # 23 libraries: GLib, libsoup, ICU, ...
bash ~/wisp/scripts/build-mesa.sh                        # Mesa: EGL + GLES + Zink + RADV, static
bash ~/wisp/scripts/build-webkit.sh                      # WPE WebKit 2.54.2 (configure + build)
bash ~/wisp/scripts/pack-webkit.sh                       # its objects in one thin archive
WISP_WEBKIT_JIT=1 bash ~/wisp/scripts/build-webkit.sh    # the web process's copy, with the JIT (v00.27)
WISP_WEBKIT_JIT=1 bash ~/wisp/scripts/pack-webkit.sh
WISP_VERSION=01.00 bash ~/wisp/app/build.sh              # the processes + out/OrbisWeb-v01.00.pkg
```

Then from Windows (Git Bash, not WSL), to the PS4's FTP server (GoldHEN's, port 2121):

```bash
~/ps4-kit/scripts/upload.sh out/OrbisWeb-v01.00.pkg <the PS4's address>
```

## The sources it starts from

- **ps4-kit** at the commit named in `patches/ps4-kit/BASE`, with `patches/ps4-kit/ps4-kit-wisp.patch`
  applied (`git apply`): the thread priorities by name and the hot-spot profiler's additions.
- **Mesa**: the kit's Mesa (branch `ps4-kit`: 26.0.8 and the kit's PS4 series), six Zink commits from the
  Etterna port, then `patches/mesa/wisp-egl-uncommitted.patch` (`scripts/build-mesa.sh` has the details).
- **WebKit**: the WPE WebKit 2.54.2 release tarball committed as is, then `patches/webkit/*.patch` in order
  (`git am`).
- **Dawn**: the revision `scripts/build-dawn.sh` fetches, with `patches/dawn/0001-*.patch`.
- **UnRAR** (optional): `bash ~/wisp/scripts/build-unrar.sh` (its tarball in `~/wisp-build/src/`), then
  build with `WISP_WITH_UNRAR=1` for encrypted RAR archives in Files. Its license adds restrictions the GPL
  forbids, so packages given to others are built without it (libarchive reads RAR then).

## What the scripts do

| Script | |
|---|---|
| `scripts/env.sh` | flags for every dependency: the kit's, `-U__FreeBSD__` for C (musl's headers are not FreeBSD's), `-nostdlibinc` (no host headers), `pkg-config --static` |
| `scripts/setup-tree.sh` | `~/wisp-build`, the meson cross file, `ps4-cc`/`ps4-c++` for autotools, `libwisp_nolinux.a` (musl's Linux-only calls fail every configure check) |
| `scripts/wisp-toolchain.cmake` | the kit's CMake toolchain plus Wisp's sysroot |
| `scripts/build-deps.sh` | each library's build, resumable (`build/deps/<name>.done`) |
| `scripts/build-mesa.sh` | `~/wisp-build/src/mesa`: the kit's Mesa (branch ps4-kit) + Etterna's Zink commits + static EGL |
| `scripts/build-webkit.sh` | WPE's CMake options for the PS4; the tree `~/wisp-build/src/WebKit` is git (branch `wisp`: the release tarball, then one commit per PS4 change) |
| `scripts/build-unrar.sh` | RARLAB's UnRAR 7.3.2 as a static library (its DLL API, built unmodified, no thread pool) for the PS4 and the PC (`tests/archives`); `~/wisp-build/src/unrarsrc-7.3.2.tar.gz` is checked against its SHA-256 |
| `scripts/patch-epoxy.py` | libepoxy without dlopen: GL through `wisp_gl_lookup()` |
| `scripts/abp2webkit.py` | Adblock Plus lists to WebKit content-blocker rules (`app/data/adblock`) |
| `scripts/make-icon.py` | the package's icon and start screen |

## The WebKit changes (branch `wisp`)

- OS detection for CMake's "Generic" system; Skia's expat target name
- 16 KiB pages (`CeilingOnPageSize`); anonymous shared memory (`SHM_ANON`)
- WPEProcessManager built for the PS4; ProcessLauncherGLib uses it (the app starts the processes)
- memory figures from the app (`WTF/wtf/ps4/MemoryPS4.cpp`: no /proc, sysinfo, kinfo_proc)
- `libWPEWebKit` static
- one web process for every page (7 local processes, one GPU address space)
- CPU rendering on request (`WISP_CPU_RENDERING`)
- `DRM_FORMAT_XRGB8888` without libdrm's headers
- Skia sources the tarball's list missed (`SkPathDump`, `SkStrikeRef`)
- page reservations without memory behind them (`WTF/wtf/ps4/OSAllocatorPS4.cpp`, replacing
  `posix/OSAllocatorPOSIX.cpp`): the PS4 backs every anonymous mapping when it is made and has no
  `MAP_ALIGNED`, so JSC's aligned structure heap took 512 MiB of real memory, came back unaligned
  and stopped the process (v00.01). Reserve = `sceKernelReserveVirtualRange` with the alignment,
  commit = flexible memory mapped `MAP_FIXED`, decommit = a reservation again. JSC's structure heap
  is 128 MiB, as on Sony's PlayStation port.
- over-aligned classes get aligned memory (`WTF/wtf/FastMalloc.h`: the aligned `operator new`
  overloads in `WTF_DEPRECATED_MAKE_FAST_ALLOCATED`): without TZone allocation the class-scope
  `operator new(size_t)` hid the global aligned one, so `JSC::Lexer` (64-byte aligned) came from
  malloc at 16 bytes and the web process faulted on its first script (v00.05, SIGBUS on a 32-byte
  `vmovaps`).

- no JIT tiers (`Source/cmake/OptionsWPE.cmake`, and `build-webkit.sh` passes the same: WebKit's
  options are cached, so an existing build folder needs them on its command line): the LLInt
  interpreter runs JavaScript, as on Sony's PlayStation port; the sampling profiler goes too.
  WebAssembly came back in v00.21 without a JIT (`WISP_WEBKIT_WASM`, default 1): JavaScriptCore's
  in-place interpreter (IPInt) runs it, JavaScript enters it through the LLInt's
  `js_to_wasm_wrapper_entry`, and the BBQ and OMG tiers stay off (they are JITs). v00.27: the web
  process links a second build, `build/webkit-jit` (`WISP_WEBKIT_JIT=1` for build-webkit.sh and
  pack-webkit.sh), with the JIT and DFG tiers on (no FTL, so no BBQ: it depends on the FTL in 2.54);
  the UI and network processes keep `build/webkit-wasm`. Every WebKit patch needs both rebuilt.
- two frame buffers per page instead of four (`AcceleratedSurface.h`), 512 KiB stacks for JavaScript
  threads (`WTF/wtf/Threading.cpp`, as PLATFORM(PLAYSTATION))

The commits are exported to `patches/webkit/` (`git format-patch` from the tarball's commit).
`pack-webkit.sh` packs the objects ninja lists for the current configuration, not every object on
disk: an option that takes sources out of the build (the JIT's) leaves their old objects behind.

## Size

Each process's whole program image is charged to the flexible memory pool the app and its local
processes share (~448 MiB at the app's start), so size matters more than speed here:

| | UI | network | web | left of 448 MiB |
|---|---|---|---|---|
| v00.03: -O3, unwind tables, no function sections | 133 MiB | 129 | 149 | ~37 |
| v00.04: -O2, no unwind tables, function/data sections | 114 | 110 | 131 | ~93 |
| v00.05: + link script without `KEEP(.data.rel.ro)` | 108 | 99 | 122 | ~119 |
| v00.07: no JIT tiers; web process without Mesa (CPU) | 89 | 84 | 80 (GPU binary: 107) | ~195 |

- WebKit is built into `build/webkit-wasm` (v00.21 on; `build/webkit-small` is the same without
  WebAssembly, for `WISP_WEBKIT_DIR=~/wisp-build/build/webkit-small WISP_WEBKIT_WASM=0`) with `-O2 -ffunction-sections -fdata-sections
  -fno-unwind-tables -fno-asynchronous-unwind-tables -mprefer-vector-width=128` (`build-webkit.sh`;
  the last keeps the vectorizer's stores at the 16-byte alignment malloc gives, and Jaguar splits
  256-bit operations anyway); the app links with `--gc-sections`.
- OpenOrbis's link script (the kit's copy) KEEPs every `.data.rel.ro` section, which holds the
  vtables; a kept vtable keeps all its class's virtual functions. `app/CMakeLists.txt` takes the KEEP
  out of the build's copy.
- What remains is reached for real: the network process hosts WebKit's service-worker server, whose
  destructors reach SerializedScriptValue -> OffscreenCanvas -> ScriptExecutionContext -> Document ->
  layout (`ld.lld-21 --why-live=<symbol>` with the app's link command shows such chains; PacBrew's
  lld is too old for it).

WebKit's GResource bundles register themselves from constructors and nothing refers to their
objects, so a static link leaves them out (the web process then had no form-control images, v00.07).
`app/CMakeLists.txt` pulls WebKitResources in with `--undefined=WebKitResourcesGResourceBundle_get_resource`;
PDF.js's bundles (3.6 MiB of the image) and the media controls' stay out for now.

A change to a WebKit CMake file makes ninja want to rebuild ~1,000 objects (the copied WTF headers
get new times); when no header changed, rebuilding the changed library's target and re-packing is
enough. Delete the object of any source a change takes out of the build: `pack-webkit.sh` packs
every object it finds.
