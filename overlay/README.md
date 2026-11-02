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
