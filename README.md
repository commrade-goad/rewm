# rewm — JIT-reloadable X11 window manager

`rewm` is a minimal, **dwm-style X11 window manager** where WM logic is compiled at runtime using a CMM/MIR JIT pipeline.

Instead of rebuilding and restarting the WM for every logic change, you edit the WM source and reload behavior through the host/JIT flow.
By this design you are not only stuck with the default dwm-style wm, you can modify the `src/wm.c` live to make it floating wm etc...

There is limitation if you need add/load more libs, you need to edit the build.c to link with that lib on the native apps for now... (will be worked on in the future!)

---

## Important warning

This WM can use **more RAM than expected**.  
It is effectively a **C compiler/runtime pipeline bundled with X11/window-management libraries**, not just a tiny static WM binary.

If you want extremely low memory overhead, this design may not match your expectations  and just use dwm.

---

## What it is

- Native host process managing X11 connection and lifecycle
- Runtime compilation of WM logic (`src/wm.c`) through CMM/MIR
- Reload-oriented architecture where runtime state is designed to survive recompiles
- Tile/monocle/floating style behavior (based on current project docs)

---

## High-level architecture

- **Host layer** (native binary):
  - Initializes display and WM state
  - Compiles/recompiles WM logic
  - Enters WM entrypoint and handles reload loop

- **JIT/compiler layer**:
  - Compiles WM source at runtime
  - Resolves required symbols (X11/libc/etc.)
  - Produces executable machine code

- **WM logic layer** (`src/wm.c`):
  - Event loop and key/mouse handling
  - Client/window lifecycle
  - Layout behavior (tile/monocle/floating)
  - Returns control flags like reload/quit

---

## Requirements

If the included CMM is not compatible please just build [CMM](https://github.com/commrade-goad/cmm) and copy it to deps!

Typical Linux/X11 dependencies:

- `gcc`
- `libX11`
- `libXft`
- `fontconfig`
- `libXinerama`
- standard C runtime/development tooling (`dl`, `m`, `pthread` link deps where needed)

> Package names vary by distribution (`-dev` / `-devel` variants).

---

## Build

From repository root:

```sh
gcc build.c -o nob
./nob
```

This should produce the `rewm` executable.

---

## Launch

Use this example to launch rewm:

```sh
export REWM_PATH="$HOME/Documents/dev/rewm/src/"
export REWM_CFLAGS="-I/usr/include/freetype2"
exec $HOME/Documents/dev/rewm/rewm &> /tmp/rewmlog
```

---

## Hot-reload workflow

1. Start `rewm`
2. Edit WM logic source (typically `src/wm.c`)
3. Trigger/allow reload path
4. Host recompiles and re-enters WM logic
5. WM state is intended to persist across reloads

---

## Current feature snapshot

Based on current repository documentation:

- ✅ Tile layout
- ✅ Monocle layout
- ✅ Floating layout
- ✅ Keybindings (hardcoded)
- ✅ Client create/destroy/focus
- ✅ Multi-tag support (1–9)
- ✅ Hot-reload on file change
- ✅ Spawn terminal / dmenu
- ✅ Basic mouse move/resize
- ✅ Status bar
- ❌ Systray (will not add!)

---

## Notes

- This project is experimental by nature due to runtime compilation architecture.
- Behavior and keybindings may change rapidly.
- Expect rough edges compared to mature static WMs.
- As the dev i too _dogfooding_ this wm right now 🗿.
