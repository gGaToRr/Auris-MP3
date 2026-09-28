#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    AUDIO_FMT_UNKNOWN = 0,
    AUDIO_FMT_MP3,
    AUDIO_FMT_FLAC,
    AUDIO_FMT_WAV,
    AUDIO_FMT_OGG,
    AUDIO_FMT_AAC,
} audio_format_t;

typedef struct {
    uint32_t sample_rate;
    uint8_t  channels;
    uint8_t  bits_per_sample;
    uint32_t total_samples;
} audio_info_t;

// Unified Decoder Abstract Interface
typedef struct audio_decoder {
    audio_format_t format;
    void *priv_data; // Decoder-specific context state

    bool (*open)(struct audio_decoder *dec, const char *filepath, audio_info_t *info);
    int  (*decode)(struct audio_decoder *dec, int16_t *pcm_out, size_t max_samples, audio_info_t *info);
    void (*close)(struct audio_decoder *dec);
} audio_decoder_t;