# Implementation log

## Change 1 — Display test pattern was added to the app

### Context
The original project was a basic ESP32 hello-world template. The current app entry point was replaced with a display-focused implementation.

### Files changed
- [../../main/auris.c](../../main/auris.c)

### What changed
- Added LCD dimensions for a 480x320 virtual panel.
- Added a `draw_test_pattern` function that fills four horizontal bands using RGB565 colors.
- Initialized a virtual RGB LCD using `esp_lcd_new_rgb_qemu`.
- Called `esp_lcd_panel_init` and rendered the test bitmap.

### Why this mattered
This creates a visible output path for the firmware, which is useful for validating that the ESP32 LCD pipeline is functioning before wiring to actual hardware.

### Agile traceability
- Epic: Display validation
- Story: Validate the ESP32 LCD output path
- Acceptance: Firmware creates a display panel and renders a pattern

---

## Change 2 — ESP-LCD component dependency was added

### Context
The project needed the display driver component for the QEMU RGB panel support.

### Files changed
- [../../main/idf_component.yml](../../main/idf_component.yml)

### What changed
- Added `espressif/esp_lcd_qemu_rgb: '*'` to the component manifest.

### Why this mattered
The firmware depends on this component to create the virtual panel implementation used by the display test pattern.

### Agile traceability
- Epic: Build and environment readiness
- Story: Ensure the project can compile with required display support
- Acceptance: Required dependency is declared in the component manifest

---

## Change 3 — IDE and SDK configuration were aligned with the local toolchain

### Context
The workspace was configured for local ESP-IDF and tool-aware C/C++ indexing.

### Files changed
- [../../.vscode/settings.json](../../.vscode/settings.json)
- [../../sdkconfig](../../sdkconfig)

### What changed
- Added clangd configuration pointing to the local ESP-Clang toolchain.
- Set the build directory for compile commands.
- Configured the project for JTAG flashing and the active source file.
- Generated SDK configuration for ESP-IDF 6.0.1 with LCD-related support enabled.

### Why this mattered
This reduces friction in local development and aligns the workspace with the ESP-IDF environment used by the project.

### Agile traceability
- Epic: Developer enablement
- Story: Improve local build and debugging consistency
- Acceptance: Project is configured for the active toolchain and environment

---

## Change 4 — A KiCad board prototype was added

### Context
A board design directory exists outside the firmware project and is part of the current project state.

### Files involved
- [../../../MP3BoardKiCad/](../../../MP3BoardKiCad/)

### What changed
- Added a KiCad board project directory for the MP3 board design.

### Why this mattered
This indicates the project is evolving from a pure firmware prototype toward a hardware design footprint and board integration effort.

### Agile traceability
- Epic: Hardware design readiness
- Story: Prepare the board foundation for future integration
- Acceptance: A board layout project exists in the workspace for iteration

---

## Change 5 — Local workspace/environment files were created

### Context
The current workspace includes additional generated or editor support files.

### Files involved
- [../../.vscode/c_cpp_properties.json](../../.vscode/c_cpp_properties.json)
- [../../dependencies.lock](../../dependencies.lock)
- [../../managed_components/](../../managed_components/)

### What changed
- Added generated dependency state and editor metadata for the project.

### Why this mattered
These files support consistent local builds and dependency resolution, but they should be treated as generated artifacts and reviewed during release packaging.

### Agile traceability
- Epic: Project hygiene and environment consistency
- Story: Keep the workspace reproducible for development and debugging
- Acceptance: Build environment metadata is present without manual intervention

---

# Sprint 2 — MP3 player

Sprint 1 stopped at a colour-bar test pattern. Sprint 2 replaced it with an
actual player: decode an MP3, push it out over I2S, and show what is playing.
Nine changes, logged below in the order they were made.

## Change 6 — The audio task stack overflowed and was crashing the core

### Context
As soon as real decoding was wired up, playback panicked within seconds:
`Guru Meditation Error: Core 1 panic'ed (InstrFetchProhibited)`.

### Files changed
- [../../main/auris.c](../../main/auris.c)

### What changed
- Raised `AUDIO_TASK_STACK_SIZE` from 16 KB to 32 KB.
- Recorded the measurement that justifies it in a comment above the constant.
- Checked the return value of `xTaskCreatePinnedToCore` for both tasks instead
  of ignoring it.
- Logged `uxTaskGetStackHighWaterMark()` when the audio task ends.

### Why this mattered
`mp3dec_decode_frame()` declares a 17,200-byte `mp3dec_scratch_t` as a *local*
variable, so it lands on the calling task's stack. The deepest Layer III path
(`mp3dec_decode_frame` → `L3_decode` → `L3_decode_scalefactors` → `mp3d_DCT_II`
→ `L3_imdct36`) needs about 17.6 KB statically and measures 18.1 KB in
practice. Against a 16,384-byte stack that overflows on every decoded frame:
the decoder's return addresses end up in heap memory, get overwritten, and the
next `ret` jumps to garbage.

32 KB leaves 45% headroom. The high-water mark now reports ~14.5 KB still free,
and the comment tells the next person to re-measure if minimp3 is ever updated.

### Agile traceability
- Epic: Playback stability
- Story: Decode an MP3 without crashing the core
- Acceptance: 90 s of continuous playback in QEMU with zero panics, and the
  logged stack high-water mark stays well clear of zero

---

## Change 7 — A dead I2S DMA could freeze the display forever

### Context
`i2s_channel_write()` was called with `portMAX_DELAY`. If the DMA never
completed, the audio task blocked indefinitely and the UI stopped updating with
no indication of why.

### Files changed
- [../../main/auris.c](../../main/auris.c)

### What changed
- Write timeout changed to `pdMS_TO_TICKS(1000)`.
- Added a dropped-block counter that logs on the first failure and every 100th
  after that.

### Why this mattered
On real hardware the DMA keeps up easily, so a timeout means something is
actually wrong. Dropping one block is then much better than stalling playback
forever. It also makes the QEMU limitation visible as data rather than as a
mystery: the ESP32-S3 QEMU model has no I2S peripheral, so every write times
out with `0x107` and the counter climbs honestly.

### Agile traceability
- Epic: Playback stability
- Story: Keep the UI responsive when audio output misbehaves
- Acceptance: A failing I2S write is counted and logged instead of blocking

---

## Change 8 — The PCM5102A master clock was missing

### Context
A PCM5102A DAC was selected for the earbud output. That chip will not produce
audio without its system clock on SCKI, and the firmware left MCLK unassigned.

### Files changed
- [../../main/auris.c](../../main/auris.c)

### What changed
- Added `I2S_MCLK_PIN GPIO_NUM_8` alongside the existing BCLK/WS/DOUT pins.
- `.mclk` changed from `I2S_GPIO_UNUSED` to that pin.
- Documented the clock ratios next to the config.

### Why this mattered
`i2s_channel_init_std_mode()` *silently* skips a pin left at `I2S_GPIO_UNUSED`
— no error, no warning. So the symptom would have been a perfectly clean boot
log followed by silence.

`I2S_STD_CLK_DEFAULT_CONFIG` already generates the right clock internally
(MCLK at 256 × Fs = 11.2896 MHz and BCLK at 64 × Fs = 2.8224 MHz at 44.1 kHz);
only the pin was missing. The PCM5102A's MODE pins strap it to expect SCKI at
128/192/256/384 × fS, and a mismatch shows up as wrong pitch or distortion
rather than as silence.

### Agile traceability
- Epic: Hardware audio output
- Story: Drive an external I2S DAC
- Acceptance: MCLK is routed to SCKI and the generated ratios are documented

---

## Change 9 — Output level and click suppression

### Context
The PCM was written at unity gain with no volume control and no fades. The test
track peaks at 32767 — 100% of full scale, 488 samples at the ceiling — straight
into a low-impedance earbud load.

### Files changed
- [../../main/auris.c](../../main/auris.c)

### What changed
- Added `AUDIO_GAIN_SHIFT` (set to 1, i.e. half scale) and `AUDIO_FADE_SAMPLES`
  (882, i.e. 20 ms at 44.1 kHz).
- Added `shape_output()`, which applies the gain and the ramps in place.
- Added `output_block()`, which shapes one block and writes it.
- The playback loop now holds one decoded block back so the end of the file is
  known *before* the last block is sent, which is what makes a fade-out possible
  at all.
- Extracted the write site (and its drop accounting) into `output_block()`.

### Why this mattered
An MP3 stream starts and stops at whatever sample value the encoder left, and
that step is an audible click in the analogue domain. Fading the end needs
lookahead: without it the final block has already left the DMA by the time
`mp3_decoder_read_pcm()` reports EOF, so there is nothing left to fade. The
lookahead costs 4,608 bytes of heap and about 26 ms of latency.

The arithmetic was verified on the host, because QEMU cannot route I2S
anywhere. That test caught two real bugs in the first attempt: both ramps
started and stopped at 1/882 of full scale instead of true silence, so the click
was still there. `shape_output()` is now covered by 19 checks that pass at gain
1/1, 1/2, 1/4 and 1/8.

### Agile traceability
- Epic: Hardware audio output
- Story: Listen to the output through earbuds without being startled
- Acceptance: Output gain is adjustable by a single constant, and playback
  starts and ends at zero amplitude

---

## Change 10 — File-info UI replaced the test pattern

### Context
The screen still showed the sprint 1 colour bars, which says nothing about what
the player is doing.

### Files changed
- [../../main/player_ui.c](../../main/player_ui.c)
- [../../main/player_ui.h](../../main/player_ui.h)
- [../../main/player_status.c](../../main/player_status.c)
- [../../main/player_status.h](../../main/player_status.h)
- [../../main/auris.c](../../main/auris.c)

### What changed
- New self-contained renderer with no new component dependencies (no LVGL).
- Drawn through the panel frame-buffer API
  (`esp_lcd_rgb_qemu_get_frame_buffer` + `esp_lcd_rgb_qemu_refresh`) with
  `.bpp = RGB_QEMU_BPP_16`, on a 480x480 RGB565 panel.
- Renders header, filename, path, codec, format, clock, progress bar,
  elapsed/total and percent, status message and footer.
- Added a mutex-guarded status snapshot shared between the audio and display
  tasks, published once per displayed second rather than once per frame.
- Dropped `mpeg_version` from the status struct; it is derived from the sample
  rate at draw time.

### Why this mattered
It turns the display into something that verifies playback: if the clock
advances and the bar fills, the decode path and the I2S path are both alive.
Keeping the renderer self-contained preserved the "no new component
dependencies" constraint from sprint 1.

### Agile traceability
- Epic: Playback visibility
- Story: Show what is currently playing
- Acceptance: The panel shows track metadata, a running clock, a progress bar
  and a percent, and all of it is readable back off the panel

---

## Change 11 — A real pixel font replaced the outline-derived one

### Context
The UI font made `0`, `O` and `D` ambiguous — `0` and `O` differed by a single
pixel and both read as a squared-off `D`, so `STEREO` looked like `STERED`.
Cap heights were also inconsistent between glyphs.

### Files changed
- [../../tools/gen_ui_font.py](../../tools/gen_ui_font.py)
- [../../main/include/ui_font7x8.h](../../main/include/ui_font7x8.h)

### What changed
- Rewrote the generator to consume the system console face at
  `/usr/share/kbd/consolefonts/alt-8x8.gz` (a raw 2048-byte CP437/VGA 8x8 blob).
- Emit a 7x8 cell with an 8 px advance instead of the previous 6x10 cell.
- Generator self-validates: glyph coverage, no duplicate glyphs, cap/x-height/
  descender classes, and widest-glyph gap.
- Regeneration is byte-identical.
- Deleted `ui_font6x10.h`.
- One glyph tweak: lowercase `l` loses its top serif so it cannot read as `I`.

### Why this mattered
The panel is 480x480 and glyphs are magnified 2x..6x, so an outline font
rasterised into a small cell turns to mud. A console face is hand-drawn on an
8x8 grid and stays crisp when magnified. This particular face also solves the
ambiguity structurally rather than by patching: `0` has a diagonal slash, `O` is
a smooth oval, `D` is flat-sided. Measured separation went from barely a pixel
or two to 16 px (`0`/`O`), 10 px (`O`/`D`) and 20 px (`0`/`D`).

The 7-wide cell drops the mostly-unused 8th column, which keeps the advance at
8 px so every existing UI string still fits at its existing scale; a full 8-wide
cell would push the times and percent line past the right margin. Vertical
metrics are unchanged from the old 6x10 cell — caps still occupy rows 0..6 — so
no layout constant in `player_ui.c` had to move.

### Agile traceability
- Epic: Playback visibility
- Story: Read the track information at a glance
- Acceptance: `0`, `O` and `D` are visually distinct at every UI scale, cap
  heights are consistent, and the text still fits the layout

---

## Change 12 — VBR bitrate and duration were both wrong

### Context
The UI showed `64 KBPS` and a total of `~14:54` for a track that is actually
VBR at about 233 kbps and 4:05 long. Both numbers came from the same mistake:
the first frame's bitrate.

### Files changed
- [../../main/decoder_mp3.h](../../main/decoder_mp3.h)
- [../../main/decode_mp3.c](../../main/decode_mp3.c)
- [../../main/player_status.h](../../main/player_status.h)
- [../../main/player_ui.c](../../main/player_ui.c)
- [../../main/auris.c](../../main/auris.c)

### What changed
- `decode_mp3.c` parses the Xing/Info header in the first frame: the MPEG
  version bits come from byte 1, the side-information length follows from the
  version and channel count, and the tag sits immediately after it.
- The frame count gives an **exact** duration. The byte count in the same header
  gives an **exact** whole-file average bitrate, available from the first block.
- Without a Xing header, a running average over decoded frames is used instead,
  held back until half a second of audio so encoder start-up does not skew it.
- New `mp3_decoder_total_seconds()` and `mp3_decoder_total_is_exact()`.
- `progress_percent()` is now time-based, falling back to bytes only when no
  duration is known.
- The UI's `~` marker now means "estimated" and is omitted for a counted
  duration. It previously meant "this is a CBR guess" and was always shown.

### Why this mattered
On a VBR track the first frame is just whatever bitrate the encoder happened to
start at. `file_size * 8 / first_frame_bitrate` then overestimates the duration
by 3.6x, which is what produced `~14:54` for a 4:05 file. Byte position is also
a poor progress proxy when frame sizes vary tenfold, so the bar jumped around.

Two bugs were found and fixed while implementing this. minimp3's return value is
samples *per channel*, so the first accounting pass divided by the channel count
a second time and produced a duration exactly half the truth. And truncating
rather than rounding the average made a 128 kbps CBR file display as 127.

### Agile traceability
- Epic: Playback visibility
- Story: Trust the numbers on the screen
- Acceptance: The displayed bitrate matches the file's average and the displayed
  total matches its real duration, for VBR and CBR files alike

---

## Change 13 — The decoder was validated offline against a reference

### Context
QEMU cannot route I2S to host audio, so the decode half of the pipeline had no
end-to-end test. `decode_mp3.c` has no ESP-IDF dependencies, which makes it
testable on the host.

### Files changed
- Host-only harnesses under `/tmp/opencode/` (not yet committed — see follow-ups)

### What changed
- Built a host harness that compiles the project's own `decode_mp3.c` and
  compares its output with ffmpeg on a synthetic file with a known L/R layout
  (440 Hz left, 1760 Hz right) and on the real test track.
- Added a second harness for the VBR metadata changes.

### Why this mattered
The synthetic tone test disproved an early hypothesis that minimp3 output was
planar and needed de-interleaving. It is already correctly interleaved stereo —
a distinction worth having settled before anyone "fixes" it.

Measured on `track.mp3`: 0.998 correlation per channel, 0.000 cross-channel,
identical duration (245.41 s), envelope correlation 0.99, log-spectrum
correlation above 0.999, and 9,397 calls each returning exactly 2304 samples
into a 2304-sample buffer with no overrun. Sample-exact equality is not expected
between MP3 decoder implementations.

### Agile traceability
- Epic: Verification
- Story: Prove the decode path is correct without hardware
- Acceptance: Decoder output correlates with a reference decoder on channel
  identity, duration and spectrum

---

## Change 14 — Flash layout, storage image and build wiring

### Context
The default 2 MB single-app layout had nowhere to put an MP3 file, and the
build did not know about the new source files.

### Files changed
- [../../partitions.csv](../../partitions.csv)
- [../../sdkconfig](../../sdkconfig)
- [../../main/CMakeLists.txt](../../main/CMakeLists.txt)

### What changed
- Flash size 2 MB → 16 MB, single-app table → custom
  [partitions.csv](../../partitions.csv) with an 8 MB FAT `storage` partition.
- `fatfs_create_spiflash_image(storage spiflash FLASH_IN_PROJECT)` generates the
  storage image at build time.
- Registered `player_ui.c`, `player_status.c` and `decode_mp3.c`, and added
  `include` to the include path so `minimp3.h` and the font header resolve.
- Added the test MP3 at [../../main/spiflash/track.mp3](../../main/spiflash/track.mp3).

### Why this mattered
The storage partition is what lets the player open a real file, and building
the FAT image at build time is what lets the whole thing be merged into one
16 MB image and run in QEMU without a flash programmer.

### Agile traceability
- Epic: Build and environment readiness
- Story: Give the firmware somewhere to read a file from
- Acceptance: The build produces a single flashable image containing the
  application and a FAT volume with the test track

---

## Overall status

### Done
- MP3 decoding via minimp3, output over I2S, file-info UI on a 480x480 panel
- Audio task stack overflow diagnosed from first principles and fixed
- I2S writes made non-blocking and instrumented
- Master clock routed for the PCM5102A
- Output gain and start/end fades
- Pixel console font with unambiguous `0` / `O` / `D`
- Correct VBR bitrate and duration reporting
- Decode path validated against ffmpeg off-target
- Build produces one merged 16 MB image (87% of the app partition free)

### In progress
- Hardware bring-up on a real ESP32-S3 with a PCM5102A and earbuds

### Risks / follow-up
- **Audio has never been heard.** QEMU models no I2S peripheral for the
  ESP32-S3, so the data path is verified off-target and the clock pin is
  unverified. MCLK on GPIO8 was chosen from the IOMUX group but could not be
  confirmed against a local datasheet.
- **The PCM5102A is a line-out DAC**, specified for 10 kΩ and above. Into
  16-32 Ω earbuds it will be quiet and thin unless the module carries a
  headphone buffer. This is a hardware selection question, not a firmware one.
- **Font licensing is unresolved.** The `kbd` console fonts are GPL-2; embedding
  a generated subset in the firmware affects how the firmware may be
  distributed. This needs a decision before any release.
- **The verification harnesses live in `/tmp/opencode/`** and are not in the
  repository. They are the regression suite for the decoder metadata and the
  gain/fade arithmetic and should be moved under `auris/tools/` with a way to
  run them.
- **`main/spiflash/track.mp3` is the user's test asset** and is treated as
  read-only. End-of-playback behaviour was verified by generating a separate
  short track into a throwaway flash image rather than by replacing it.
- A latent `max_samples` overrun in `mp3_decoder_read_pcm()` and the mono
  upmix path are both still unaddressed by agreement; neither is currently
  reachable in practice.

