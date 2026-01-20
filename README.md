# XFCE Ask (XFCE panel plugin)

A small XFCE panel plugin for quick one-off questions to any OpenAI-compatible Chat Completions endpoint. Type a question in the panel entry and press Enter; the answer shows in a GTK popover.

## Behavior

- `Enter`: send the current prompt.
- Follow-ups are state-based: if the popover is still open, the next `Enter` is treated as a follow-up (limited context is kept); closing the popover ends the session.

## Build

Dependencies (dev headers): `gtk3`, `libxfce4panel`, `libsoup-3`, `json-glib`, `libsecret`.

### Make

```sh
make
```

## Install (local)

### Make

```sh
make install
xfce4-panel -r
```

Then add the plugin in Panel Preferences → Items → `+` → “XFCE Ask”.

## Configure

Right-click the plugin → Properties:

- Endpoint: e.g. `https://api.openai.com/v1/chat/completions`
- Model: e.g. `gpt-4o-mini`
- Temperature
- API key: stored in the system keyring (per-endpoint)

## Debugging

The plugin writes a debug log to:

- `~/.cache/openai-ask/openai-ask.log`

Tail it while testing:

```sh
tail -f ~/.cache/openai-ask/openai-ask.log
```
