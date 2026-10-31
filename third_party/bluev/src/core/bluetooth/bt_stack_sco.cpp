// SPDX-License-Identifier: GPL-3.0-or-later
// HFP probe: the voice (SCO/eSCO) link to the headset.
#include "core/bluetooth/bt_stack.hpp"
#include "core/bytes.hpp"
#include "core/ports/log.hpp"

namespace bluev
{

int BtStack::setupSco(std::uint16_t packetTypes, std::uint16_t maxLatency, std::uint8_t retransmission,
                      std::uint16_t voiceSetting)
{
    std::uint8_t p[17];

    putLe16(p, m_handle);
    putLe16(p + 2, 8000); // transmit and receive bandwidth: 8000 bytes/s
    putLe16(p + 4, 0);
    putLe16(p + 6, 8000);
    putLe16(p + 8, 0);
    putLe16(p + 10, maxLatency);
    putLe16(p + 12, voiceSetting);
    p[14] = retransmission;
    putLe16(p + 15, packetTypes);
    m_scoDone = false;
    m_scoStatus = -1;
    logLine("sco: setup on handle %#x: packet types %#06x, latency %#x, retransmission %u, voice %#06x", m_handle,
            packetTypes, maxLatency, retransmission, voiceSetting);
    if (!cmd(0x0428, p, sizeof p))
    {
        return -1;
    }
    wait([this] { return m_scoDone; }, 8000);
    return m_scoDone ? m_scoStatus : -1;
}

void BtStack::onSyncConnectionComplete(const std::uint8_t* e)
{
    m_scoDone = true;
    m_scoStatus = e[0];
    m_scoAirMode = e[16];
    m_scoRxLength = getLe16(e + 12);
    m_scoTxLength = getLe16(e + 14);
    if (e[0] == 0)
    {
        m_scoHandle = getLe16(e + 1);
    }
    logLine("sco: connection complete: status %#x, handle %#x, link type %u (%s), tx interval %u, window %u, "
            "rx %u tx %u bytes, air mode %u",
            e[0], getLe16(e + 1), e[9], e[9] == 2 ? "eSCO" : "SCO", e[10], e[11], m_scoRxLength, m_scoTxLength,
            e[16]);
}

void BtStack::dropSco()
{
    if (m_scoHandle == 0)
    {
        return;
    }
    sendDisconnect(m_scoHandle);
    m_scoHandle = 0;
    poll(300);
}

} // namespace bluev
