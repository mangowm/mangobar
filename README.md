# mangobar

A Wayland status bar for mangowm, built on `wlr-layer-shell`.
The system tray (StatusNotifierItem / DBusMenu) is inspired by
[swaybar](https://github.com/swaywm/sway) and [waybar](https://github.com/Alexays/Waybar).

<img width="1919" height="1079" alt="image" src="https://github.com/user-attachments/assets/6c1ab40e-96fe-41a8-9d8a-89c45fa24db8" />


## Install

### Dependencies

Build tools: `meson`, `ninja`, `wayland-scanner` (part of `wayland`),
`pkg-config`.

Libraries:

- `cjson`
- `wayland`
- `wayland-protocols`
- `fcft`
- `tllist`
- `pixman`
- `cairo`
- `pango`
- `libpulse`
- `systemd-libs`
- `gdk-pixbuf2`
- `alsa-lib`

Arch:

```sh
sudo pacman -S --needed meson ninja cjson wayland wayland-protocols \
  fcft tllist pixman cairo pango libpulse systemd-libs gdk-pixbuf2 alsa-lib
```

Debian/Ubuntu:

```sh
sudo apt install meson ninja-build libcjson-dev libwayland-dev \
  wayland-protocols libfcft-dev libpixman-1-dev libcairo2-dev \
  libpango1.0-dev libpulse-dev libtllist-dev libsystemd-dev libgdk-pixbuf-2.0-dev \
  libasound2-dev pkg-config
```

### Build

```sh
git clone https://github.com/mangowm/mangobar.git
cd mangobar
meson setup build -Dprefix=/usr
ninja -C build
sudo ninja -C build install
```

If `meson.build` changes after a `git pull`, ninja re-runs meson
automatically; install any newly added dependencies first.

Runtime note: `pamixer` / `brightnessctl` are only needed if your
config's click/scroll actions call them.

### Nix

Build the package locally with:

```sh
nix build .#mangobar
./result/bin/mangobar
```



### Home Manager

The flake exports a Home Manager module. Add it to your flake inputs and
enable the user service:

```nix
{
  imports = [ inputs.mangobar.homeManagerModules.default ];

  services.mangobar = {
    enable = true;
    systemdTarget = "mango.target";
  };
}
```

The service starts with `graphical-session.target` by default. Set
`systemdTarget` to your compositor's user target, such as `mango.target`.
Override `services.mangobar.package` to use a different mangobar derivation.

Set `services.mangobar.settings` to generate
`~/.config/mangobar/config.jsonc` from a Nix attribute set, or use
`services.mangobar.configFile` for an existing JSONC file. These options are
mutually exclusive. Changing either configuration restarts the service during
Home Manager activation; rebuilding the mangobar package does as well.

```nix
services.mangobar.settings = {
  modules-left = [ "workspaces" "layout" "window" ];
  modules-right = [ "cpu" "memory" "clock#time" ];
};
```

## Usage

Run `mangobar` inside a mangowm session. It reads configuration from:

1. `$MANGOBAR_CONFIG`
2. `~/.config/mangobar/config.jsonc`

The JSONC config supports `height`, `layer`, `buffer-scale`, `css`
(or `style`), and per-module `format`, `interval`, `on-click`,
`on-scroll-*` etc. Unsupported modules are ignored. All runtime settings
come from JSONC and CSS; fonts are set in CSS only (`font-family`,
`font-size`, `font-weight`). See [`config.jsonc`](config.jsonc) for a complete
example. Any module can be placed in `modules-left`, `modules-center` or
`modules-right`; left is anchored to the left edge, right to the right edge
and center is centered between them.

`scroll-interval` (ms, default `0` = disabled) debounces scroll actions per
module: repeated scrolls inside the interval keep resetting the timer, so a
continuous scroll only triggers once. It can be set at the top level or in a
module block (`"workspaces": { ..., "scroll-interval": 100 }`), where the
module value wins.

`smooth-scrolling-threshold` controls how much continuous pointer-axis motion
(such as a touchpad two-finger gesture) produces one scroll action. It defaults
to `5.0`. Mangobar accumulates motion independently for horizontal and vertical
axes, preserving any amount below the threshold for the next frame. Discrete
mouse-wheel steps continue to use their protocol-provided step count. Set it
at the top level for all modules, or in a module block (as in Waybar) to
override that module:

```jsonc
"backlight": { "smooth-scrolling-threshold": 5.0 }
```

Every module accepts `"max-length"` (in characters, `0` = unlimited, the
default for all modules). The `window` module is squeezed by the other
modules when no limit is set: it shows fully up to the remaining bar width,
or up to `"max-length"` when configured. Truncated text gets a `...` suffix.

`buffer-scale` is a multiplier on top of the output's Wayland scale
(default `1`); leave it at `1` to follow the display's HiDPI scale
automatically. Text, icons and menus are rendered at the effective scale
so they stay sharp.

Any module with a `format` can also set `format-alt`; a left click toggles
between the two formats (the module's `on-click` command still runs if
configured). The network module additionally supports `{down}` / `{up}` in
its format strings, e.g. `"format-alt": "↓{down} ↑{up}"` for live speeds.

CSS priority is `$MANGOBAR_CSS` > `~/.config/mangobar/style.css`.

## Modules

- `tags` / `layout` / `title` / `keymode` / `keyboardlayout`: from mangowm IPC
  (`workspaces` also accepts `pinned` — tag numbers kept visible even when
  empty — and `tag-names`, an array of custom labels, index 0 = tag 1)
- `cpu`: `{usage}` is the CPU usage percent and `{load}` is the 1-minute
  load average (two decimals); `mem` reads `/proc`
- `brightness`: read `/sys/class/backlight` (auto-detected or the JSONC
  `device` field); updates immediately on external changes via udev
- `volume`: read via the PulseAudio library, with ALSA fallback; shows mute
  state and updates immediately on external changes via PulseAudio events
- `clock`: time (`#clock`) and date (`#clock.date`) with separate CSS;
  date names follow the system locale (e.g. Chinese month/weekday names)
- `network`: shows the active interface name; click toggles up/down speeds
  (KB/s below 1MB/s, MB/s otherwise)
- `hideclients`: shows the hidden-window count for the monitor; the module
  hides itself while the count is zero
- `battery`: charge percent and status (`{percent}`, `{status}`, `{icon}`);
  `"icons"` is an array of nerd-font icons shown by charge level while
  discharging, `icon-charging` / `icon-full` override the charging/full
  states, and `{ac}` / `icon-ac` mark a plugged-in adapter;
  `"hide-on-ac": true` hides the module entirely while plugged in
- `brightness` / `volume`: `"icons"` arrays switch the `{icon}` by level
  (e.g. `["󰃚", "󰃛", "󰃜", "󰃝", "󰃞", "󰃟"]`); volume also has `icon-muted`
  and `{bt}` / `icon-bluetooth` to show a bluetooth mark when the active
  sink is a bluetooth device
- `tray`: StatusNotifierItem / DBusMenu, with a side-opening submenu
- `custom/<name>`: user-defined modules (see below)

## Custom modules

Add `custom/<name>` to `modules-left` or `modules-right` and define it in the
same JSONC file:

```jsonc
"custom/power": {
    "format": "󰣇",
    "on-click": "wlogout",
    "on-click-right": "swaync-client -t -sw"
},
"custom/uptime": {
    "exec": "uptime -p | sed 's/^up //'",
    "interval": 60,
    "format": "󰅐 {}",
    "on-scroll-up": "brightnessctl s +5%",
    "on-scroll-down": "brightnessctl s 5%-"
}
```

Fields:

- `exec`: command whose stdout becomes the module text (trailing newline trimmed)
- `interval`: refresh interval in seconds; `0`/omitted runs once at startup
- `format`: shown as-is, with `{}` replaced by the exec output; if omitted the
  raw exec output is shown
- `on-click` / `on-click-middle` / `on-click-right` / `on-scroll-up` /
  `on-scroll-down`: shell commands or `@ipc:xxx` commands

Custom modules are styled with `#custom-<name>` selectors, e.g.
`#custom-power { background-color: #cc241d; color: #ffffff; }`.

Right-click a tray item with a menu to open a context menu; clicking outside
closes it.

## Styling

`~/.config/mangobar/style.css` supports a small CSS subset: `color`,
`background`, `padding`, `margin`, `border-radius`, `min-width`,
`font-family/size/weight`, `@define-color`, and `linear-gradient` (first
color). Selectors: `*`, `#module`, `#module.state`, and `#custom-<name>`.

See [`style.css.example`](style.css.example).

## Module actions

Actions are configured through the JSONC config's `on-click` /
`on-scroll-*` fields. Special commands:

- `@view` / `@toggle`: switch / toggle a tag over IPC
- `@ipc:xxx`: send `xxx` verbatim over IPC
- anything else: run via `/bin/sh -c`
