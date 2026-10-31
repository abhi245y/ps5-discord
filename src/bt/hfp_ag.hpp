// SPDX-License-Identifier: AGPL-3.0-or-later
// Hands-free (HFP 1.8) with the console as the audio gateway, for headsets without the
// AirPods mic stream, and as the AirPods' fallback. RFCOMM to the headset's hands-free
// channel (or the headset's to our channel 1, sdp_server.cpp), the AT handshake (the
// service level connection), then a voice link: mSBC (16 kHz) when the headset takes it,
// else CVSD with 8-bit samples (8 kHz). The adapter's isochronous endpoints run at
// alternate setting 1 (ugen_device.cpp), which carries both in 24-byte packets.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/bluetooth/bt_stack.hpp"
#include "resample.h"
#include "sbc.h"

class HfpAg
{
  public:
    explicit HfpAg(bluev::BtStack& bt) : m_bt(bt)
    {
    }
    void listen(); // accept RFCOMM (psm 3) from a headset that opens it itself
    void reset();
    // The service level connection: waits for the headset's, else opens it. True when up.
    bool connect();
    bool slcUp() const
    {
        return m_slc;
    }
    // The voice link: 2 mSBC, 1 CVSD, 0 none.
    int openAudio(bool allowWide);
    void closeAudio();

  private:
    void onFrame(const std::uint8_t* d, int n);
    void onMcc(const std::uint8_t* d, int n);
    void onAt(const std::string& line);
    void sendFrame(std::uint8_t dlci, std::uint8_t control, bool cr, const std::uint8_t* data, int length,
                   int credits = -1);
    void sendMcc(std::uint8_t type, bool command, const std::uint8_t* data, int length);
    void sendText(const std::string& text);
    void reply(const std::string& text); // "\r\n<text>\r\n"
    void ok();
    int findHandsFreeChannel(); // SDP: the headset's RFCOMM channel for hands-free, 0 when none
    bool connectOut();          // open RFCOMM to the headset as the initiator

    bluev::BtStack& m_bt;
    bluev::L2Channel m_ch{"rfcomm", 0x46};
    std::uint8_t m_dlci = 0;
    bool m_dlciOpen = false;
    bool m_creditFlow = false;
    int m_theirCredits = 0;
    int m_received = 0; // their frames since we last granted credits
    std::string m_line;
    bool m_slc = false;
    bool m_initiator = false; // we sent the first SABM
    bool m_ua[64] = {};       // UA seen per DLCI
    bool m_pnDone = false;
    bluev::L2Channel m_sdpOut{"sdp-out", 0x48};
    std::vector<std::uint8_t> m_sdpReply;
    std::uint32_t m_hfFeatures = 0;
    std::string m_codecs; // what AT+BAC listed
    int m_codec = 0;      // what AT+BCS confirmed
};

// The voice link's audio while it is open. Packets are 24 bytes each way (alternate
// setting 1): mSBC frames (H2 header 01 x8, 57 bytes from ad, a pad byte; 120 samples at
// 16 kHz) spread over them as Linux does, or 24 CVSD samples of 8 bits.
class HfpVoice
{
  public:
    HfpVoice(bool wide, std::uint16_t handle);
    ~HfpVoice();
    // What came in on the isochronous endpoint; mic gets 48 kHz mono samples.
    void receive(const std::uint8_t* bytes, int n, const std::function<void(const std::int16_t*, int)>& mic);
    // 48 kHz mono samples for the headset (the game and the call, mixed).
    void play(const float* mono, int n);
    // Queues packets on the endpoint, about 30 ms ahead of the clock.
    void pump();
    long frames() const
    {
        return m_frames;
    }

  private:
    void packet(std::uint8_t* out); // the next 24 bytes to send

    bool m_wide;
    std::uint16_t m_handle;
    sbc_t m_enc{}, m_dec{};
    rs_state m_up{}, m_down{};
    std::vector<std::uint8_t> m_in, m_voice, m_out;
    std::vector<float> m_play; // at the link's rate
    unsigned m_seq = 0;
    long m_frames = 0;
    std::int64_t m_nextUs = 0;
};
