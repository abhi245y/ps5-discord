# Overlay engine — stable, do not edit from UI tasks

This directory is the console plumbing behind the `oui` API in `src/app`:
process targeting and identity, the request/status protocol, loading the
worker, method resolution, code hooks, the overlay scene lifecycle, the
Builder implementation, and teardown.

It is proven working on firmware 13.00 (see evidence/notes-*). Change it
only for engine features (custom fonts, more gestures, more widget kinds,
per-game settings plumbing), not for UI work — the app layer owns that.

Layout:
- `protocol.hpp` — request/status wire format, resolved-symbol table, modes.
- `hook.hpp` — 16-byte atomic code patch, trampoline, protection handling
  (exec-only JIT pages: read permission only while copying, restored after).
- `worker.cpp` — the injected engine: Update + pad hooks, own overlay scene
  (`Application.RootScene` -> new Scene -> `AddScene`), Builder vtable,
  `oui::app::*` callbacks, queued scene teardown, text channel polling.
- `parent.cpp` — the loader payloads (resolve/observe/widget/input/restart).

Firmware and console constraints learned the hard way live in the comments
and in evidence/notes-our-overlay-20261102.md; read them before touching
anything here.
