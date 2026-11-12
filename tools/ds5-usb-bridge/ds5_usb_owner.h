// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#define DS5_USB_OWNER_DIR "/data/ps5-discord"
#define DS5_USB_LOCK "ds5-usb.lock"
#define DS5_USB_TAKEN "ds5-usb.taken"
#define DS5_USB_HANDBACK "ds5-usb.handback"

typedef struct {
    int fd;
    char marker[512];
} ds5_usb_owner;
#define DS5_USB_OWNER_INIT {-1, ""}

// Nonblocking. 0 acquired (including stale-owner recovery), 1 busy, -1 error.
// Callers bound retries and check their own cancellation flag between attempts.
int ds5_usb_owner_try(ds5_usb_owner *owner, const char *directory, const char *name,
                      void (*log)(void *, const char *), void *user);
// Only verified handback removes the marker. Failed recovery leaves a retryable marker.
int ds5_usb_owner_release(ds5_usb_owner *owner, int verified);
int ds5_usb_owner_busy(const char *directory);
