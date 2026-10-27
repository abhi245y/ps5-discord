// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pcm_stream.h"
#include "ds5_usb_format.h"
#include <errno.h>
#include <string.h>
#include <sys/socket.h>

int ds5_pcm_microphone(ds5_pcm_stream *stream, const int16_t *mono, size_t frames, int muted)
{
    if (frames > DS5_USB_MAX_MIC_FRAMES || (!mono && frames))
        return -1;
    stream->mic_frames += frames;
    if (stream->tx_offset < stream->tx_size) {
        stream->mic_dropped_frames += frames;
        return 0;
    }
    stream->tx_offset = 0;
    stream->tx_size = frames * 2;
    for (size_t i = 0; i < frames; i++)
        ds5_put_le(stream->tx + 2 * i, muted ? 0 : (uint16_t)mono[i], 2);
    return 1;
}

void ds5_pcm_receive(ds5_pcm_stream *stream, const uint8_t *bytes, size_t count)
{
    stream->received_bytes += count;
    for (size_t i = 0; i < count; i++) {
        stream->carry[stream->carry_size++] = bytes[i];
        if (stream->carry_size != 4)
            continue;
        stream->carry_size = 0;
        if (stream->count == DS5_PCM_RX_FRAMES) {
            stream->read = (stream->read + 1) % DS5_PCM_RX_FRAMES;
            stream->count--;
            stream->playback_dropped_frames++;
        }
        unsigned at = (stream->read + stream->count) % DS5_PCM_RX_FRAMES;
        for (unsigned ch = 0; ch < 2; ch++) {
            int value = ds5_le16(stream->carry + 2 * ch);
            stream->rx[at][ch] = (int16_t)(value >= 32768 ? value - 65536 : value);
        }
        stream->count++;
        stream->playback_frames++;
    }
}

size_t ds5_pcm_playback(ds5_pcm_stream *stream, int16_t *stereo, size_t frames)
{
    if (!stream->primed) {
        if (stream->count < DS5_PCM_PREFILL)
            return 0;
        stream->primed = 1;
    }
    size_t count = frames < stream->count ? frames : stream->count;
    for (size_t i = 0; i < count; i++) {
        memcpy(stereo + i * 2, stream->rx[stream->read], sizeof stream->rx[0]);
        stream->read = (stream->read + 1) % DS5_PCM_RX_FRAMES;
        stream->count--;
    }
    if (count < frames || !stream->count)
        stream->primed = 0;
    return count;
}

int ds5_pcm_pump(ds5_pcm_stream *stream, int fd)
{
    if (stream->tx_offset < stream->tx_size) {
        ssize_t sent = send(fd, stream->tx + stream->tx_offset, stream->tx_size - stream->tx_offset,
                            MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent > 0) {
            stream->tx_offset += (size_t)sent;
            stream->sent_bytes += (uint64_t)sent;
        } else if (!sent) {
            return 1;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return errno == EPIPE || errno == ECONNRESET ? 1 : -1;
        }
    }
    for (unsigned budget = 0; budget < 4; budget++) {
        uint8_t bytes[4096];
        ssize_t received = recv(fd, bytes, sizeof bytes, MSG_DONTWAIT);
        if (received > 0)
            ds5_pcm_receive(stream, bytes, (size_t)received);
        else if (!received)
            return 1;
        else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            break;
        else
            return errno == ECONNRESET ? 1 : -1;
    }
    return 0;
}
