# Agile project documentation

This folder records the work completed for the Auris project in an Agile-friendly format.

## Scope

The current work focuses on validating the ESP32 display path and setting up the project so the firmware can render a graphics test pattern on a virtual RGB LCD panel.

## Documentation set

- [agile/implementation-log.md](agile/implementation-log.md) — detailed change log and Agile traceability
- [agile/sprint-summary.md](agile/sprint-summary.md) — sprint summary, status, and next steps

## Project status

- Status: In progress
- Current sprint focus: display validation and board integration groundwork
- Primary evidence: the project now includes a virtual LCD implementation in [../main/auris.c](../main/auris.c) and a dependency entry in [../main/idf_component.yml](../main/idf_component.yml)

## Agile view

### Epic

Display output proves the firmware and display pipeline are working before hardware integration.

### User stories

1. As a developer, I want a simple RGB display output so I can validate the ESP32 LCD path.
2. As a developer, I want the project to build with the required LCD component dependency.
3. As a developer, I want the IDE and SDK configuration aligned with the current board setup.
4. As a project lead, I want a record of completed work and open follow-up items.

### Acceptance criteria

- Firmware creates a virtual RGB LCD panel and renders a test pattern.
- Project dependencies are declared in the component manifest.
- IDE config points to the local ESP-IDF toolchain and build folder.
- The change history is recorded and traceable.
