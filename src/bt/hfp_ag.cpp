// SPDX-License-Identifier: AGPL-3.0-or-later
#include "hfp_ag.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/bytes.hpp"
#include "core/hfp/iso.hpp"
#include "core/ports/log.hpp"

using namespace bluev;

namespace
{

constexpr std::uint16_t kRfcommPsm = 0x0003;
constexpr std::uint8_t kSabm = 0x2f, kUa = 0x63, kDm = 0x0f, kDisc = 0x43, kUih = 0xef, kPf = 0x10;
constexpr std::uint8_t kPn = 0x20, kMsc = 0x38, kRpn = 0x24, kRls = 0x14, kTest = 0x08, kNsc = 0x04;

// TS 07.10 frame check: CRC-8, reversed polynomial 0xe0
std::uint8_t fcs(const std::uint8_t* p, int n)
{
    static std::uint8_t table[256];
    static bool ready = false;

    if (!ready)
    {
        for (int i = 0; i < 256; i++)
        {
            std::uint8_t c = static_cast<std::uint8_t>(i);
            for (int k = 0; k < 8; k++)
            {
                c = (c & 1) ? static_cast<std::uint8_t>((c >> 1) ^ 0xe0) : static_cast<std::uint8_t>(c >> 1);
            }
            table[i] = c;
        }
        ready = true;
    }
    std::uint8_t f = 0xff;
    for (int i = 0; i < n; i++)
    {
        f = table[f ^ p[i]];
    }
    return static_cast<std::uint8_t>(0xff - f);
}

std::string printable(const std::string& s)
{
    std::string out;
    for (char c : s)
    {
        out += c == '\r' ? std::string("\\r") : c == '\n' ? std::string("\\n") : std::string(1, c);
    }
    return out;
}

} // namespace

void HfpAg::reset()
{
    m_ch.reset();
    m_dlci = 0;
    m_dlciOpen = false;
    m_creditFlow = false;
    m_theirCredits = 0;
    m_received = 0;
    m_line.clear();
    m_slc = false;
    m_hfFeatures = 0;
    m_initiator = false;
    std::memset(m_ua, 0, sizeof m_ua);
    m_pnDone = false;
    m_sdpOut.reset();
    m_sdpReply.clear();
}

void HfpAg::listen()
{
    m_bt.bindInbound(kRfcommPsm, m_ch, [this](std::uint32_t, const std::uint8_t* d, int n) { onFrame(d, n); });
}

// C/R bits (TS 07.10): commands and UIH frames carry 1 from the initiator (who
// sent the first SABM), responses carry the opposite.
void HfpAg::sendFrame(std::uint8_t dlci, std::uint8_t control, bool cr, const std::uint8_t* data, int length,
                         int credits)
{
    std::uint8_t b[700];
    int k = 0;

    b[k++] = static_cast<std::uint8_t>((dlci << 2) | (cr ? 2 : 0) | 1);
    b[k++] = control;
    if (length < 128)
    {
        b[k++] = static_cast<std::uint8_t>((length << 1) | 1);
    }
    else
    {
        b[k++] = static_cast<std::uint8_t>((length & 0x7f) << 1);
        b[k++] = static_cast<std::uint8_t>(length >> 7);
    }
    if (credits >= 0)
    {
        b[k++] = static_cast<std::uint8_t>(credits);
    }
    if (length > 0)
    {
        std::memcpy(b + k, data, static_cast<std::size_t>(length));
        k += length;
    }
    // UIH: address and control only; the others include the length
    b[k] = fcs(b, (control & ~kPf) == kUih ? 2 : 3);
    k++;
    m_bt.send(m_ch.theirCid, b, k);
}

void HfpAg::sendMcc(std::uint8_t type, bool command, const std::uint8_t* data, int length)
{
    std::uint8_t m[64];

    m[0] = static_cast<std::uint8_t>((type << 2) | (command ? 2 : 0) | 1);
    m[1] = static_cast<std::uint8_t>((length << 1) | 1);
    std::memcpy(m + 2, data, static_cast<std::size_t>(length));
    sendFrame(0, kUih, m_initiator, m, length + 2);
}

void HfpAg::sendText(const std::string& text)
{
    if (m_creditFlow && m_theirCredits <= 0)
    {
        m_bt.wait([this] { return m_theirCredits > 0; }, 2000);
    }
    if (m_creditFlow && m_received > 0)
    {
        sendFrame(m_dlci, kUih | kPf, m_initiator, reinterpret_cast<const std::uint8_t*>(text.data()),
                  static_cast<int>(text.size()), m_received);
        m_received = 0;
    }
    else
    {
        sendFrame(m_dlci, kUih, m_initiator, reinterpret_cast<const std::uint8_t*>(text.data()),
                  static_cast<int>(text.size()));
    }
    m_theirCredits--;
    logLine("hfp: > %s", printable(text).c_str());
}

void HfpAg::reply(const std::string& text)
{
    sendText("\r\n" + text + "\r\n");
}

void HfpAg::ok()
{
    reply("OK");
}

void HfpAg::onFrame(const std::uint8_t* d, int n)
{
    if (n < 4)
    {
        return;
    }
    const std::uint8_t dlci = d[0] >> 2;
    const std::uint8_t control = d[1] & static_cast<std::uint8_t>(~kPf);
    const bool pf = (d[1] & kPf) != 0;
    int length, header;

    if (d[2] & 1)
    {
        length = d[2] >> 1;
        header = 3;
    }
    else
    {
        length = (d[2] >> 1) | (d[3] << 7);
        header = 4;
    }

    switch (control)
    {
    case kSabm:
        logLine("rfcomm: SABM dlci %u", dlci);
        sendFrame(dlci, kUa | kPf, !m_initiator, nullptr, 0);
        if (dlci != 0)
        {
            m_dlci = dlci;
            m_dlciOpen = true;
            const std::uint8_t msc[2] = {static_cast<std::uint8_t>((dlci << 2) | 2 | 1), 0x8d};
            sendMcc(kMsc, true, msc, 2);
        }
        break;
    case kDisc:
        logLine("rfcomm: DISC dlci %u", dlci);
        sendFrame(dlci, kUa | kPf, !m_initiator, nullptr, 0);
        if (dlci == 0 || dlci == m_dlci)
        {
            m_dlciOpen = false;
        }
        break;
    case kUa:
    case kDm:
        logLine("rfcomm: %s dlci %u", control == kUa ? "UA" : "DM", dlci);
        if (control == kUa && dlci < 64)
        {
            m_ua[dlci] = true;
        }
        break;
    case kUih: {
        const std::uint8_t* p = d + header;
        if (dlci == 0)
        {
            onMcc(p, length);
            break;
        }
        if (pf && m_creditFlow)
        {
            m_theirCredits += p[0];
            p++;
        }
        if (length > 0)
        {
            m_received++;
            for (int i = 0; i < length; i++)
            {
                const char c = static_cast<char>(p[i]);
                if (c == '\r')
                {
                    if (!m_line.empty())
                    {
                        onAt(m_line);
                    }
                    m_line.clear();
                }
                else if (c != '\n')
                {
                    m_line += c;
                }
            }
        }
        // keep the headset able to talk: hand back credits before it runs dry
        if (m_creditFlow && m_received >= 4)
        {
            sendFrame(m_dlci, kUih | kPf, m_initiator, nullptr, 0, m_received);
            m_received = 0;
        }
        break;
    }
    default:
        logLine("rfcomm: frame %#x dlci %u", d[1], dlci);
    }
}

void HfpAg::onMcc(const std::uint8_t* p, int n)
{
    if (n < 2)
    {
        return;
    }
    const bool command = (p[0] & 2) != 0;
    const std::uint8_t type = p[0] >> 2;
    const int length = p[1] >> 1;
    const std::uint8_t* v = p + 2;

    logLine("rfcomm: mcc type %#x %s, %d bytes", type, command ? "command" : "response", length);
    if (!command)
    {
        if (type == kPn && length >= 8)
        {
            m_creditFlow = (v[1] & 0xf0) == 0xe0;
            m_theirCredits = v[7];
            m_pnDone = true;
            logLine("rfcomm: PN answer dlci %u, flow %#x, mtu %u, credits %u", v[0], v[1], getLe16(v + 4), v[7]);
        }
        return;
    }
    switch (type)
    {
    case kPn: {
        std::uint8_t r[8];
        std::memcpy(r, v, 8);
        if ((v[1] & 0xf0) == 0xf0)
        {
            m_creditFlow = true;
            r[1] = 0xe0;
            m_theirCredits = v[7];
        }
        else
        {
            r[1] = 0;
        }
        r[3] = 0;
        const int theirMtu = getLe16(v + 4);
        const int ours = m_bt.maxFrame() - 6 < 330 ? m_bt.maxFrame() - 6 : 330;
        putLe16(r + 4, static_cast<unsigned>(theirMtu < ours ? theirMtu : ours));
        r[6] = 0;
        r[7] = 7; // our credits for them
        logLine("rfcomm: PN dlci %u, flow %#x, mtu %d (we use %u), their credits %u", v[0], v[1], theirMtu,
                getLe16(r + 4), v[7]);
        sendMcc(kPn, false, r, 8);
        break;
    }
    case kMsc:
    case kRls:
    case kTest:
        sendMcc(type, false, v, length);
        break;
    case kRpn: {
        if (length == 1)
        {
            const std::uint8_t r[8] = {v[0], 0x03, 0x03, 0x00, 0x11, 0x13, 0xff, 0x3f}; // 9600 8N1
            sendMcc(kRpn, false, r, 8);
        }
        else
        {
            sendMcc(kRpn, false, v, length);
        }
        break;
    }
    default: {
        const std::uint8_t r[1] = {p[0]};
        sendMcc(kNsc, false, r, 1);
    }
    }
}

void HfpAg::onAt(const std::string& line)
{
    logLine("hfp: < %s", line.c_str());
    if (line.rfind("AT+BRSF=", 0) == 0)
    {
        m_hfFeatures = static_cast<std::uint32_t>(std::strtoul(line.c_str() + 8, nullptr, 10));
        reply("+BRSF: 512"); // codec negotiation: CVSD and mSBC
        ok();
    }
    else if (line.rfind("AT+BAC=", 0) == 0)
    {
        m_codecs = line.substr(7);
        ok();
    }
    else if (line.rfind("AT+BCS=", 0) == 0)
    {
        m_codec = std::atoi(line.c_str() + 7);
        ok();
    }
    else if (line == "AT+CIND=?")
    {
        reply("+CIND: (\"service\",(0,1)),(\"call\",(0,1)),(\"callsetup\",(0-3)),(\"callheld\",(0-2)),"
              "(\"signal\",(0-5)),(\"roam\",(0,1)),(\"battchg\",(0-5))");
        ok();
    }
    else if (line == "AT+CIND?")
    {
        reply("+CIND: 1,0,0,0,5,0,5");
        ok();
    }
    else if (line.rfind("AT+CMER=", 0) == 0)
    {
        ok();
        m_slc = true; // no three-way calling on our side: the SLC is up after CMER
    }
    else if (line == "AT+CHLD=?")
    {
        reply("+CHLD: (0,1,2,3)");
        ok();
    }
    else if (line == "AT+BIND=?")
    {
        reply("+BIND: (1,2)");
        ok();
    }
    else if (line == "AT+BIND?")
    {
        reply("+BIND: 1,1");
        reply("+BIND: 2,1");
        ok();
    }
    else if (line.rfind("AT+XAPL=", 0) == 0)
    {
        reply("+XAPL=iPhone,2");
        ok();
    }
    else if (line == "AT+COPS?")
    {
        reply("+COPS: 0,0,\"PS5\"");
        ok();
    }
    else
    {
        ok(); // everything else (BIA, NREC, VGS, VGM, CLIP, CCWA, CMEE, XAPL extras...): accepted
    }
}

// Asks the headset's SDP server for the hands-free unit (0x111e) and reads
// the RFCOMM channel out of its protocol list (... 19 00 03 08 <channel>).
int HfpAg::findHandsFreeChannel()
{
    m_sdpOut.reset();
    m_sdpReply.clear();
    if (!m_bt.openChannel(m_sdpOut, 0x0001, [this](std::uint32_t, const std::uint8_t* d, int n) {
            m_sdpReply.assign(d, d + n);
        }))
    {
        logLine("hfp: sdp channel to the headset failed");
        return 0;
    }
    // Service Search Attribute: pattern {0x111e}, up to 1024 bytes, attribute 0x0004
    const std::uint8_t req[] = {0x06, 0x00, 0x01, 0x00, 0x0f, 0x35, 0x03, 0x19, 0x11, 0x1e, 0x04,
                                0x00, 0x35, 0x05, 0x0a, 0x00, 0x04, 0x00, 0x04, 0x00};
    m_bt.send(m_sdpOut.theirCid, req, sizeof req);
    m_bt.wait([this] { return !m_sdpReply.empty(); }, 5000);
    int channel = 0;
    for (std::size_t i = 0; i + 4 < m_sdpReply.size(); i++)
    {
        if (m_sdpReply[i] == 0x19 && m_sdpReply[i + 1] == 0x00 && m_sdpReply[i + 2] == 0x03 &&
            m_sdpReply[i + 3] == 0x08)
        {
            channel = m_sdpReply[i + 4];
        }
    }
    logLine("hfp: the headset's SDP answer: %zu bytes, hands-free on RFCOMM channel %d", m_sdpReply.size(), channel);
    m_bt.closeChannel(m_sdpOut);
    return channel;
}

// As the phone does: RFCOMM to the headset's hands-free channel. The headset
// then starts the AT handshake as usual.
bool HfpAg::connectOut()
{
    const int channel = findHandsFreeChannel();
    if (channel == 0)
    {
        return false;
    }
    m_ch.reset();
    if (!m_bt.openChannel(m_ch, kRfcommPsm, [this](std::uint32_t, const std::uint8_t* d, int n) { onFrame(d, n); }))
    {
        logLine("hfp: rfcomm channel to the headset failed");
        return false;
    }
    m_initiator = true;
    sendFrame(0, kSabm | kPf, true, nullptr, 0);
    if (!m_bt.wait([this] { return m_ua[0]; }, 5000))
    {
        logLine("hfp: no UA for dlci 0");
        return false;
    }
    m_dlci = static_cast<std::uint8_t>(channel << 1); // the initiator's direction bit is 0
    const std::uint8_t pn[8] = {m_dlci, 0xf0, 7, 0, 0x4a, 0x01, 0, 7}; // credit flow, mtu 330, 7 credits
    sendMcc(kPn, true, pn, 8);
    if (!m_bt.wait([this] { return m_pnDone; }, 5000))
    {
        logLine("hfp: no PN answer");
        return false;
    }
    sendFrame(m_dlci, kSabm | kPf, true, nullptr, 0);
    if (!m_bt.wait([this] { return m_ua[m_dlci]; }, 5000))
    {
        logLine("hfp: no UA for dlci %u", m_dlci);
        return false;
    }
    m_dlciOpen = true;
    const std::uint8_t msc[2] = {static_cast<std::uint8_t>((m_dlci << 2) | 2 | 1), 0x8d};
    sendMcc(kMsc, true, msc, 2);
    logLine("hfp: rfcomm open to channel %d (dlci %u)", channel, m_dlci);
    return true;
}


bool HfpAg::connect()
{
    // a headset that dialled in opens RFCOMM itself; one we called waits for us
    if (!m_bt.wait([this] { return m_slc || m_ch.up; }, 4000) && m_bt.linkUp() && !m_ch.up)
    {
        connectOut();
    }
    m_bt.wait([this] { return m_slc; }, 10000);
    logLine("hfp: service level connection %s, headset features %#x, codecs %s", m_slc ? "up" : "not up",
            m_hfFeatures, m_codecs.c_str());
    return m_slc;
}

int HfpAg::openAudio(bool allowWide)
{
    if (!m_slc)
    {
        return 0;
    }
    reply("+VGS: 12");
    reply("+VGM: 8"); // the AirPods distort at 15
    bool wide = allowWide && m_codecs.find('2') != std::string::npos;
    if (wide)
    {
        m_codec = 0;
        reply("+BCS: 2");
        wide = m_bt.wait([this] { return m_codec != 0; }, 3000) && m_codec == 2;
    }
    struct Setting
    {
        std::uint16_t packets;
        std::uint16_t latency;
        std::uint8_t retransmission;
        const char* name;
    };
    // The Linux kernel's chains; the EDR bits mean "not allowed".
    const Setting msbc[] = {{0x0380, 0x000d, 0x02, "T2"}, {0x03c8, 0x0008, 0x02, "T1"}};
    const Setting cvsd[] = {{0x0380, 0x000a, 0x01, "S3"},
                            {0x0380, 0x0007, 0x01, "S2"},
                            {0x03c8, 0x0007, 0x01, "S1"},
                            {0x03c4, 0xffff, 0x01, "D1"}};
    const Setting* chain = wide ? msbc : cvsd;
    const int count = wide ? 2 : 4;
    // voice setting: transparent for mSBC; CVSD from 8-bit two's complement samples
    const std::uint16_t voice = wide ? 0x0063 : 0x0040;
    for (int i = 0; i < count && m_bt.linkUp(); i++)
    {
        const int status = m_bt.setupSco(chain[i].packets, chain[i].latency, chain[i].retransmission, voice);
        logLine("hfp: voice link %s (%s) -> %d", chain[i].name, wide ? "mSBC" : "CVSD", status);
        if (status == 0)
        {
            return wide ? 2 : 1;
        }
    }
    return 0;
}

void HfpAg::closeAudio()
{
    m_bt.dropSco();
}

namespace
{
constexpr int kPacket = 24;
const std::uint8_t kH2[4] = {0x08, 0x38, 0xc8, 0xf8};

std::int64_t nowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

HfpVoice::HfpVoice(bool wide, std::uint16_t handle) : m_wide(wide), m_handle(handle)
{
    if (wide)
    {
        sbc_init_msbc(&m_enc, 0);
        sbc_init_msbc(&m_dec, 0);
    }
    rs_init(&m_up, wide ? 3 : 6, 1);   // to 48 kHz
    rs_init(&m_down, 1, wide ? 3 : 6); // from 48 kHz
    m_nextUs = nowUs();
}

HfpVoice::~HfpVoice()
{
    if (m_wide)
    {
        sbc_finish(&m_enc);
        sbc_finish(&m_dec);
    }
}

void HfpVoice::receive(const std::uint8_t* bytes, int n, const std::function<void(const std::int16_t*, int)>& mic)
{
    m_in.insert(m_in.end(), bytes, bytes + n);
    // HCI SCO packets: handle (12 bits) and flags, a length byte, the data
    while (m_in.size() >= 3)
    {
        const std::uint16_t h = static_cast<std::uint16_t>(m_in[0] | (m_in[1] << 8));
        const int len = m_in[2];
        if ((h & 0x0fff) != m_handle)
        {
            m_in.erase(m_in.begin()); // not a header: find the next one
            continue;
        }
        if (m_in.size() < static_cast<std::size_t>(3 + len))
        {
            break;
        }
        m_voice.insert(m_voice.end(), m_in.begin() + 3, m_in.begin() + 3 + len);
        m_in.erase(m_in.begin(), m_in.begin() + 3 + len);
    }
    float link[120], up[720];
    std::int16_t pcm[720];
    int got = 0;
    if (m_wide)
    {
        // mSBC frames: the H2 header and the sync byte, then 57 bytes
        while (m_voice.size() >= 60)
        {
            if (!(m_voice[0] == 0x01 && (m_voice[1] & 0x0f) == 0x08 && m_voice[2] == 0xad))
            {
                m_voice.erase(m_voice.begin());
                continue;
            }
            std::int16_t s[120] = {};
            std::size_t written = 0;
            if (sbc_decode(&m_dec, m_voice.data() + 2, 57, s, sizeof s, &written) <= 0)
            {
                std::memset(s, 0, sizeof s);
            }
            m_voice.erase(m_voice.begin(), m_voice.begin() + 60);
            m_frames++;
            for (int i = 0; i < 120; i++)
            {
                link[i] = s[i] / 32768.0f;
            }
            got = rs_process(&m_up, link, 120, up, 720);
            for (int i = 0; i < got; i++)
            {
                pcm[i] = static_cast<std::int16_t>(up[i] > 1 ? 32767 : up[i] < -1 ? -32768 : up[i] * 32767);
            }
            mic(pcm, got);
        }
        return;
    }
    // CVSD: the controller decodes; 8-bit samples
    while (m_voice.size() >= 24)
    {
        for (int i = 0; i < 24; i++)
        {
            link[i] = static_cast<std::int8_t>(m_voice[static_cast<std::size_t>(i)]) / 128.0f;
        }
        m_voice.erase(m_voice.begin(), m_voice.begin() + 24);
        m_frames++;
        got = rs_process(&m_up, link, 24, up, 720);
        for (int i = 0; i < got; i++)
        {
            pcm[i] = static_cast<std::int16_t>(up[i] > 1 ? 32767 : up[i] < -1 ? -32768 : up[i] * 32767);
        }
        mic(pcm, got);
    }
}

void HfpVoice::play(const float* mono, int n)
{
    float out[2048];
    while (n > 0)
    {
        const int chunk = n < 1024 ? n : 1024;
        const int got = rs_process(&m_down, mono, chunk, out, 2048);
        m_play.insert(m_play.end(), out, out + got);
        mono += chunk;
        n -= chunk;
    }
    const std::size_t limit = m_wide ? 8000 : 4000; // over half a second behind: stay live
    if (m_play.size() > limit)
    {
        m_play.erase(m_play.begin(), m_play.end() - static_cast<long>(limit / 5));
    }
}

void HfpVoice::packet(std::uint8_t* out)
{
    if (m_wide)
    {
        // code another 7.5 ms frame whenever less than a packet is left
        while (static_cast<int>(m_out.size()) < kPacket)
        {
            std::int16_t s[120] = {};
            const int have = m_play.size() >= 120 ? 120 : 0;
            for (int i = 0; i < have; i++)
            {
                const float v = m_play[static_cast<std::size_t>(i)];
                s[i] = static_cast<std::int16_t>(v > 1 ? 32767 : v < -1 ? -32768 : v * 32767);
            }
            m_play.erase(m_play.begin(), m_play.begin() + have);
            std::uint8_t frame[60] = {0x01, kH2[m_seq++ & 3]};
            ssize_t written = 0;
            sbc_encode(&m_enc, s, sizeof s, frame + 2, 57, &written);
            m_out.insert(m_out.end(), frame, frame + 60);
        }
        std::memcpy(out, m_out.data(), kPacket);
        m_out.erase(m_out.begin(), m_out.begin() + kPacket);
        return;
    }
    for (int i = 0; i < kPacket; i++)
    {
        const float v = i < static_cast<int>(m_play.size()) ? m_play[static_cast<std::size_t>(i)] : 0;
        out[i] = static_cast<std::uint8_t>(static_cast<std::int8_t>(v > 1 ? 127 : v < -1 ? -128 : v * 127));
    }
    m_play.erase(m_play.begin(), m_play.begin() + (m_play.size() < kPacket ? static_cast<long>(m_play.size()) : kPacket));
}

void HfpVoice::pump()
{
    // 24 bytes are 3 ms either way (8000 bytes a second)
    while (nowUs() + 30000 > m_nextUs)
    {
        std::uint8_t pkt[3 + kPacket];
        pkt[0] = static_cast<std::uint8_t>(m_handle);
        pkt[1] = static_cast<std::uint8_t>(m_handle >> 8);
        pkt[2] = kPacket;
        packet(pkt + 3);
        g_iso.write(pkt, sizeof pkt);
        m_nextUs += 3000;
    }
}
