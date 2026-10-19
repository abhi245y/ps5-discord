# ps5-discord

Native Discord client for PS5, running as a payload via `ps5-payload-sdk` and elfldr.

Status: pre-alpha.


## Target

- Console firmware 13.00 (never install or trigger >= 14.00)
- Payload loaded through elfldr (TCP 9021), logs via klog

## Workflow

- `main` stays releasable; work on `feat/*`, `fix/*` branches, merged via PR.
- Tags `vX.Y.Z` + GitHub release only on meaningful milestones.

## Milestones

1. `v0.1.0`: connect to gateway with a pasted token, heartbeat, log READY to klog
2. `v0.2.0`: QR login on screen
3. `v0.3.0`: read and send text messages
4. `v0.4.0`: join voice channel (muted placeholder)
5. `v0.5.0`: voice audio (Opus + DAVE)

Use a throwaway account. Third-party clients break Discord ToS.
