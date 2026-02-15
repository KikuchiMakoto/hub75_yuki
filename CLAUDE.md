# CLAUDE.md - AI Assistant Guide for hub75_yuki

## Project Overview

HUB75 LED matrix panel controller system for 128x32 RGB displays (two chained 64x32 HUB75 panels) driven by a Raspberry Pi Pico (RP2040). The system has three components:

1. **Firmware** (`firmware/`) - C++ running on RP2040, drives the LED panels via PIO+DMA
2. **Python Application** (`application/`) - Desktop CLI for sending images, video, demos to the display
3. **Web Application** (`web_application/`) - Browser-based controller using Web Serial API

## Repository Structure

```
firmware/                    # RP2040 firmware (C++ / Arduino / PlatformIO)
  platformio.ini             # Build config with 3 environments: pico, pico_gpio, pico_64
  src/main.cpp               # All firmware logic (~530 lines)
  include/
    hub75_config.h           # Pin mapping and display dimensions
    hub75.pio.h              # PIO state machine program
    tusb_config.h            # TinyUSB CDC buffer configuration

application/                 # Python desktop application
  pyproject.toml             # Package config (hatchling build, uv runner)
  src/led_matrix_controller/
    main.py                  # CLI entry point and argument parsing
    controller.py            # Core logic: image processing, demos, COBS encoding
    devices/
      base.py                # Abstract device interface
      serial_device.py       # USB CDC serial communication
      simulator.py           # Terminal and image file output for testing

web_application/             # TypeScript/React browser app
  package.json               # Bun + Vite + React 19 + Tailwind CSS 4
  vite.config.ts             # Vite build configuration
  src/
    App.tsx                  # Main React component
    components/              # DropZone, DemoSelector
    lib/
      serial.ts             # Web Serial API wrapper + RGB565 + COBS
      media.ts              # Image/video loading with letterbox scaling
      demos.ts              # Procedural animations (rainbow, plasma, fire, etc.)
      videoProcessor.ts     # ffmpeg.wasm in-browser video transcoding
      cobs.ts               # COBS encoder (TypeScript)
    types/index.ts           # Shared TypeScript types

.github/workflows/
  deploy-web.yml             # GitHub Actions: build web app, deploy to GitHub Pages
```

## Build Commands

### Firmware (PlatformIO)

```bash
# Build (default PIO mode - recommended)
cd firmware && pio run -e pico

# Build with GPIO bit-banging fallback
cd firmware && pio run -e pico_gpio

# Build for 128x64 panel
cd firmware && pio run -e pico_64

# Upload to connected Pico
cd firmware && pio run -t upload -e pico

# Serial monitor
cd firmware && pio device monitor
```

Requires: PlatformIO CLI. Uses Earle Philhower's Arduino core for RP2040.

### Python Application (uv)

```bash
cd application

# Install and run
uv run led-matrix --help

# Run with specific input
uv run led-matrix --image photo.png
uv run led-matrix --demo rainbow
uv run led-matrix --video clip.mp4
uv run led-matrix --camera

# Run against terminal simulator (no hardware needed)
uv run led-matrix --device terminal --demo plasma
```

Requires: Python >=3.9, `uv` package manager.

### Web Application (Bun + Vite)

```bash
cd web_application

# Install dependencies
bun install

# Development server (localhost:5173)
bun run dev

# Production build (outputs to dist/)
bun run build

# Preview production build
bun run preview
```

Requires: Bun runtime.

## Linting and Formatting

### Python

Dev dependencies include `black`, `ruff`, and `pytest` (in `[project.optional-dependencies] dev`).

```bash
cd application

# Format
uv run black src/

# Lint
uv run ruff check src/

# Lint with auto-fix
uv run ruff check --fix src/
```

Configuration in `pyproject.toml`:
- Line length: 100
- Target: Python 3.9
- Ruff rules: E, F, W (pycodestyle errors/warnings + pyflakes)

### Web Application

No explicit linter configured. TypeScript compiler provides type checking:

```bash
cd web_application
bunx tsc --noEmit
```

## Testing

No formal test suites exist currently. The Python `pyproject.toml` lists `pytest` as a dev dependency but no tests are written. Testing is done manually via:

- Terminal simulator device (`--device terminal`) for visual verification
- Image output device (`--device image`) for frame capture
- Direct hardware testing with the LED panel

## CI/CD

Single GitHub Actions workflow (`.github/workflows/deploy-web.yml`):
- **Triggers**: Push to `main`/`master` (when `web_application/**` changes), or manual dispatch
- **Steps**: Checkout -> Setup Bun -> `bun install` -> `bun run build` -> Deploy to GitHub Pages
- No CI for firmware or Python application

## Key Technical Concepts

### Communication Protocol

All three components (firmware, Python app, web app) implement the same protocol:

1. **Pixel format**: RGB565 (2 bytes/pixel, 5-bit red, 6-bit green, 5-bit blue)
2. **Frame size**: 128 x 32 x 2 = 8,192 bytes raw
3. **Framing**: COBS (Consistent Overhead Byte Stuffing) encoding + `0x00` delimiter
4. **Transport**: USB CDC serial at 115200 baud (rate irrelevant for USB CDC)
5. **Flow control**: Firmware responds `'K'` (ACK) or `'E'` (NAK) after each frame

COBS is implemented three times (must stay in sync):
- `firmware/src/main.cpp` - C++ decoder
- `application/src/led_matrix_controller/controller.py` - Python encoder
- `web_application/src/lib/cobs.ts` - TypeScript encoder

### Firmware Architecture

- **Core 0**: USB CDC reception + COBS decoding + RGB565-to-BCM conversion
- **Core 1**: Display refresh loop only (isolated for flicker-free output)
- **BCM (Binary Code Modulation)**: 6-bit color depth (64 levels per channel)
- **PIO**: Hardware state machine shifts pixel data with clock side-set
- **DMA**: Double-buffered transfers overlap data prep with transmission
- **Gamma correction**: 2.2 gamma lookup table

### Display Hardware

- **Panel**: HUB75 128x32 (two 64x32 chained), 16-scan
- **MCU**: RP2040 (Raspberry Pi Pico), dual Cortex-M0+ @ 125MHz
- **GPIO pins**: GP0-5 (RGB data), GP6 (CLK), GP7 (LAT), GP8 (OE), GP9-12 (row address A-D)
- **Color depth**: 6-bit BCM = 262,144 colors
- **Max FPS**: ~18 FPS (empirical USB throughput limit)

## Code Conventions

- **Firmware**: Single-file C++ (`main.cpp`) with config headers. Uses Arduino framework APIs alongside raw RP2040 SDK (PIO, DMA, multicore).
- **Python**: Package structure under `src/led_matrix_controller/`. Uses abstract base classes for device interface. Line length 100, formatted with `black`, linted with `ruff`.
- **Web**: React 19 with functional components and hooks. Tailwind CSS 4 for styling. TypeScript strict mode. Vite for bundling.

## Important Notes for AI Assistants

- The three COBS implementations (C++, Python, TypeScript) must produce identical encoding/decoding behavior. Changes to the protocol require updating all three.
- The firmware uses both Arduino framework (`Serial`, `delay`) and raw RP2040 SDK (`pio_*`, `dma_*`, `multicore_*`). Both APIs coexist via the Earle Philhower core.
- The `tusb_config.h` CDC buffer sizes (16KB RX) are tuned for full-frame reception. Changing frame size requires updating these buffers.
- Display dimensions are compile-time constants in `hub75_config.h` (128x32 default, 128x64 via `pico_64` env).
- The web app uses `@ffmpeg/ffmpeg` (ffmpeg.wasm) for in-browser video preprocessing - this is a large dependency loaded on demand.
- Web Serial API requires Chrome/Edge/Opera 89+. Firefox and Safari are not supported.
