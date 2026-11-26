<p align="center">
  <img src="docs/images/banner.png" alt="OrbisWeb, a WebKit browser for the PS4" width="100%">
</p>

<p align="center">
  <a href="https://github.com/iHaiDeeZ/OrbisWeb/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/iHaiDeeZ/OrbisWeb?label=release&color=5b8cff"></a>
  <img alt="PS4 homebrew" src="https://img.shields.io/badge/PS4-homebrew-1f5fd6">
  <img alt="WPE WebKit 2.54.2" src="https://img.shields.io/badge/WPE%20WebKit-2.54.2-48a0ff">
  <a href="LICENSE"><img alt="GPL-2.0-or-later" src="https://img.shields.io/badge/license-GPL--2.0--or--later-7a5cff"></a>
</p>

<h3 align="center">The modern web, on your PS4.</h3>

<p align="center">
  <a href="https://github.com/iHaiDeeZ/OrbisWeb/releases/latest"><b>⬇ Get the latest version</b></a> ·
  <a href="#getting-started">Getting started</a> ·
  <a href="#controls">Controls</a> ·
  <a href="#the-menu">The menu</a> ·
  <a href="#faq">FAQ</a> ·
  <a href="#thanks">Credits</a>
</p>

---

## Overview

OrbisWeb is a desktop-class web browser for jailbroken PS4 consoles, built on **WPE WebKit 2.54.2**, the
same engine family as Safari. It installs as an app on the home screen and brings what the PS4's own
browser never had: a JavaScript JIT, WebAssembly with SIMD and threads, WebGL 1 and 2, WebGPU, tabs,
downloads with torrents, a file manager, a media player and browser extensions.

Pages are drawn on the PS4's GPU through Mesa (Zink on RADV, adapted to submit work straight to the PS4's
AMD GPU), WebGL goes through ANGLE, and WebGPU through Dawn on Vulkan. You drive it with the DualShock 4 as
a mouse, or plug in a USB keyboard and mouse and use it like a computer.

## Highlights

**A browser made for the couch**
- Tabs, an address bar with suggestions, bookmarks and history, with a desktop-style toolbar.
- The DualShock 4 is a mouse: the left stick moves the pointer, the right stick scrolls, **Cross** clicks.
- **Triangle** opens the PS4's keyboard for the address; text fields open it too.
- A USB keyboard and mouse work as on a computer, shortcuts included (Ctrl+T, Ctrl+W, Ctrl+L, F5, F11...).

**Fast pages**
- JavaScriptCore's JIT (Baseline, DFG and FTL) and WebAssembly's BBQ and OMG compilers, with SIMD and
  threads, so heavy web apps and WebAssembly games run.
- GPU drawing for pages, canvases on the GPU, WebGL 1 and 2, and WebGPU.
- A hidden tab goes to sleep after a while, so the one you look at keeps the PS4's memory.

**Games on web pages**
- Pages that read gamepads get the DualShock 4 as a standard gamepad, vibration included.
- On pages that don't, holding **L1 + L2 + R1 + R2** turns the controller into a keyboard (arrows, Space,
  Enter, Z, X, Shift, Ctrl, Esc) for games that want keys.

**Downloads and files**
- A download manager built on aria2: HTTP, HTTPS, FTP, magnet links and `.torrent` files, several
  connections per file, pause and resume.
- **Files**, a file manager for `/data` and USB drives: copy, move, rename and delete, open ZIP, RAR, 7z,
  tar and ISO archives as folders, and compress to ZIP or 7z.
- A media player for video and music, and viewers for pictures, text and PDFs.

**Extensions and privacy**
- Browser extensions from the Chrome Web Store, Opera add-ons and Firefox's AMO (**Add to OrbisWeb** on
  their pages), or `.crx`, `.xpi` and `.zip` files from Files.
- Ad blocking with EasyList and EasyPrivacy, and skipping of YouTube's ads.

**Built for the PS4's hardware**
- WebKit's processes, memory and threads are fitted to the PS4: one web process for every page, pages
  kept in the PS4's direct memory, compiler threads that give way to the page you play.
- Logs, freeze reports, a hot-spot profiler and WebKit's remote Web Inspector, each with its switch in
  **Settings → Diagnostics**.

## Getting started

**You need:** a jailbroken PS4 with **GoldHEN**. OrbisWeb was tested on a PS4 Pro (firmware 14.00) and an
original PS4 (firmware 13.52).

1. Grab the latest **`OrbisWeb-vXX.XX.pkg`** from the [releases page](https://github.com/iHaiDeeZ/OrbisWeb/releases/latest)
   and install it with GoldHEN's **Package Installer**.
2. Start **OrbisWeb** from the home screen. It creates **`/data/Wisp/`** for your settings, downloads and
   logs.
3. Press **Triangle**, type an address, and you're browsing. **OPTIONS** opens the menu.

> **Tip:** a USB keyboard and mouse make long typing and precise clicking much easier. Plug them in at any
> time; the on-screen keyboard steps aside while a keyboard is connected.

**Updating OrbisWeb:** install the newer package on top of the old one. Your settings, bookmarks,
downloads and extensions live in `/data/Wisp/` and survive updates. **OPTIONS → About OrbisWeb** shows the
version you're on.

## Controls

### Browsing

| Press | To |
|---|---|
| Left stick | Move the pointer |
| Right stick | Scroll |
| Cross or R2 | Click |
| Square | Open a link in a new tab |
| Circle | Go back, or leave a page's full screen |
| Triangle | Type an address (the PS4's keyboard) |
| OPTIONS | Open the menu |
| L1 / R1 | Previous / next tab |
| D-pad left / right | Back / forward |
| D-pad up / down | Scroll a third of the screen |

### Games on web pages

| Press | To |
|---|---|
| (nothing) | A page that reads gamepads gets the controller by itself, with a message saying so |
| Touch pad button, held 1 s | Take the controller back from the page (hold again to give it back) |
| L1 + L2 + R1 + R2, held half a second | Switch the controller between the page and the browser |

On a page that doesn't read gamepads, the L1 + L2 + R1 + R2 hold turns the controller into a keyboard:

| DualShock 4 | Key |
|---|---|
| D-pad / left stick | Arrow keys |
| Cross | Space |
| Circle | Enter |
| Square | Z |
| Triangle | X |
| L2 / R2 | Shift / Ctrl |
| OPTIONS | Esc |

### Keyboard and mouse

| Keys | To |
|---|---|
| Ctrl+T / Ctrl+W | New tab / close the tab |
| Ctrl+Tab, Ctrl+Page Up / Down, Ctrl+1-9 | Switch tabs |
| Ctrl+L, F6 or Alt+D | Type an address |
| Alt+Left / Alt+Right, Alt+Home | Back / forward, home page |
| F5 or Ctrl+R (Ctrl+Shift+R: without the cache) | Reload |
| F11, Esc | Full screen, leave it |
| Ctrl + / - / 0 | Zoom in, out, back to 100% |

The mouse works as on a computer: right click for the page's menu, middle click for a link in a new tab, the
side buttons for back and forward, Shift + wheel to scroll sideways and Ctrl + wheel to zoom.

## The menu

**OPTIONS** opens it on every tab:

| Row | What it does |
|---|---|
| New tab / Close tab | Open a tab, close this one |
| Reload | Load the page again |
| Full screen | Hide the toolbar (Circle or Esc brings it back) |
| Add bookmark / Bookmarks / History | Keep the page, and find pages you kept or visited |
| Downloads | The download manager |
| Files | The file manager |
| Extensions | Installed extensions: turn them on or off, open their options, remove them |
| Settings | Everything below |
| About OrbisWeb | Version, this console's drawing, JavaScript and memory, and what OrbisWeb is built on |
| Restart OrbisWeb / Leave OrbisWeb | Start the browser again, or close it |

## Settings at a glance

| Group | Highlights |
|---|---|
| General | Home page, search engine |
| Privacy | Cookies, ad blocking (EasyList and EasyPrivacy), skipping YouTube's ads |
| Pages and JavaScript | The JavaScript and WebAssembly JITs and their tiers, WebAssembly SIMD and threads, WebGL, WebGPU, autoplay, video quality cap, H.264-only video, service workers, memory for compiled code |
| Drawing and memory | CPU + GPU or GPU drawing, canvases on the GPU, painting threads, when a hidden tab sleeps, the web process's heap, a memory meter |
| Controller | Pointer size, the controller's name for pages |
| Downloads | The download manager, connections per file, files at once, seeding finished torrents |
| Player | Video decoding threads, playing the next file |
| Diagnostics | Logs (page messages, memory, GPU, WebGPU, sound, failed loads), thread dumps of a page that stops answering, the hot-spot profiler, the remote Web Inspector |

## Your files

Everything OrbisWeb creates sits in **`/data/Wisp/`** (Wisp is the project's working name):

- `Downloads/`: what you download
- `settings.txt`: your settings
- `bookmarks.tsv`, `history.tsv`: bookmarks and history
- `extensions/`: installed extensions
- `profile/`: the sites' cookies and storage
- `aria2/`: the download manager's sessions and torrent state
- `player/`: the media player's recent files and where each one stopped
- `cache/`: the web cache, media and the GPU's shader cache
- `logs/`: `ui.log`, `web.log`, `net.log` and `web.hang.log`, for bug reports
- `kit-ui/`, `kit-web-gpu/`, ...: each process's boot trace, crash report and profile

## FAQ

**Why do PS2 or N64 emulator sites run slowly?**
They run a console emulator compiled to WebAssembly, usually on one thread. OrbisWeb compiles it with its
fastest tier, and that thread still fills one of the PS4's CPU cores: the PS4's CPU is the limit, not the
browser. A native emulator or a native port of the game runs far better on the same console.

**A page says WebGL or WebGPU isn't available.**
Both need GPU drawing: check **Settings → Drawing and memory → Drawing** is *CPU + GPU* or *GPU*, and that
*WebGL 1 and 2* and *WebGPU* are on in **Pages and JavaScript**.

**A page keeps crashing.**
Turn **Settings → Pages and JavaScript → JavaScript JIT** off for a test: the interpreter is slower but
simpler. If that helps, please report the page.

**Videos stutter.**
Try **Video quality cap** and **H.264 video only** in **Pages and JavaScript**: H.264 is the cheapest
format for the PS4 to decode.

**How do I report a bug?**
[Open an issue](https://github.com/iHaiDeeZ/OrbisWeb/issues) with the page's address, what happened, your
PS4 model and firmware, and the OrbisWeb version (**About OrbisWeb**). Attach `logs/web.log`, `logs/ui.log`
and, if there is one, `kit-web-gpu/crash.log` from `/data/Wisp/`.

## Good to know

- Every tab shares one web process. If it crashes, its tabs offer a reload; **OPTIONS → Restart OrbisWeb**
  starts everything again.
- Very large web games can run out of the PS4's memory: close other tabs first.
- The first time ad blocking is turned on, its lists compile for about a minute.
- Release builds read RAR archives through libarchive; encrypted RAR archives can't be opened.
- Sites behind a Cloudflare check can take a while to let you in.

## Building

Everything builds in WSL with [ps4-kit](https://github.com/iHaiDeeZ/ps4-kit) on the OpenOrbis toolchain.
The steps, scripts and every WebKit change are in **[docs/BUILDING.md](docs/BUILDING.md)**; the PS4 changes
to WebKit, Mesa, Dawn and ps4-kit are in [`patches/`](patches). [docs/DESIGN.md](docs/DESIGN.md) is the
project's diary, version by version.

## Thanks

- **[WebKit](https://webkit.org)** and **[WPE](https://wpewebkit.org)** (Igalia and the WebKit
  contributors): the engine
- **[Mesa](https://mesa3d.org)**: Zink and RADV draw every page
- **[Dawn](https://dawn.googlesource.com/dawn)**, **[ANGLE](https://angleproject.org)** and
  **[Skia](https://skia.org)**: WebGPU, WebGL and 2D drawing
- **[GStreamer](https://gstreamer.freedesktop.org)** and **[FFmpeg](https://ffmpeg.org)**: video and sound
- **[aria2](https://aria2.github.io)** and **[libarchive](https://libarchive.org)**: downloads and archives
- **[EasyList](https://easylist.to)**: the ad-blocking lists
- **[Vita3K](https://vita3k.org)**: the software canvas the interface is drawn with
- **[OpenOrbis](https://github.com/OpenOrbis)**, **[PacBrew](https://github.com/PacBrew)** and
  **[GoldHEN](https://github.com/GoldHEN/GoldHEN)**: the PS4 toolchains and the homebrew enabler

## License and disclaimer

OrbisWeb is released under the **GNU General Public License, version 2 or later**. See [LICENSE](LICENSE).
Its interface canvas comes from Vita3K's PS4 port (GPL 2 or later) and its shell from WinePS4 (LGPL 2.1).
Every library it is built on keeps its own license: WebKit (LGPL 2.1 and BSD), Mesa (MIT), Dawn (Apache
2.0), ANGLE and Skia (BSD), GStreamer and FFmpeg (LGPL 2.1), aria2 (GPL 2), libarchive (BSD), EasyList and
EasyPrivacy (GPL 3 / CC BY-SA 3.0), and the Noto, Liberation and Roboto fonts (OFL, Apache 2.0). The About
page lists them on the console.

This is a fan-made project with no ties to Sony Interactive Entertainment, Apple, Igalia or Google.
PlayStation and PS4 are trademarks of Sony Interactive Entertainment; WebKit is a trademark of Apple Inc.
OrbisWeb is a browser: what you open with it is up to you. Please only play games you own.
