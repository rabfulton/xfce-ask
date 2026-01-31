# XFCE Ask (XFCE panel plugin)

![XFCE Ask screenshot](screenshots/screenshot.png)

A small XFCE panel plugin for quick one-off questions to any OpenAI-compatible Chat Completions endpoint. Type a question in the panel entry and press Enter; the answer shows in a GTK popover.

Works with any OpenAI-compatible endpoint, including local/self-hosted ones. If your endpoint does not require auth, leave the API key blank.

License: MIT (see `LICENSE`).

## Behavior

- `Enter`: send the current prompt.
- Follow-ups are state-based: if the popover is still open, the next `Enter` is treated as a follow-up (limited context is kept); closing the popover ends the session.

## Build

Dependencies (dev headers): `gtk3`, `libxfce4panel`, `libsoup-3`, `json-glib`, `libsecret`.

Arch:
```sh
sudo pacman -S xfce4-panel gtk3 libsoup3 json-glib libsecret pkgconf
```

Debian/Ubuntu:
```sh
sudo apt install libxfce4panel-2.0-dev libgtk-3-dev libsoup-3.0-dev libjson-glib-dev libsecret-1-dev pkg-config
```

Fedora:
```sh
sudo dnf install xfce4-panel-devel gtk3-devel libsoup3-devel json-glib-devel libsecret-devel pkgconf
```

openSUSE:
```sh
sudo zypper install xfce4-panel-devel gtk3-devel libsoup-3_0-devel json-glib-devel libsecret-devel pkgconf-pkg-config
```

Then build:

```sh
make
```

## Install

```sh
sudo make install
xfce4-panel -r
```

By default, the Makefile auto-detects install paths from pkg-config. Override if needed:

```sh
# User-local install (no sudo)
make PREFIX=$HOME/.local install

# Explicit system install
sudo make PREFIX=/usr install

# Fedora/lib64 systems
sudo make LIBDIR=/usr/lib64 install
```

## AUR

If you use Arch Linux or a derivative thereof use:

- AUR package: https://aur.archlinux.org/packages/xfce4-panel-xfce-ask-git

## Pre-built Packages

Download `.deb` or `.rpm` from the [Releases](https://github.com/rabfulton/xfce-ask/releases) page.

Debian/Ubuntu:
```sh
sudo apt install ./xfce-ask_*.deb
```

Fedora:
```sh
sudo dnf install ./xfce-ask-*.rpm
```

Dependencies are installed automatically.

## Configure

Right-click the plugin → Properties:

- Endpoint: e.g. `https://api.openai.com/v1/chat/completions`
- Model: e.g. `gpt-4o-mini`
- Temperature
- API key: stored in the system keyring (per-endpoint)

## Debugging

The plugin writes a debug log to:

- `~/.cache/openai-ask/openai-ask.log`

Logging is disabled by default. Enable it by starting your session/panel with `XFCE_ASK_DEBUG=1`.

Tail it while testing:

```sh
tail -f ~/.cache/openai-ask/openai-ask.log
```

## Other Useful Projects

- A lightweight speech to text implementation [Auriscribe](https://github.com/rabfulton/Auriscribe)
- A full featured AI application [ChatGTK](https://github.com/rabfulton/ChatGTK)
- A Markdown notes application for your system tray [TrayMD](https://github.com/rabfulton/TrayMD)