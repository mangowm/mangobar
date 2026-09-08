# mangobar

A Wayland status bar for mangowm, built on `wlr-layer-shell`.
The system tray (StatusNotifierItem / DBusMenu) is inspired by
[swaybar](https://github.com/swaywm/sway) and [waybar](https://github.com/Alexays/Waybar).

<img width="1919" height="1079" alt="image" src="https://github.com/user-attachments/assets/6c1ab40e-96fe-41a8-9d8a-89c45fa24db8" />

## Documentation

The full user documentation lives in the
**[mangobar wiki](https://github.com/mangowm/mangobar/wiki)**:

- [Installation](https://github.com/mangowm/mangobar/wiki/Installation) —
  dependencies, build, Nix, Home Manager
- [Configuration](https://github.com/mangowm/mangobar/wiki/Configuration) —
  bar options, module placement, `max-length`, scrolling, module actions
- [Module pages](https://github.com/mangowm/mangobar/wiki/Module:-Tags) —
  one page per module (tags, window, cpu, memory, brightness, volume, clock,
  network, battery, tray, ...)
- [Styling](https://github.com/mangowm/mangobar/wiki/Styling) —
  CSS subset, `@define-color`, gradients and `mix()`
- [Custom modules](https://github.com/mangowm/mangobar/wiki/Module:-Custom)

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

Arch:

```sh
sudo pacman -S --needed meson ninja cjson wayland wayland-protocols \
  fcft tllist pixman cairo pango libpulse systemd-libs gdk-pixbuf2
```

Debian/Ubuntu:

```sh
sudo apt install meson ninja-build libcjson-dev libwayland-dev \
  wayland-protocols libfcft-dev libpixman-1-dev libcairo2-dev \
  libpango1.0-dev libpulse-dev libtllist-dev libsystemd-dev libgdk-pixbuf-2.0-dev \
  pkg-config
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

## Example

See [`config.jsonc`](config.jsonc) and
[`style.css.example`](style.css.example) for complete examples.
