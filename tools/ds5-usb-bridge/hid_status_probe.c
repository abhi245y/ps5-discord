// SPDX-License-Identifier: AGPL-3.0-or-later
// Can a payload read the DualSense's status while the console keeps its HID
// interface? Asks for input report 0x01 over the control endpoint (HID GET_REPORT)
// every 100 ms for 30 s and logs the headphone bit (byte 54 bit 0), the mic-muted
// bit (byte 54 bit 2) and the mute button (byte 10 bit 2). Detaches nothing.
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG_PATH "/data/ps5-discord-probe/hidstatus.txt"

static FILE *g_log;

static void log_line(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[hid-status] %s\n", line);
    if (g_log) {
        fprintf(g_log, "%s\n", line);
        fflush(g_log);
    }
}

static int find_dualsense(char *path, size_t cap)
{
    for (int bus = 0; bus < 8; bus++)
        for (int unit = 1; unit < 16; unit++) {
            snprintf(path, cap, "/dev/ugen%d.%d", bus, unit);
            int fd = open(path, O_RDWR | O_NONBLOCK);
            if (fd < 0)
                continue;
            struct usb_device_descriptor d;
            memset(&d, 0, sizeof d);
            int hit = ioctl(fd, USB_GET_DEVICE_DESC, &d) == 0 && UGETW(d.idVendor) == 0x054c &&
                      (UGETW(d.idProduct) == 0x0ce6 || UGETW(d.idProduct) == 0x0df2);
            close(fd);
            if (hit)
                return 1;
        }
    return 0;
}

int main(void)
{
    mkdir("/data/ps5-discord-probe", 0755);
    g_log = fopen(LOG_PATH, "w");
    char path[32];
    if (!find_dualsense(path, sizeof path)) {
        log_line("no DualSense on USB");
        return 0;
    }
    int fd = open(path, O_RDWR);
    log_line("DualSense at %s (fd %d)", path, fd);
    int ok = 0, fail = 0, last = -1;
    for (int i = 0; i < 300 && fd >= 0; i++) {
        uint8_t r[64];
        memset(r, 0, sizeof r);
        struct usb_ctl_request req;
        memset(&req, 0, sizeof req);
        req.ucr_request.bmRequestType = 0xa1; // device to host, class, interface
        req.ucr_request.bRequest = 0x01;      // GET_REPORT
        USETW(req.ucr_request.wValue, 0x0101); // input report, id 1
        USETW(req.ucr_request.wIndex, 3);      // the HID interface
        USETW(req.ucr_request.wLength, sizeof r);
        req.ucr_data = r;
        req.ucr_flags = 0x0004; // USB_SHORT_XFER_OK (dev/usb/usbdi.h, kernel-only header)
        if (ioctl(fd, USB_DO_REQUEST, &req) != 0) {
            if (fail++ < 3)
                log_line("GET_REPORT failed (errno %d)", errno);
        } else {
            ok++;
            int state = (r[54] & 5) << 8 | (r[10] & 4);
            if (state != last || i % 50 == 0)
                log_line("t=%4dms len %d id %02x status %02x headphones %d mic-muted %d mute-button %d", i * 100,
                         req.ucr_actlen, r[0], r[54], r[54] & 1, !!(r[54] & 4), !!(r[10] & 4));
            last = state;
        }
        usleep(100000);
    }
    log_line("done: %d reports, %d failures", ok, fail);
    if (fd >= 0)
        close(fd);
    return 0;
}
