# Zoomer

<img width="480" height="270" alt="Image" src="https://github.com/user-attachments/assets/32a2fbc3-1348-4fa3-90ae-8c0c9150417a" />

*Demo by [Ranthos](https://github.com/mitterdoo)*

A Wayland screen magnifier inspired by [boomer](https://github.com/tsoding/boomer) by Tsoding.

---

## Overview

Zoomer is a low-level screen zoom tool that works directly with Wayland. It captures your entire desktop, composites all monitors into a single buffer, and renders it with smooth physics-based zoom and pan.

Two capture backends are supported:

- **wlroots** — used on compositors like Hyprland via the `ext-image-copy-capture` protocol
- **PipeWire portal** — used on Plasma and Niri (with [SHM patch](https://github.com/niri-wm/niri/pull/1791)) via `xdg-desktop-portal`

It should work on any Wayland compositor that supports these protocols, but this project was built for fun and **no support is provided**.

---

## Dependencies

Runtime dependencies (dynamically linked):

| Library | Package | Required |
|---|---|---|
| `libEGL` | libglvnd | yes |
| `libGL` | libglvnd | yes |
| `libwayland-client` | wayland | yes |
| `libwayland-egl` | wayland | yes |
| `libwayland-cursor` | wayland | yes |
| `libdl` | glibc | yes |
| `libm` | glibc | yes |
| `libsystemd` | systemd | optional |
| `libpipewire-0.3` | pipewire | optional |

`libsystemd` and `libpipewire-0.3` are only needed for the PipeWire portal capture backend, used on compositors like Plasma and Niri. They are loaded at runtime via `dlopen` rather than linked directly.

> [!IMPORTANT]
> Because `libsystemd` and `libpipewire-0.3` are loaded via `dlopen`, they must be discoverable on your library path at runtime. Make sure they are present in your `LD_LIBRARY_PATH` or in a path your dynamic linker searches by default (e.g. `/usr/lib`). On NixOS, the provided flake handles this automatically via `makeWrapper`.

---

## Building

### With Nix

A flake is provided. To build:

```bash
nix build
```

To enter a development shell:

```bash
nix develop
```

Then build with make:

```bash
make
```

### Without Nix

You will need:

- `gcc`
- `pkg-config`
- `wayland-scanner`
- `wayland` + `wayland-protocols` + `wlr-protocols`
- `libGL` / `mesa`
- `libsystemd` (optional, for portal backend)
- `libpipewire-0.3` (optional, for portal backend)

Then:

```bash
make
```

---

## Configuration

Zoomer reads a config file at `~/.config/zoomer/config` on startup. If it does not exist, defaults are used.

```ini
# how quickly scrolling zooms in/out
scroll_speed = 5.000

# how quickly panning slows down after dragging
drag_friction = 5.000

# how quickly zooming slows down after scrolling
scale_friction = 10.000
```

---

## Controls

| Input | Action |
|---|---|
| Scroll wheel | Zoom in / out |
| `=` | Zoom in |
| `-` | Zoom out |
| Click and drag | Pan |
| `F` | Toggle flashlight mode |
| `Ctrl` or `Shift` (held) | Adjust flashlight radius |
| `R` | Reset view |
| `Esc` | Quit |

---

## Known Working

- **Hyprland** — wlroots backend
- **Plasma** — PipeWire portal backend
- **Niri** — PipeWire portal backend, requires the SHM patch ([PR #1791](https://github.com/niri-wm/niri/pull/1791)) until it is merged upstream

For NixOS, the patch can be applied in your niri module:

```nix
programs.niri.package = pkgs.niri.overrideAttrs (old: {
    patches = (old.patches or [ ]) ++ [
        (pkgs.fetchpatch {
            name = "niri-shm-screenshare.patch";
            url = "https://github.com/wrvsrx/niri/compare/tag_support-shm-sharing_4~19..tag_support-shm-sharing_4.patch";
            hash = "sha256-LLbzjrUmCXOCqboGKFc19Lw7hyE2tMHJdadWtltfn5U=";
        })
    ];
});
```
