# ps5-overlay-poc

An in-game overlay for PS5 homebrew (firmware 13.00, GPL-3.0-or-later):
panels and labels drawn over a running game or app, opened and closed with
the L3+R3 chord, live-updated from a text channel. Proven on console end to
end: injection, render and pad hooks at 60 Hz, visible UI, gesture toggling,
clean teardown.

## Where things live

| Path | What it is | Who edits it |
|---|---|---|
| `src/app/` | The UI: `oui` Builder API + the current overlay app | UI tasks — start at `src/app/README.md` |
| `src/core/` | The engine: loader, hooks, scene lifecycle, Builder impl | Engine features only |
| `vendor/shellui-loader/` | GPL loader lineage (see its SOURCES.md) | Nobody, pinned |
| `tests/` | Host suites: protocol/hook safety, fake-runtime app+engine tests | With their owners |
| `tools/` | ELF embedding and validation | Rarely |
| `evidence/` | Console session notes and findings | Append-only |

## Quick start

```
make test CXX=clang++ CC=clang   # host tests (PC)
make all                         # payloads (build PC; needs PS5_PAYLOAD_SDK)
```

`overlay-widget.elf` runs the UI overlay; `overlay-input.elf` observes pad
events; `overlay-resolve.elf`/`overlay-observe.elf` are engine diagnostics;
`overlay-restart.elf` recycles the system UI for a fresh attempt. Run logs
and the live text channel land in the overlay data folder on the console.

Status, console findings and the full session history:
`evidence/notes-our-overlay-20261102.md`.

## Standalone pill lab

Build on the build PC, from `overlay/`: `make pill-lab` (requires
`PS5_PAYLOAD_SDK`). This produces `build/pill-lab.elf`, with its own worker,
blob and matching build ID. It is opt-in and is not part of the Discord app
package. Host checks: `make test CXX=clang++` (lab PNG checks also need host
zlib headers/library). `make lab-preview CXX=clang++` writes editor-on,
editor-off, four-corner and long/muted scenarios under `build/lab-preview-out/`;
`tools/preview.py` uses Pillow. These previews approximate PUI text metrics;
final text measurement and tuning happen on the TV.

Before launching, quit **both the Discord app and its core**. Send
`build/overlay-restart.elf` or reboot ShellUI so the overlay injection slot
is free. Then, from `overlay/`, send:

```sh
nc <ps5> 9021 < build/pill-lab.elf
```

The lab starts captured, showing a synthetic live pill and a centre-screen
editor. It ignores Discord feeds and needs no Discord core. Its mode-5
loader supplies the normal worker keepalive; losing that loader still tears
down safely. The lab alone disables the 30-second capture idle release.

- D-pad up/down: select parameter; left/right: adjust one step.
- L1/R1: decrease/increase by ten steps (clamped).
- Cross: toggle boolean or cycle choice; numeric values use left/right.
- Triangle: reset all values to the next preset, Original or Notch.
- Square: hide/show editor; held repeats do not toggle it again.
- R2: save now. Circle: release controller capture; pill stays visible.
- L3+R3 or hold Options for 600 ms: recapture, even with editor hidden.

All 66 parameters live in one table. The `preset` row also selects/resets a
preset; other edits retain that preset's name as their starting point.
`long_time` switches the timer between 12:34 and 1:02:45, `viewers` edits the
count, and `muted` switches the mic glyph/tint. Corner values are TL/TR/BL/BR
(0/1/2/3). Radius roles map to the screen-facing outer corner, its two
adjacent edge corners, and the centre-facing inner corner. Radii clamp to
half the current height. RGB uses 0–255, alpha uses percent, and all layout
rectangles snap to 0.5 layout pixels (one physical 4K pixel). Font size is
integer. Fixed time slots measure all ten repeated-digit strings and keep
the widest; measured mode follows the actual timer width. Glass is a
translucent fill, matching the reference's colours; no backdrop blur.

Values autosave after 150 ms without a value change, using temp-file plus
rename, to `/user/data/discord-overlay-poc/lab/pill-lab.txt`. Startup loads
known valid keys, clamps values and ignores malformed records. Each save
also appends one copyable `PILL_LAB name=value ...` line to
`/user/data/discord-overlay-poc/worker.log`. Copy the values file for later
integration into the real overlay; the lab does not apply them there.
Background PNGs are versioned under the same `lab/` directory and regenerate
only when raster geometry/materials change and input settles. The previous
PNG is deleted after switching images; the final PNG remains on teardown.
