# rewm — JIT-reloadable window manager

A minimal dwm-style X11 window manager where the core logic is
**JIT-compiled at runtime** via [c2mir/MIR](https://github.com/commrade-goad/cmm).

Edit `src/wm.c`, save, and the WM hot-reloads — no restart, no lost windows.

## Architecture

```
┌──────────────────────────────────────────────────┐
│                 rewm (native binary)              │
│                                                   │
│  src/host.c                                       │
│    ├── XOpenDisplay / XSelectInput                │
│    ├── init WMState (heap, survive reload)        │
│    └── loop:                                      │
│          ├── rewm_compile("src/wm.c")              │
│          ├── call wm_entry(&state)                 │
│          └── if RELOAD → re-compile → loop         │
│                                                   │
│  src/c2mir-lib.c  — wrapper:                     │
│    ├── c2mir_compile() → MIR modules              │
│    ├── MIR_gen() → native code                     │
│    └── import_resolver (X11/libc symbols)          │
└──────────────────────┬───────────────────────────┘
                       │ WMState* (native heap)
┌──────────────────────▼───────────────────────────┐
│  wm.c (JIT-compiled, hot-reloadable)              │
│                                                   │
│  wm_entry(WMState *s):                            │
│    ├── grab keys / buttons                        │
│    ├── XNextEvent loop                            │
│    ├── keypress / buttonpress / maprequest / etc  │
│    ├── client management (create/destroy/focus)   │
│    ├── layouts (tile, monocle, floating)          │
│    └── return REWM_QUIT / REWM_RELOAD             │
└──────────────────────────────────────────────────┘
```

## Build

```sh
# from source:
gcc -x c -o nob build.cmm -ldl -lm -lpthread
./nob

# run (must be in an X session):
./rewm
```

Requires: `gcc`, `libX11`, `libXft`, `fontconfig`, `libXinerama`.

## Hot-reload

1. Edit `src/wm.c`
2. Save — the host detects mtime change
3. MIR context is torn down and re-created
4. `wm.c` is recompiled and `wm_entry` re-entered
5. **WMState stays intact** — all windows, monitors, focus preserved

To trigger reload from inside the WM: bind a key that returns `REWM_RELOAD`
(currently unused — for v2).

## Features (current)

| Feature | Status |
|---|---|
| Tile layout | ✅ |
| Monocle layout | ✅ |
| Floating layout | ✅ |
| Keybindings | ✅ (hardcoded — see below) |
| Client create/destroy/focus | ✅ |
| Border highlights on focus | ✅ |
| Multi-tag support (1–9) | ✅ |
| Hot-reload on file change | ✅ |
| spawn terminal / dmenu | ✅ |
| Mouse move/resize | 🔶 (basic) |
| Status bar | ❌ |
| Systray | ❌ |
| Xinerama multi-monitor | ❌ (single monitor) |
| Config file (config.h) | ❌ (hardcoded) |
| Rules (window class matching) | ❌ |
| EWMH/NetWM hints | ❌ (partial) |
| drw drawing library | ❌ |
| Fonts & color schemes | ❌ |
| Mouse button bindings | ❌ |
| Stack-based focus history | ❌ |
| Fullscreen toggle | ❌ |
| Status text via stdin | ❌ |

### Keybindings (hardcoded)

| Key | Action |
|---|---|
| `Mod+Return` | spawn terminal (`st`) |
| `Mod+p` | spawn dmenu (`dmenu_run`) |
| `Mod+q` | quit WM |
| `Mod+j` / `Mod+k` | focus next / prev client |
| `Mod+Tab` | cycle focus |
| `Mod+h` / `Mod+l` | decrease / increase master area |
| `Mod+1`…`Mod+9` | switch to tag |
| `Mod+Button1` | move window |
| `Mod+Button3` | resize window |

Mod = Super (Mod4).

## What was cut from dwm

`wm.c` is **434 lines** vs dwm's ~3200 (dwm.c + drw.c + util.c).
Removed to keep the initial JIT port minimal:

- **drw** (drawing library) — the full text rendering / color / font layer.
  Requires Xft/fontconfig which adds library symbol resolution complexity
  for the JIT import resolver.
- **Status bar** — no bar drawing, no layout symbol, no window title,
  no status text, no systray.
- **config.h** — keybindings, rules, layouts, colors, fonts are all
  hardcoded in wm.c.  A future version will read a C config file
  that's also JIT-compiled alongside wm.c.
- **Rules** — auto-tagging or floating windows by class/instance/title.
- **Xinerama** — single monitor only.  Multi-mon via `XineramaQueryScreens`
  is straightforward to add back.
- **Mouse button bindings** — clicking on tags/layout symbols on the bar.
  (No bar → no clicks.)
- **Fullscreen / EWMH** — `_NET_WM_STATE_FULLSCREEN`, `_NET_ACTIVE_WINDOW`,
  and friends are declared in WMState but not fully wired.
- **Stack-based focus history** — dwm's `stack` for Alt+Tab ordering.
  wm.c cycles by linked-list order instead.
- **Floating window drag** — no `mousemove`/`mouseresize` handlers yet.
- **Updatestatus / signal-driven status** — dwm reads status text from
  stdin and refreshes on `SIGUSR1`.
- **XResources** — no `.Xresources` reload.
- **Gaps patch** — the `gappx` variable is declared in the Monitor struct
  and initialised, but the tile/monocle layouts don't apply gaps.

## Why JIT? / Prior art

The usual dwm config story: edit `config.h` → `make` → `kill -HUP` → lose
all windows.  With the JIT approach the X11 connection and all client state
live in the native heap (`host.c`), while the actual WM logic (event loop,
layouts, keybindings) lives in a `.c` file that gets compiled to MIR and
JIT-executed by c2mir.  Editing the logic means the host tears down the
MIR context, re-reads the file, re-compiles, and calls the entry point
again — state intact.

Performance is near-native after the MIR codegen pass (O3 equivalent)
and compile times are in the low milliseconds.

## License

MIT (see [dwm's original MIT license](https://git.suckless.org/dwm/file/LICENSE.html)).
