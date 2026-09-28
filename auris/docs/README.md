# Agile project documentation

This folder records the work completed for the Auris project in an Agile-friendly format.

## Scope

Sprint 1 validated the ESP32 display path with a colour-bar test pattern.
Sprint 2 replaced it with an MP3 player: decode a file, drive an external I2S
DAC, and show what is playing. The player is complete and verified in
simulation; it has not yet been heard on real hardware.

## Documentation set

- [agile/implementation-log.md](agile/implementation-log.md) — detailed change log and Agile traceability
- [agile/sprint-summary.md](agile/sprint-summary.md) — sprint goals, acceptance criteria, evidence, and next steps

## Project status

- Status: In progress — software complete, blocked on hardware bring-up
- Current sprint focus: PCM5102A and earbud output on a real ESP32-S3
- Primary evidence: playback runs in QEMU with zero panics, the panel reads back
  with no unmatched glyphs, and the displayed bitrate and duration match ffprobe
  for the test file

## Agile view

### Epics

| Epic | State |
| --- | --- |
| Display validation | Done (sprint 1) |
| Build and environment readiness | Done |
| Playback stability | Done |
| Playback visibility | Done |
| Hardware audio output | In progress — firmware ready, hardware unverified |
| Verification | Done off-target, pending an on-target audio check |

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

### Acceptance criteria

Playback:

- 90 s of continuous playback with zero panics, and the logged stack high-water
  mark stays well clear of zero.
- A failing I2S write is counted and logged instead of blocking the task.
- The build produces a single flashable image containing the application and a
  FAT volume with the test track.

Audio output:

- MCLK is routed to the DAC's SCKI pin and the generated clock ratios are
  documented.
- Output gain is adjustable by a single constant, and playback starts and ends
  at zero amplitude.

Display:

- The panel shows track metadata, a running clock, a progress bar and a percent.
- `0`, `O` and `D` are visually distinct at every UI scale, and cap heights are
  consistent.
- The displayed bitrate matches the file's average and the displayed total
  matches its real duration, for VBR and CBR files alike.

Verification:

- Decoder output correlates with a reference decoder on channel identity,
  duration and spectrum.
- Sound comes out of the earbuds on the target board. **Not yet met.**

## Known risks

- **Audio has never been heard.** QEMU models no I2S peripheral for the
  ESP32-S3, so the data path is verified off-target and the clock pin is
  unverified.
- **The PCM5102A is a line-out DAC**, specified for 10 kΩ and above. Into
  16-32 Ω earbuds it will be quiet and thin unless the module carries a
  headphone buffer.
- **Font licensing is unresolved.** The `kbd` console fonts are GPL-2, and
  embedding a generated subset affects how the firmware may be distributed.
  This needs a decision before any release.
- **The verification harnesses are not in the repository** — they live in
  `/tmp/opencode/` and should be moved under `auris/tools/`.
