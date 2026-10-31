// SPDX-License-Identifier: AGPL-3.0-or-later
#include "aacp_mic.hpp"

#include <chrono>
#include <cstring>

#ifdef HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
}
#endif

#include "core/bytes.hpp"
#include "core/ports/log.hpp"

using bluev::getLe16;
using bluev::logLine;

namespace
{
constexpr std::uint16_t kAacpPsm = 0x1001;
// As LibrePods sends them: the 04 00 04 00 header, then the message type.
const std::uint8_t kHandshake[] = {0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00,
                                   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const std::uint8_t kFeatureFlags[] = {0x04, 0x00, 0x04, 0x00, 0x4d, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const std::uint8_t kNotifications[] = {0x04, 0x00, 0x04, 0x00, 0x0f, 0x00, 0xff, 0xff, 0xff, 0xff};
const std::uint8_t kStartAudio[] = {0x04, 0x00, 0x04, 0x00, 0x58, 0x00, 0x00, 0x00, 0x09, 0x00,
                                    0x00, 0x01, 0x82, 0x00, 0x00, 0x00, 0x04, 0x96, 0x00};
const std::uint8_t kStopAudio[] = {0x04, 0x00, 0x04, 0x00, 0x58, 0x00, 0x00, 0x00, 0x02, 0x00, 0x03, 0x01};
} // namespace

AacpMic::~AacpMic()
{
    close();
}

void AacpMic::send(const std::uint8_t* d, int n)
{
    m_bt.wait([this] { return m_bt.canSend(); }, 500);
    m_bt.send(m_ch.theirCid, d, n);
}

bool AacpMic::open()
{
    m_ch.reset();
    m_open = m_bt.openChannel(m_ch, kAacpPsm, [this](std::uint32_t, const std::uint8_t* d, int n) { onFrame(d, n); });
    if (!m_open)
    {
        logLine("aacp: no channel (not AirPods)");
        return false;
    }
    send(kHandshake, sizeof kHandshake);
    m_bt.wait([] { return false; }, 300);
    send(kFeatureFlags, sizeof kFeatureFlags);
    m_bt.wait([] { return false; }, 300);
    send(kNotifications, sizeof kNotifications);
    m_bt.wait([] { return false; }, 300);
    logLine("aacp: channel open");
    return true;
}

void AacpMic::close()
{
    stop();
#ifdef HAVE_FFMPEG
    if (m_ctx != nullptr)
    {
        av_frame_free(&m_frame);
        av_packet_free(&m_pkt);
        avcodec_free_context(&m_ctx);
    }
#endif
    if (m_open)
    {
        m_bt.closeChannel(m_ch);
    }
    m_open = false;
}

void AacpMic::start()
{
    if (!m_open || m_running)
    {
        return;
    }
    rs_init(&m_rs, 3, 4); // 64 kHz to 48 kHz
    m_silent = 0;
    m_framesAtTick = m_frames;
    send(kStartAudio, sizeof kStartAudio);
    m_running = true;
    logLine("aacp: mic started");
}

void AacpMic::stop()
{
    if (!m_running)
    {
        return;
    }
    send(kStopAudio, sizeof kStopAudio);
    m_running = false;
    logLine("aacp: mic stopped: %ld frames, %ld errors, %.0f us per frame", m_frames, m_errors, decodeUs());
}

int AacpMic::silentSeconds()
{
    if (!m_running)
    {
        return 0;
    }
    m_silent = m_frames == m_framesAtTick ? m_silent + 1 : 0;
    m_framesAtTick = m_frames;
    return m_silent;
}

// 0x58 audio messages: 04 00 04 00 58 00 01 00, a 22-byte header, then
// [timestamp u32][length u8][frame] repeated.
void AacpMic::onFrame(const std::uint8_t* d, int n)
{
    const bool audio = n >= 8 && d[0] == 0x04 && d[2] == 0x04 && getLe16(d + 4) == 0x58 && getLe16(d + 6) == 0x0001;
    if (!audio || !m_running)
    {
        return;
    }
    for (int off = 22; off + 5 <= n;)
    {
        const int len = d[off + 4];
        if (off + 5 + len > n)
        {
            break;
        }
        decode(d + off + 5, len);
        off += 5 + len;
    }
}

// AAC-ELD as LibrePods configures it: F8 E6 30 00 (object type 39, mono, 480-sample
// frames, no SBR). The configuration says 48 kHz; the stream is really 64 kHz.
void AacpMic::decode(const std::uint8_t* au, int length)
{
#ifdef HAVE_FFMPEG
    if (m_ctx == nullptr)
    {
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
        m_ctx = codec != nullptr ? avcodec_alloc_context3(codec) : nullptr;
        if (m_ctx == nullptr)
        {
            return;
        }
        static const std::uint8_t asc[4] = {0xf8, 0xe6, 0x30, 0x00};
        m_ctx->extradata = static_cast<std::uint8_t*>(av_mallocz(4 + AV_INPUT_BUFFER_PADDING_SIZE));
        std::memcpy(m_ctx->extradata, asc, 4);
        m_ctx->extradata_size = 4;
        m_ctx->sample_rate = 48000;
        av_channel_layout_default(&m_ctx->ch_layout, 1);
        m_pkt = av_packet_alloc();
        m_frame = av_frame_alloc();
        if (avcodec_open2(m_ctx, codec, nullptr) < 0 || m_pkt == nullptr || m_frame == nullptr)
        {
            logLine("aacp: the AAC decoder would not open");
            av_frame_free(&m_frame);
            av_packet_free(&m_pkt);
            avcodec_free_context(&m_ctx);
            return;
        }
    }
    if (length > 512)
    {
        m_errors++;
        return;
    }
    std::uint8_t buf[512 + AV_INPUT_BUFFER_PADDING_SIZE] = {};
    std::memcpy(buf, au, static_cast<std::size_t>(length));
    m_pkt->data = buf;
    m_pkt->size = length;
    const auto t0 = std::chrono::steady_clock::now();
    if (avcodec_send_packet(m_ctx, m_pkt) < 0 || avcodec_receive_frame(m_ctx, m_frame) < 0 ||
        m_frame->format != AV_SAMPLE_FMT_FLTP || m_frame->nb_samples > 1024)
    {
        m_errors++;
        return;
    }
    m_decodeUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    m_frames++;
    float out[1024];
    const int n = rs_process(&m_rs, reinterpret_cast<const float*>(m_frame->data[0]), m_frame->nb_samples, out, 1024);
    std::int16_t pcm[1024];
    for (int i = 0; i < n; i++)
    {
        const float v = out[i] > 1 ? 1 : out[i] < -1 ? -1 : out[i];
        pcm[i] = static_cast<std::int16_t>(v * 32767);
    }
    m_sink(pcm, n);
#else
    (void)au;
    (void)length;
    m_errors++;
#endif
}
