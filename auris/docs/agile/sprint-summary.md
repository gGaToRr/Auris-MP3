# Sprint summaries

## Sprint 1 — Display prototype

### Sprint goal

Establish a working ESP32 display prototype and document the work as a traceable Agile story set.

### Completed work

- Replaced the hello-world app with a 480x320 RGB test pattern render path.
- Added the required `esp_lcd_qemu_rgb` dependency.
- Aligned the workspace with the local ESP-IDF toolchain and build configuration.
- Added a KiCad board project to support hardware-side evolution.

### Definition of done

The sprint is considered complete for the software prototype step when:

- the build is consistent with the current toolchain,
- the app initializes the virtual LCD panel,
- the display pattern renders successfully,
- the project change set is documented and traceable.

### Outcome

Met. Delivered a visible output path, but the sprint stopped at colour bars —
it proved the display pipeline worked and nothing more. Everything below is
sprint 2.

---

## Sprint 2 — MP3 player

### Sprint goal

Turn the display prototype into a player: decode an MP3, drive an external I2S
DAC, and show what is playing — with the numbers on screen actually true.

### User stories

1. As a developer, I want an MP3 to decode and play so the audio path is real
   rather than a test tone.
2. As a developer, I want playback not to crash the core so a long track can run
   to the end.
3. As a developer, I want a failing I2S write to be visible rather than a silent
   freeze so I can tell hardware faults from firmware faults.
4. As a listener, I want the volume reduced and the start and end of the track
   faded so earbuds are usable and do not click.
5. As a listener, I want to see the track name, format and progress so I know
   what I am hearing.
6. As a listener, I want to be able to tell `0`, `O` and `D` apart at a glance.
7. As a listener, I want the bitrate and total time to be correct on a VBR file.
8. As a developer, I want to verify the decoder without hardware so I can change
   it safely.
9. As a project lead, I want this work recorded and traceable.

### Completed work

Changes 6 to 14 in the [implementation log](implementation-log.md):

- Diagnosed and fixed the audio task stack overflow (16 KB → 32 KB).
- Made I2S writes non-blocking with a drop counter.
- Enabled the PCM5102A master clock on GPIO8.
- Added output gain and 20 ms fades at both ends, with a one-block lookahead.
- Replaced the colour bars with a full file-info UI on a 480x480 panel.
- Swapped the font for a hand-drawn 8x8 console face, fixing `0` / `O` / `D`.
- Corrected VBR bitrate and duration via Xing/Info header parsing.
- Validated the decoder against ffmpeg off-target.
- Moved to a 16 MB custom partition layout with a build-time FAT storage image.

### Acceptance criteria

- [x] 90 s of continuous playback in QEMU with zero panics.
- [x] Stack high-water mark reported and comfortably clear of zero.
- [x] A failing I2S write is counted and logged instead of blocking.
- [x] MCLK routed to SCKI; generated clock ratios documented.
- [x] Output gain adjustable by one constant; playback starts and ends silent.
- [x] Panel shows metadata, a running clock, a progress bar and a percent.
- [x] `0`, `O` and `D` distinct at every UI scale; cap heights consistent.
- [x] Displayed bitrate and total match the real file, VBR and CBR alike.
- [x] Decoder output correlates with a reference decoder on channel identity,
      duration and spectrum.
- [x] Build produces one merged 16 MB image.
- [ ] **Audio is audible on real hardware.** Blocked on a physical ESP32-S3.

### Definition of done

The sprint is complete when the player runs on the target board and sound comes
out of the earbuds. Everything up to that point is done and evidenced; the last
criterion is the one that is not.

### Current status

- Status: Complete in simulation, blocked on hardware bring-up
- Confidence: High for the software, unknown for the audio
- Remaining risk: nothing has been listened to; QEMU models no I2S peripheral
  for the ESP32-S3, so the clock pin and the analogue chain are both unverified

### Verification evidence

| Check | Result |
| --- | --- |
| QEMU playback run | 0 panics, ~14.5 KB stack still free |
| Panel readback (OCR of a QEMU screendump) | 0 unmatched glyph cells |
| Displayed metadata | `44.1 KHZ  233 KBPS  STEREO`, `00:27 / 04:05`, `11%` |
| Ground truth for that file (ffprobe) | 245.41 s, 233.3 kbps average — matches |
| Progress bar | 47 px filled, exactly 11% of the 436 px track |
| End of playback | `FINISHED`, `00:08 / 00:08`, `100%`, `PLAYBACK COMPLETE` |
| Gain and fade arithmetic | 19 host checks, pass at gain 1/1, 1/2, 1/4, 1/8 |
| Decoder vs ffmpeg | 0.998 same-channel, 0.000 cross-channel, identical duration |
| Font regeneration | byte-identical from source |
| Build | clean, no warnings; app partition 87% free |

### Next sprint recommendations

1. Bring up the PCM5102A on the real board: scope BCLK, WS and DOUT before
   fitting earbuds, so a digital fault is not mistaken for an analogue one.
2. Confirm MCLK on GPIO8 against the ESP32-S3 datasheet, and check the module's
   MODE pin strapping against the 256 × fS ratio the firmware generates.
3. Decide the font licensing question (GPL-2) before any distribution.
4. Move the host verification harnesses out of `/tmp/opencode/` into
   `auris/tools/` so the decoder and gain/fade work keep a regression suite.
5. Link the firmware pin map to the KiCad board design.

### Deferred, by agreement

- The latent `max_samples` overrun in `mp3_decoder_read_pcm()`.
- The mono upmix path.
- Any refactor of the audio path beyond what the volume and fade work required.

### Agile metrics

| Metric | Sprint 1 | Sprint 2 |
| --- | --- | --- |
| Story count | 4 | 9 |
| Change log entries | 5 | 9 (cumulative 14) |
| Acceptance criteria met | 4 / 4 | 10 / 11 |
| Open follow-up items | 3 | 5 |
| Primary blocker | hardware validation | hardware validation, narrowed to audio |

The blocker is the same one sprint 1 ended on, but it is now a single specific
question — does sound come out of SCKI — instead of a general "validate on
hardware".
