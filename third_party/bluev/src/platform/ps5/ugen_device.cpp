// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/ugen_device.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "core/hfp/iso.hpp"
#include "core/ports/log.hpp"

namespace bluev
{

IsoPort g_iso;

UgenDevice::UgenDevice()
{
    std::memset(m_eps, 0, sizeof m_eps);
    std::memset(m_bufPtr, 0, sizeof m_bufPtr);
    std::memset(m_bufLen, 0, sizeof m_bufLen);
    g_iso.start = [this] { return isoStart(); };
    g_iso.stop = [this] { isoStop(); };
    g_iso.read = [this](std::uint8_t* out, int max) {
        const int n = static_cast<int>(m_isoIn.size()) < max ? static_cast<int>(m_isoIn.size()) : max;
        std::memcpy(out, m_isoIn.data(), static_cast<std::size_t>(n));
        m_isoIn.erase(m_isoIn.begin(), m_isoIn.begin() + n);
        return n;
    };
    g_iso.write = [this](const std::uint8_t* data, int length) {
        if (m_isoOut.size() > 16384)
        {
            m_isoDropped += length;
            return false;
        }
        m_isoOut.insert(m_isoOut.end(), data, data + length);
        for (int s = kIsoSlots / 2; s < kIsoSlots; s++)
        {
            if (!m_isoBusy[s] && m_isoRunning)
            {
                isoArmOut(s);
            }
        }
        return true;
    };
    g_iso.describe = [this] { return isoDescribe(); };
}

UgenDevice::~UgenDevice()
{
    close();
}

bool UgenDevice::open(const char* node, int transfers, int forceShort)
{
    usb_fs_init init;
    char path[48];

    std::snprintf(path, sizeof path, "/dev/%s", node);
    m_fd = ::open(path, O_RDWR);
    if (m_fd < 0)
    {
        m_error = errno;
        logLine("hci: open %s errno=%d", path, m_error);
        return false;
    }

    // HFP probe: the voice endpoints of an adapter need an alternate setting of
    // interface 1, chosen before the transfer table exists (changing it later
    // takes the table down). /data/bluev/hfp-alt overrides the default 1
    // (transparent mSBC, one link: 63-byte packets in seven 9-byte frames, as
    // Linux picks it; 2 is CVSD with 16-bit samples).
    m_isoAlt = -1;
    if (std::strcmp(node, "ugen0.2") != 0)
    {
        int alt = 1;
        if (FILE* f = std::fopen("/data/bluev/hfp-alt", "r"))
        {
            if (std::fscanf(f, "%d", &alt) != 1)
            {
                alt = 1;
            }
            std::fclose(f);
        }
        usb_alt_interface ai;
        std::memset(&ai, 0, sizeof ai);
        ai.uai_interface_index = 1;
        ai.uai_alt_index = static_cast<std::uint8_t>(alt);
        if (ioctl(m_fd, USB_SET_ALTINTERFACE, &ai) == 0)
        {
            m_isoAlt = alt;
            logLine("hfp: interface 1 alternate setting %d", alt);
        }
        else
        {
            logLine("hfp: interface 1 alternate setting %d failed, errno=%d", alt, errno);
        }
    }

    std::memset(m_eps, 0, sizeof m_eps);
    for (int i = 0; i < kUsbTransfers; i++)
    {
        m_bufPtr[i] = m_buf[i];
        m_eps[i].ppBuffer = reinterpret_cast<void**>(&m_bufPtr[i]);
        m_eps[i].pLength = &m_bufLen[i];
        m_bufLen[i] = kUsbTransferBytes;
        m_eps[i].nFrames = 1;
        m_eps[i].flags = USB_FS_FLAG_SINGLE_SHORT_OK;
    }
    m_eps[forceShort].flags = USB_FS_FLAG_FORCE_SHORT;

    std::memset(&init, 0, sizeof init);
    init.pEndpoints = m_eps;
    m_transfers = transfers;
    init.ep_index_max = static_cast<std::uint8_t>(transfers + (m_isoAlt >= 0 ? kIsoSlots : 0));
    if (ioctl(m_fd, USB_FS_INIT, &init) != 0)
    {
        m_error = errno;
        logLine("hci: USB_FS_INIT errno=%d", m_error);
        close();
        return false;
    }
    if (m_isoAlt >= 0)
    {
        isoOpen();
    }
    return true;
}

void UgenDevice::isoOpen()
{
    for (int s = 0; s < kIsoSlots; s++)
    {
        const bool in = s < kIsoSlots / 2;
        usb_fs_open op;

        std::memset(&op, 0, sizeof op);
        op.max_bufsize = kIsoFrames * kIsoFrameMax;
        op.max_frames = kIsoFrames;
        op.ep_index = static_cast<std::uint8_t>(m_transfers + s);
        op.ep_no = in ? 0x83 : 0x03;
        if (ioctl(m_fd, USB_FS_OPEN, &op) != 0)
        {
            logLine("hfp: iso USB_FS_OPEN %#x (slot %d) errno=%d", op.ep_no, s, errno);
            return;
        }
        m_isoMps[in ? 0 : 1] = op.max_packet_length;
        if (s == 0 || s == kIsoSlots / 2)
        {
            logLine("hfp: iso endpoint %#x: packet %u, frames %u, buffer %u", op.ep_no, op.max_packet_length,
                    static_cast<unsigned>(op.max_frames), op.max_bufsize);
        }
    }
    m_isoOpened = m_isoMps[0] > 0 && m_isoMps[0] <= kIsoFrameMax && m_isoMps[1] <= kIsoFrameMax;
}

bool UgenDevice::isoArm(int slot)
{
    const int index = m_transfers + slot;

    for (int f = 0; f < kIsoFrames; f++)
    {
        m_isoPtr[slot][f] = m_isoBuf[slot][f];
        m_isoLen[slot][f] = m_isoMps[0];
    }
    m_eps[index].ppBuffer = m_isoPtr[slot];
    m_eps[index].pLength = m_isoLen[slot];
    m_eps[index].nFrames = kIsoFrames;
    m_eps[index].aFrames = 0;
    m_eps[index].status = 0;
    m_eps[index].timeout = 0;
    m_eps[index].flags = USB_FS_FLAG_SINGLE_SHORT_OK | USB_FS_FLAG_MULTI_SHORT_OK;
    std::uint8_t id = static_cast<std::uint8_t>(index);
    if (ioctl(m_fd, USB_FS_START, &id) != 0)
    {
        m_isoErrors++;
        return false;
    }
    m_isoBusy[slot] = true;
    return true;
}

// Fills the frames of an out slot from the queue, one packet's worth each.
bool UgenDevice::isoArmOut(int slot)
{
    const int index = m_transfers + slot;
    const std::uint32_t mps = m_isoMps[1];
    int frames = 0;

    if (mps == 0)
    {
        return false;
    }
    while (frames < kIsoFrames && !m_isoOut.empty())
    {
        const std::uint32_t n = m_isoOut.size() < mps ? static_cast<std::uint32_t>(m_isoOut.size()) : mps;
        std::memcpy(m_isoBuf[slot][frames], m_isoOut.data(), n);
        m_isoOut.erase(m_isoOut.begin(), m_isoOut.begin() + n);
        m_isoPtr[slot][frames] = m_isoBuf[slot][frames];
        m_isoLen[slot][frames] = n;
        m_isoOutBytes += n;
        frames++;
    }
    if (frames == 0)
    {
        return false;
    }
    m_eps[index].ppBuffer = m_isoPtr[slot];
    m_eps[index].pLength = m_isoLen[slot];
    m_eps[index].nFrames = static_cast<std::uint32_t>(frames);
    m_eps[index].aFrames = 0;
    m_eps[index].status = 0;
    m_eps[index].timeout = 0;
    m_eps[index].flags = 0;
    std::uint8_t id = static_cast<std::uint8_t>(index);
    if (ioctl(m_fd, USB_FS_START, &id) != 0)
    {
        m_isoErrors++;
        return false;
    }
    m_isoBusy[slot] = true;
    return true;
}

bool UgenDevice::isoStart()
{
    if (!m_isoOpened)
    {
        return false;
    }
    m_isoRunning = true;
    m_isoIn.clear();
    for (int s = 0; s < kIsoSlots / 2; s++)
    {
        if (!m_isoBusy[s])
        {
            isoArm(s);
        }
    }
    return true;
}

void UgenDevice::isoStop()
{
    m_isoRunning = false;
    for (int s = 0; s < kIsoSlots; s++)
    {
        if (m_isoBusy[s])
        {
            std::uint8_t id = static_cast<std::uint8_t>(m_transfers + s);
            ioctl(m_fd, USB_FS_STOP, &id);
            m_isoBusy[s] = false;
        }
    }
    m_isoOut.clear();
}

void UgenDevice::isoDone(int slot)
{
    const int index = m_transfers + slot;

    m_isoBusy[slot] = false;
    if (m_eps[index].status != 0)
    {
        m_isoErrors++;
    }
    else if (slot < kIsoSlots / 2)
    {
        m_isoInTransfers++;
        for (std::uint32_t f = 0; f < m_eps[index].aFrames && f < kIsoFrames; f++)
        {
            const std::uint32_t n = m_isoLen[slot][f];
            if (n > 0 && n <= kIsoFrameMax)
            {
                if (m_isoIn.size() < 65536)
                {
                    m_isoIn.insert(m_isoIn.end(), m_isoBuf[slot][f], m_isoBuf[slot][f] + n);
                }
                m_isoInBytes += n;
            }
        }
    }
    if (!m_isoRunning)
    {
        return;
    }
    if (slot < kIsoSlots / 2)
    {
        isoArm(slot);
    }
    else
    {
        isoArmOut(slot);
    }
}

std::string UgenDevice::isoDescribe()
{
    char text[256];
    std::snprintf(text, sizeof text,
                  "iso: alt %d, packet in %u out %u, in transfers %ld bytes %ld, out bytes %ld, errors %ld, "
                  "dropped %ld",
                  m_isoAlt, m_isoMps[0], m_isoMps[1], m_isoInTransfers, m_isoInBytes, m_isoOutBytes, m_isoErrors,
                  m_isoDropped);
    return text;
}

void UgenDevice::close()
{
    if (m_fd < 0)
    {
        return;
    }
    // One call takes the whole transfer table down (its argument is a dummy byte),
    // as the console's own libusb does when it closes a device.
    std::uint8_t dummy = 0;

    isoStop();
    m_isoOpened = false;
    ioctl(m_fd, USB_FS_UNINIT, &dummy);
    ::close(m_fd);
    m_fd = -1;
}

bool UgenDevice::openTransfer(int index, std::uint8_t endpoint, std::uint32_t length)
{
    usb_fs_open op;

    std::memset(&op, 0, sizeof op);
    op.max_bufsize = length;
    op.max_frames = 1;
    op.ep_index = static_cast<std::uint8_t>(index);
    op.ep_no = endpoint;
    if (ioctl(m_fd, USB_FS_OPEN, &op) != 0)
    {
        m_error = errno;
        return false;
    }
    return true;
}

std::uint8_t* UgenDevice::buffer(int index)
{
    return m_buf[index];
}

bool UgenDevice::start(int index, std::uint32_t length)
{
    std::uint8_t id = static_cast<std::uint8_t>(index);

    m_bufLen[index] = length;
    m_eps[index].nFrames = 1;
    m_eps[index].aFrames = 0;
    m_eps[index].status = 0;
    if (ioctl(m_fd, USB_FS_START, &id) != 0)
    {
        m_error = errno;
        return false;
    }
    return true;
}

bool UgenDevice::stop(int index)
{
    std::uint8_t id = static_cast<std::uint8_t>(index);

    return ioctl(m_fd, USB_FS_STOP, &id) == 0;
}

int UgenDevice::complete()
{
    for (;;)
    {
        std::uint8_t id = 0;

        if (ioctl(m_fd, USB_FS_COMPLETE, &id) != 0)
        {
            return -1;
        }
        if (id < m_transfers || m_transfers == 0)
        {
            return id;
        }
        if (id - m_transfers < kIsoSlots)
        {
            isoDone(id - m_transfers);
        }
    }
}

UsbTransferResult UgenDevice::result(int index)
{
    UsbTransferResult result;

    result.status = static_cast<int>(m_eps[index].status);
    result.frames = m_eps[index].aFrames;
    result.length = m_bufLen[index];
    return result;
}

bool UgenDevice::control(std::uint8_t interface, const std::uint8_t* data, std::uint16_t length)
{
    usb_ctl_request setup;

    std::memset(&setup, 0, sizeof setup);
    setup.ucr_request.bmRequestType = UT_WRITE_CLASS_DEVICE;
    setup.ucr_request.bRequest = 0x00;
    USETW(setup.ucr_request.wIndex, interface);
    USETW(setup.ucr_request.wLength, length);
    setup.ucr_data = const_cast<std::uint8_t*>(data);
    if (ioctl(m_fd, USB_DO_REQUEST, &setup) != 0)
    {
        m_error = errno;
        return false;
    }
    return true;
}

void UgenDevice::wait(std::uint32_t microseconds)
{
    usleep(microseconds);
}

} // namespace bluev
