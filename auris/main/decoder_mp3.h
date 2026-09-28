#ifndef DECODER_MP3_H
#define DECODER_MP3_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

// 1152 samples per channel * 2 channels (stereo) = 2304 16-bit samples
#define MP3_MAX_SAMPLES_PER_FRAME 2304

typedef struct {
    uint32_t sample_rate;
    uint8_t  channels;
    uint8_t  bits_per_sample;
    uint8_t  layer;            // 1, 2 or 3
    uint32_t bitrate_kbps;     // Average over everything decoded so far, NOT
                               // the first frame (which is meaningless for VBR).
    bool     vbr;              // Frame bitrates differ, or a Xing header was found
} mp3_info_t;

typedef struct {
    FILE       *file;
    uint32_t    file_size;     // Bytes, captured at init (after seeking back)
    mp3_info_t  info;
    uint8_t    *input_buf;
    size_t      buf_size;
    size_t      buf_filled;
    size_t      buf_pos;
    bool        is_header_parsed;
    void       *mp3dec_ctx;

    // VBR/duration accounting, all updated by mp3_decoder_read_pcm().
    uint32_t    samples_per_frame;   // Per channel, from the first frame header
    uint32_t    frames_decoded;
    uint64_t    stream_bytes;        // Bytes consumed by decoded frames
    uint64_t    total_samples;       // Per channel
    uint32_t    min_frame_bitrate;   // 0 until two frames have been seen
    uint32_t    xing_frames;         // 0 when the file has no Xing/Info count
    uint32_t    xing_bytes;          // Stream byte count from the same header
    bool        xing_checked;        // First frame's header already inspected

    // Set when the first frame carried a usable Xing/Info frame count, which
    // makes mp3_decoder_total_seconds() exact instead of an estimate.
    bool        has_xing_count;
} mp3_decoder_t;

bool mp3_decoder_init(mp3_decoder_t *dec, FILE *file);
int mp3_decoder_read_pcm(mp3_decoder_t *dec, int16_t *pcm_out, size_t max_samples);
void mp3_decoder_deinit(mp3_decoder_t *dec);

// Best available total track length in seconds.
//
// Exact when the file carries a Xing/Info frame count (most LAME-encoded VBR
// files do, and that count includes every audio frame in the file). Otherwise
// it is an estimate from the file size and the average bitrate seen so far, so
// it settles as decoding proceeds. Returns 0 before the first frame is decoded.
uint32_t mp3_decoder_total_seconds(const mp3_decoder_t *dec);

// True when mp3_decoder_total_seconds() is a counted duration rather than an
// estimate. The UI uses this to decide whether to prefix the total with '~'.
bool mp3_decoder_total_is_exact(const mp3_decoder_t *dec);

// File offset of the next frame that has not been decoded yet. Unlike a bare
// ftell() this accounts for the read-ahead buffer, so it is the real playback
// position and can be compared against the file size for a progress bar.
uint32_t mp3_decoder_position(const mp3_decoder_t *dec);

#endif // DECODER_MP3_H