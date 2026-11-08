// SPDX-License-Identifier: AGPL-3.0-or-later
// DualSense USB takeover probe for ps5-discord. Payload only, one shot, 30 s.
// Lists every /dev/ugenB.U device, then for a DualSense on USB detaches the
// system's driver from its HID interface only (the audio interfaces stay with the
// system), reads its input reports from the interrupt endpoint and logs buttons and
// sticks once a second. At the end it releases the device and re-enumerates it so
// the system takes it back. Approach from Ghostcontrol (gc_main.c, GPL-3.0-or-later).
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOG_PATH "/data/ps5-discord-probe/ds5usb.txt"
#define SONY 0x054c
#define DUALSENSE 0x0ce6
#define DUALSENSE_EDGE 0x0df2
#define HID_IFACE 3 // DualSense USB: interfaces 0-2 audio, 3 HID
#define EP_IN 0x84
#define RUN_MS 30000u

static FILE *g_log;

static void log_line(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[ds5-usb] %s\n", line);
    if (g_log)
    {
        fprintf(g_log, "%s\n", line);
        fflush(g_log);
    }
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

// Logs every USB device; returns the path of the first DualSense in found.
static int scan(char *found, size_t cap)
{
    int hit = 0;
    for (int bus = 0; bus < 8; bus++)
        for (int unit = 1; unit < 16; unit++)
        {
            char path[32];
            snprintf(path, sizeof path, "/dev/ugen%d.%d", bus, unit);
            int fd = open(path, O_RDWR | O_NONBLOCK);
            if (fd < 0)
                continue;
            struct usb_device_descriptor d;
            memset(&d, 0, sizeof d);
            int rc = ioctl(fd, USB_GET_DEVICE_DESC, &d);
            unsigned vid = UGETW(d.idVendor), pid = UGETW(d.idProduct);
            log_line("%s: rc %d vid %04x pid %04x class %02x", path, rc, vid, pid, d.bDeviceClass);
            if (rc == 0 && vid == SONY && (pid == DUALSENSE || pid == DUALSENSE_EDGE) && !hit)
            {
                snprintf(found, cap, "%s", path);
                hit = 1;
            }
            close(fd);
        }
    return hit;
}

// USB report 0x01: sticks at 1-4, triggers 5-6, buttons at 8-10.
static void describe(const uint8_t *r, char *out, size_t cap)
{
    static const char *hat[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW", "-"};
    snprintf(out, cap, "L %3u,%3u R %3u,%3u L2 %3u R2 %3u dpad %s %s%s%s%s b1 %02x b2 %02x", r[1], r[2], r[3], r[4],
             r[5], r[6], hat[(r[8] & 0x0f) > 8 ? 8 : r[8] & 0x0f], r[8] & 0x10 ? "square " : "",
             r[8] & 0x20 ? "cross " : "", r[8] & 0x40 ? "circle " : "", r[8] & 0x80 ? "triangle " : "", r[9], r[10]);
}

int main(void)
{
    mkdir("/data/ps5-discord-probe", 0755);
    g_log = fopen(LOG_PATH, "w");
    log_line("start");

    char path[32];
    if (!scan(path, sizeof path))
    {
        log_line("no DualSense on USB, stop");
        return 0;
    }
    log_line("DualSense at %s", path);

    // The system's HID driver re-attaches quickly; detach and open until we win.
    int fd = -1;
    struct usb_fs_endpoint eps[1];
    struct usb_fs_init init;
    struct usb_fs_open op;
    for (int attempt = 0; attempt < 5 && fd < 0; attempt++)
    {
        fd = open(path, O_RDWR);
        if (fd < 0)
        {
            log_line("open errno %d", errno);
            return 1;
        }
        int iface = HID_IFACE;
        int drc = ioctl(fd, USB_IFACE_DRIVER_DETACH, &iface);
        memset(eps, 0, sizeof eps);
        memset(&init, 0, sizeof init);
        init.pEndpoints = eps;
        init.ep_index_max = 1;
        int irc = ioctl(fd, USB_FS_INIT, &init);
        memset(&op, 0, sizeof op);
        op.ep_index = 0;
        op.ep_no = EP_IN;
        op.max_bufsize = 64;
        op.max_frames = 1;
        int orc = irc == 0 ? ioctl(fd, USB_FS_OPEN, &op) : -1;
        log_line("attempt %d: detach %d init %d open %d (errno %d) maxpkt %u", attempt, drc, irc, orc, errno,
                 (unsigned)op.max_packet_length);
        if (orc != 0)
        {
            if (irc == 0)
            {
                struct usb_fs_uninit u = {0};
                ioctl(fd, USB_FS_UNINIT, &u);
            }
            close(fd);
            fd = -1;
            usleep(50000);
        }
    }
    if (fd < 0)
    {
        log_line("could not take the HID endpoint, stop");
        return 1;
    }

    uint8_t buf[64];
    void *buffers[1] = {buf};
    uint32_t lengths[1];
    unsigned reports = 0, errors = 0;
    const uint64_t t0 = now_ms();
    uint64_t last = 0;
    while (now_ms() - t0 < RUN_MS)
    {
        lengths[0] = sizeof buf;
        eps[0].ppBuffer = buffers;
        eps[0].pLength = lengths;
        eps[0].nFrames = 1;
        eps[0].aFrames = 0;
        eps[0].status = 0;
        eps[0].timeout = 200;
        eps[0].flags = USB_FS_FLAG_SINGLE_SHORT_OK | USB_FS_FLAG_MULTI_SHORT_OK;
        struct usb_fs_start st = {.ep_index = 0};
        if (ioctl(fd, USB_FS_START, &st) != 0)
        {
            errors++;
            if (errno == ENXIO || errno == ENOTTY)
            {
                log_line("device gone (errno %d)", errno);
                break;
            }
            struct usb_fs_stop sp = {.ep_index = 0};
            ioctl(fd, USB_FS_STOP, &sp);
            usleep(5000);
            continue;
        }
        int ok = 0;
        for (;;)
        {
            struct usb_fs_complete c = {.ep_index = 0};
            if (ioctl(fd, USB_FS_COMPLETE, &c) == 0)
            {
                ok = 1;
                break;
            }
            if (errno != EBUSY)
                break;
            usleep(1000);
        }
        if (!ok || eps[0].status != 0 || lengths[0] < 11 || buf[0] != 0x01)
        {
            errors += !ok || eps[0].status != 0;
            continue;
        }
        reports++;
        if (now_ms() - last >= 1000u)
        {
            last = now_ms();
            char d[192];
            describe(buf, d, sizeof d);
            log_line("t=%2us reports %u errors %u | %s", (unsigned)((last - t0) / 1000u), reports, errors, d);
        }
    }

    struct usb_fs_stop sp = {.ep_index = 0};
    ioctl(fd, USB_FS_STOP, &sp);
    struct usb_fs_close cl = {.ep_index = 0};
    ioctl(fd, USB_FS_CLOSE, &cl);
    struct usb_fs_uninit u = {0};
    ioctl(fd, USB_FS_UNINIT, &u);
    // Releasing alone leaves the controller orphaned; a re-enumeration is a software
    // replug, so the system's driver attaches again.
    int zero = 0;
    int erc = ioctl(fd, USB_DEVICEENUMERATE, &zero);
    log_line("re-enumerate rc %d (errno %d)", erc, erc ? errno : 0);
    close(fd);
    log_line("done: %u reports, %u errors in %u s; device released", reports, errors, RUN_MS / 1000u);
    return 0;
}
