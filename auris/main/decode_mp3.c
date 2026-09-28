#include <stdlib.h>
#include <string.h>
#include "decoder_mp3.h"

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#define IN_BUF_SIZE (4 * 1024) // 4KB read buffer for MP3 stream chunks

static size_t skip_id3v2_tag(FILE *file)
{
    uint8_t header[10];
    if (fread(header, 1, 10, file) < 10) {
        fseek(file, 0, SEEK_SET);
        return 0;
    }

    // Check for ID3v2 magic bytes "ID3"
    if (header[0] == 'I' && header[1] == 'D' && header[2] == '3') {
        // Calculate tag size (synchsafe integer encoding)
        size_t tag_size = ((header[6] & 0x7F) << 21) |
                          ((header[7] & 0x7F) << 14) |
                          ((header[8] & 0x7F) << 7)  |
                           (header[9] & 0x7F);
        size_t total_id3_len = 10 + tag_size;
        fseek(file, total_id3_len, SEEK_SET);
        return total_id3_len;
    }

    fseek(file, 0, SEEK_SET);
    return 0;
}

bool mp3_decoder_init(mp3_decoder_t *dec, FILE *file)
{
    if (!dec || !file) return false;

    memset(dec, 0, sizeof(mp3_decoder_t));
    dec->file = file;

    // Remember the total size before anything else moves the cursor: the
    // duration estimate needs it and the ID3 skip below seeks anyway.
    if (fseek(file, 0, SEEK_END) == 0) {
        long end = ftell(file);
        if (end > 0) dec->file_size = (uint32_t)end;
    }
    rewind(file);

    // Skip ID3v2 metadata header if present
    skip_id3v2_tag(file);

    // Allocate ring/stream input buffer
    dec->input_buf = malloc(IN_BUF_SIZE);
    if (!dec->input_buf) return false;
    dec->buf_size = IN_BUF_SIZE;

    // Allocate minimp3 state decoder handle
    mp3dec_t *mp3d = malloc(sizeof(mp3dec_t));
    if (!mp3d) {
        free(dec->input_buf);
        return false;
    }
    mp3dec_init(mp3d);
    dec->mp3dec_ctx = mp3d;

    return true;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

// Reads the Xing/Info header that encoders hide in the first MPEG frame of a
// VBR file. LAME writes one, and its frame count covers every audio frame in
// the file, so the total duration becomes exact without decoding the track.
//
// The frame header is not re-parsed in full: the version and layer bits are two
// fields of byte 1, and the side-information length follows from the version
// plus the channel count minimp3 already reported. The tag sits immediately
// after that side information.
static bool parse_xing_header(mp3_decoder_t *dec, const uint8_t *frame,
                              size_t available, uint8_t channels)
{
    if (available < 4) return false;
    if (frame[0] != 0xFF || (frame[1] & 0xE0) != 0xE0) return false;   // frame sync

    unsigned version = (frame[1] >> 3) & 0x03;    // 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5
    if (version == 1) return false;                                  // reserved

    size_t side_info = (version == 3) ? (channels == 1 ? 17u : 32u)
                                      : (channels == 1 ?  9u : 17u);
    size_t tag = 4 + side_info;
    if (available < tag + 12) return false;

    bool is_info = (memcmp(frame + tag, "Info", 4) == 0);
    if (!is_info && memcmp(frame + tag, "Xing", 4) != 0) return false;

    uint32_t flags = be32(frame + tag + 4);
    if (!(flags & 0x0001u)) return false;   // no frame count -> nothing usable

    uint32_t frames = be32(frame + tag + 8);
    if (frames == 0) return false;

    dec->xing_frames    = frames;
    dec->has_xing_count = true;
    dec->info.vbr       = true;         // only written for variable bitrate

    // Flag bit 1 is the file's total stream byte count. Not needed for the
    // duration (the frame count already gives that) but it makes the average
    // bitrate exact from the very first block instead of a running estimate.
    if (flags & 0x0002u) {
        dec->xing_bytes = be32(frame + tag + 12);
    }
    return true;
}

// Keeps mp3_info_t::bitrate_kbps an average over the whole file rather than
// whatever the first frame happened to be - on a VBR track the first frame is
// just the bit rate the encoder started at (64 KBPS on a 233 KBPS file).
static void update_average_bitrate(mp3_decoder_t *dec)
{
    if (dec->info.sample_rate == 0) return;

    uint64_t samples, bytes;
    if (dec->has_xing_count && dec->xing_bytes > 0 && dec->samples_per_frame > 0) {
        // Whole-file figures straight from the encoder's header, so the number
        // is right as soon as the first frame is decoded.
        samples = (uint64_t)dec->xing_frames * dec->samples_per_frame;
        bytes   = dec->xing_bytes;
    } else if (dec->total_samples * 2u >= dec->info.sample_rate) {
        // Otherwise accumulate a running average, but only from half a second
        // of audio onwards: before that it is mostly encoder start-up.
        samples = dec->total_samples;
        bytes   = dec->stream_bytes;
    } else {
        return;
    }

    if (samples == 0 || bytes == 0) return;

    // bit/s = bytes * 8 * sample_rate / samples, rounded to the nearest kbps.
    uint64_t num = bytes * 8ull * dec->info.sample_rate;
    uint64_t den = samples * 1000ull;
    dec->info.bitrate_kbps = (uint32_t)((num + den / 2u) / den);
}

int mp3_decoder_read_pcm(mp3_decoder_t *dec, int16_t *pcm_out, size_t max_samples)
{
    // Guard against NULL handles
    if (!dec || !dec->file || !dec->input_buf || !dec->mp3dec_ctx || !pcm_out) {
        return 0;
    }

    mp3dec_t *mp3d = (mp3dec_t *)dec->mp3dec_ctx;
    mp3dec_frame_info_t info;

    while (1) {
        if (dec->buf_pos > 0) {
            size_t remaining = dec->buf_filled - dec->buf_pos;
            if (remaining > 0) {
                memmove(dec->input_buf, dec->input_buf + dec->buf_pos, remaining);
            }
            dec->buf_filled = remaining;
            dec->buf_pos = 0;
        }

        size_t space_available = dec->buf_size - dec->buf_filled;
        if (space_available > 0 && !feof(dec->file)) {
            size_t read_bytes = fread(dec->input_buf + dec->buf_filled, 1, space_available, dec->file);
            dec->buf_filled += read_bytes;
        }

        if (dec->buf_filled == 0) {
            return 0; // EOF reached
        }

        int samples = mp3dec_decode_frame(
            mp3d,
            dec->input_buf + dec->buf_pos,
            dec->buf_filled - dec->buf_pos,
            pcm_out,
            &info
        );

        dec->buf_pos += info.frame_bytes;

        // The very first frame in the file may carry the encoder's Xing/Info
        // header. Inspect it even when the frame itself decodes to silence,
        // which is what the Xing placeholder frame does.
        if (!dec->xing_checked && info.frame_bytes > 0 && info.hz > 0) {
            dec->xing_checked = true;
            size_t frame_start = dec->buf_pos - (size_t)info.frame_bytes;
            if (frame_start < dec->buf_filled) {
                parse_xing_header(dec, dec->input_buf + frame_start,
                                  dec->buf_filled - frame_start,
                                  (uint8_t)(info.channels ? info.channels : 1));
            }
        }

        if (samples > 0) {
            // mp3dec_decode_frame() returns samples PER CHANNEL; the return
            // value below multiplies by the channel count to give the
            // interleaved count the caller writes to I2S.
            uint8_t channels = info.channels ? (uint8_t)info.channels : 1;
            uint32_t samples_per_channel = (uint32_t)samples;

            if (!dec->is_header_parsed) {
                dec->info.sample_rate     = info.hz;
                dec->info.channels        = channels;
                dec->info.bits_per_sample = 16;
                dec->info.layer           = info.layer;
                dec->info.bitrate_kbps    = info.bitrate_kbps;
                dec->min_frame_bitrate    = info.bitrate_kbps;
                dec->samples_per_frame    = samples_per_channel;
                dec->is_header_parsed     = true;
            }

            // Running statistics for the VBR/duration reporting.
            dec->frames_decoded++;
            dec->stream_bytes  += (size_t)info.frame_bytes;
            dec->total_samples += samples_per_channel;
            if ((uint32_t)info.bitrate_kbps != dec->min_frame_bitrate) {
                dec->info.vbr = true;
            }
            update_average_bitrate(dec);

            return samples * info.channels;
        }

        if (info.frame_bytes == 0) {
            dec->buf_pos++;
            if (dec->buf_pos >= dec->buf_filled && feof(dec->file)) {
                return 0;
            }
        }
    }
}

bool mp3_decoder_total_is_exact(const mp3_decoder_t *dec)
{
    return dec && dec->has_xing_count && dec->samples_per_frame > 0;
}

uint32_t mp3_decoder_total_seconds(const mp3_decoder_t *dec)
{
    if (!dec || !dec->is_header_parsed || dec->info.sample_rate == 0) return 0;

    // Exact: the encoder counted every audio frame, and a stream of one format
    // carries the same number of samples in each of them.
    if (mp3_decoder_total_is_exact(dec)) {
        uint64_t samples = (uint64_t)dec->xing_frames * dec->samples_per_frame;
        return (uint32_t)(samples / dec->info.sample_rate);
    }

    // Estimate: scale the bytes decoded so far up to the whole file at the
    // bit rate they decode to,
    //     seconds = file_size * decoded_samples / (decoded_bytes * sample_rate)
    // which avoids rounding through an integer kbps figure. For CBR the very
    // first frame already gives the right answer; for VBR without a Xing header
    // the number settles during the first second of playback.
    if (dec->total_samples == 0 || dec->stream_bytes == 0) return 0;
    return (uint32_t)(((uint64_t)dec->file_size * dec->total_samples) /
                      (dec->stream_bytes * dec->info.sample_rate));
}

uint32_t mp3_decoder_position(const mp3_decoder_t *dec)
{
    if (!dec || !dec->file) return 0;

    long file_pos = ftell(dec->file);
    if (file_pos < 0) return 0;

    // Bytes pulled from the file by fread() but not consumed by the decoder yet.
    size_t pending = dec->buf_filled - dec->buf_pos;
    if ((size_t)file_pos <= pending) return 0;

    return (uint32_t)(file_pos - pending);
}

void mp3_decoder_deinit(mp3_decoder_t *dec)
{
    if (!dec) return;
    if (dec->input_buf) {
        free(dec->input_buf);
        dec->input_buf = NULL;
    }
    if (dec->mp3dec_ctx) {
        free(dec->mp3dec_ctx);
        dec->mp3dec_ctx = NULL;
    }
}