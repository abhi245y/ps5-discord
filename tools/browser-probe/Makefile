# Prefer arlp-pc; local build explicitly authorized for this probe.
ifndef PS5_PAYLOAD_SDK
$(error Set PS5_PAYLOAD_SDK to an existing payload SDK)
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

.PHONY: all
all:
	$(error All console probes quarantined after two console crashes; offline diagnosis required)

discord-browser-probe.elf: main.c
	$(error Original Discord probe is quarantined after a console crash)

discord-browser-lifecycle.elf: lifecycle.c
	$(error Lifecycle-only probe also crashed the console; do not redeploy)

.PHONY: local
local: lifecycle.c
	$(error Local-page test blocked by failed lifecycle baseline; do not deploy)

# Explicitly requested experiment: no parent-side watchdog kill or waitpid.
.PHONY: passive-local
passive-local: lifecycle.c
	$(error Passive watchdog variant also crashed after rendering; do not redeploy)

# User explicitly requested removing the watchdog for this one-shot launcher.
.PHONY: no-watchdog
no-watchdog: launch.c
	@test -n "$(PROBE_URL)" || { echo 'Set PROBE_URL to the diagnostic page'; exit 2; }
	$(CC) -std=gnu11 -Wall -Wextra -Werror -O2 '-DPROBE_URL="$(PROBE_URL)"' -o discord-browser-no-watchdog.elf $< -lSceSystemService -lSceUserService
