// SPDX-License-Identifier: AGPL-3.0-or-later
// Saved opt-in USB controller mic/speaker capture, scoped to active calls.
#pragma once
void cv_init(void);
// Legacy output-picker API maps to the saved controller_audio preference.
void cv_set_tv(int tv);
int cv_tv(void);
const char *cv_state(void);    // off, starting, on, missing, stopping, error
int cv_holds_controller(void); // Includes startup and physical handback.
void cv_pause(int on);         // Remote Play reserves USB; preference stays unchanged.
void cv_stop(void);            // Suppress automatic capture until current call ends.
