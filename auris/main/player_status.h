// Shared playback state published by the audio task and consumed by the UI task.
#ifndef PLAYER_STATUS_H
#define PLAYER_STATUS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PLAYER_PHASE_IDLE = 0,   // Nothing loaded yet
    PLAYER_PHASE_LOADING,     // File opened, waiting for the first decoded frame
    PLAYER_PHASE_PLAYING,     // Decoding and feeding I2S
    PLAYER_PHASE_FINISHED,    // End of file reached
    PLAYER_PHASE_ERROR,       // Open/decode/I2S failure, see `message`
} player_phase_t;

// Everything the display needs to know about the current track. Fields are only
// written by the audio task through player_status_set() and read by the display
// task through player_status_get(), both of which copy under a mutex.
typedef struct {
    char            path[48];
    char            message[32];        // Status line / error detail
    uint32_t        sample_rate;        // Hz
    uint32_t        bitrate_kbps;
    uint8_t         channels;
    uint8_t         bits_per_sample;    // PCM sample width fed to I2S
    uint8_t         layer;              // 1, 2 or 3
    uint32_t        file_size;          // Bytes
    uint32_t        file_pos;           // Bytes consumed so far
    uint32_t        samples_played;     // Decoded samples per channel
    uint32_t        total_seconds;      // Track length; 0 until the first frame
    bool            total_exact;        // Counted, so the UI drops its '~' marker
    player_phase_t  phase;
} player_status_t;

// Creates the mutex guarding the status. Call once from app_main().
void player_status_init(void);

// Publish a new snapshot (whole struct is copied under the lock).
void player_status_set(const player_status_t *status);

// Fetch the latest snapshot (whole struct is copied under the lock).
void player_status_get(player_status_t *out);

#endif // PLAYER_STATUS_H
