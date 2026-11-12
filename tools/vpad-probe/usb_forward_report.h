// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef VPAD_USB_FORWARD_REPORT_H
#define VPAD_USB_FORWARD_REPORT_H
#include "ds5_usb.h"
#include <stdatomic.h>
#include <string.h>

/* Kernel offsets/masks: FGG-XSense src/vpad.c:33-57,119-140.
 * PS mask: Ghostpad virtualDS5research.md's confirmed PS5 mapping (bit 16).
 * DS5_USB_* is a different button namespace: convert every supported bit. */
static inline void usb_forward_report(uint8_t report[160], const ds5_usb_controller *s)
{
    static const struct { uint32_t usb, kernel; } map[] = {
        {DS5_UP, 0x10}, {DS5_RIGHT, 0x20}, {DS5_DOWN, 0x40}, {DS5_LEFT, 0x80},
        {DS5_CROSS, 0x4000}, {DS5_CIRCLE, 0x2000},
        {DS5_SQUARE, 0x8000}, {DS5_TRIANGLE, 0x1000},
        {DS5_L1, 0x400}, {DS5_R1, 0x800}, {DS5_L2, 0x100}, {DS5_R2, 0x200},
        {DS5_L3, 2}, {DS5_R3, 4}, {DS5_OPTIONS, 8},
        {DS5_PS, 0x10000}, {DS5_TOUCHPAD, 0x100000}
    };
    uint32_t buttons = 0;
    for (size_t i = 0; i < sizeof map / sizeof map[0]; ++i)
        if (s->buttons & map[i].usb)
            buttons |= map[i].kernel;
    memset(report, 0, 160);
    memcpy(report + 12, &buttons, sizeof buttons);
    report[16] = (uint8_t)(s->left_x / 256 + 128);
    report[17] = (uint8_t)(s->left_y / 256 + 128);
    report[18] = (uint8_t)(s->right_x / 256 + 128);
    report[19] = (uint8_t)(s->right_y / 256 + 128);
    report[20] = s->left_trigger;
    report[21] = s->right_trigger;
    report[23] = 1;
    const float identity = 1.0f;
    memcpy(report + 0x84, &identity, sizeof identity);
}

/* One USB producer, one HID consumer. Release/acquire publishes whole states;
 * a full queue drops new reports instead of blocking the USB audio thread. */
#define USB_FORWARD_QUEUE_SIZE 64u
typedef struct {
    atomic_uint write, read, dropped;
    struct {
        ds5_usb_controller state;
        uint64_t received_ms;
    } reports[USB_FORWARD_QUEUE_SIZE];
} usb_forward_queue;

static inline int usb_forward_push(usb_forward_queue *q, const ds5_usb_controller *s,
                                   uint64_t received_ms)
{
    unsigned write = atomic_load_explicit(&q->write, memory_order_relaxed);
    unsigned next = (write + 1u) % USB_FORWARD_QUEUE_SIZE;
    if (next == atomic_load_explicit(&q->read, memory_order_acquire)) {
        atomic_fetch_add_explicit(&q->dropped, 1, memory_order_relaxed);
        return 0;
    }
    q->reports[write].state = *s;
    q->reports[write].received_ms = received_ms;
    atomic_store_explicit(&q->write, next, memory_order_release);
    return 1;
}

static inline int usb_forward_pop(usb_forward_queue *q, ds5_usb_controller *s,
                                  uint64_t *received_ms)
{
    unsigned read = atomic_load_explicit(&q->read, memory_order_relaxed);
    if (read == atomic_load_explicit(&q->write, memory_order_acquire))
        return 0;
    *s = q->reports[read].state;
    *received_ms = q->reports[read].received_ms;
    atomic_store_explicit(&q->read, (read + 1u) % USB_FORWARD_QUEUE_SIZE, memory_order_release);
    return 1;
}

static inline ds5_usb_controller usb_forward_fresh_state(const ds5_usb_controller *s,
                                                         uint64_t received_ms, uint64_t now)
{
    return now - received_ms > 250u ? (ds5_usb_controller){0} : *s;
}
#endif
