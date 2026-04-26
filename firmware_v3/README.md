# firmware_v3 — HUB75 LED Matrix Controller (v3)

## Overview

`firmware_v3` is a high-performance RP2040 firmware for HUB75 LED matrix panels, targeting **9-bit native BCM (512 levels per colour)** at **60 fps** with **maximum PIO/DMA efficiency**.

## Key Features

- **9-bit Native BCM**: True 512-level PWM per colour channel (R/G/B).
- **5 Pixels / 32-bit Word**: PIO FIFO utilisation increased from 18.75% to **93.75%**.
- **DMA + PIO Pipeline**: Core1 is display-only; Core0 handles USB reception and frame conversion.
- **Triple Buffering**: Eliminates frame-tearing and display stutter.
- **All PIO0 SMs Utilised**:
  - **SM0**: High-efficiency data shifter (5px/word)
  - **SM1**: Automatic LAT+OE timing generator
  - **SM2**: Row address output (future expansion)
  - **SM3**: Sequencer / frame boundary (future expansion)
- **Protocol Compatibility**: COBS + RGB565 little-endian + `0x00` delimiter (unchanged from v1/v2).

## Architecture

| Component | Responsibility |
|-----------|---------------|
| **Core0** | USB CDC receive → COBS decode → RGB565 → 9-bit BCM conversion → triple buffer management |
| **Core1** | Display refresh ONLY (DMA-driven PIO, minimal CPU intervention) |
| **DMA CH0** | Row data → PIO0 SM0 TX FIFO |
| **DMA CH1** | OE duration → PIO0 SM1 TX FIFO |
| **DMA CH2-3** | Reserved for future expansion |

## Display Parameters

| Parameter | Value |
|-----------|-------|
| Panel size | 128×32 (configurable to 128×64) |
| Scan rows | 16 (32 for 64-row panels) |
| Colour depth | **9-bit native** (512 levels) |
| Frame rate target | **60 fps** |
| System clock | 200 MHz |
| PIO FIFO efficiency | **93.75%** |

## Build

### PlatformIO

```bash
cd firmware_v3
pio run -e pico_picosdk
```

### CMake (pico-sdk)

```bash
cd firmware_v3
set PICO_SDK_PATH=C:\path\to\pico-sdk
cmake -S . -B build -G Ninja
cmake --build build -j
```

## Important Notes

### `hub75.pio.h` — Manual Pre-generation

`src/hub75.pio.h` is **manually written** with estimated PIO instruction encodings. For guaranteed correctness, regenerate it using the SDK's `pioasm`:

```bash
pioasm src/hub75.pio src/hub75.pio.h
```

If you have `pico_generate_pio_header` enabled in `CMakeLists.txt`, it will be regenerated automatically at build time.

### PIO Instruction Encodings

The following encodings in `hub75.pio.h` are **best-effort estimates** and should be verified with `pioasm`:

- `out pins, 6 side 0 [3]` → `0x4306`
- `nop side 1 [3]` (mov y,y) → `0xb322`
- `jmp x--, <addr> side 0` → `0x2024`
- `irq set 0 side 0` → `0xc000`

## Protocol

Identical to `firmware` and `firmware_v2`:

1. Host sends **RGB565 little-endian** frame data (128×32 = 8,192 bytes).
2. Frame is **COBS-encoded**.
3. Packet is terminated with **`0x00`**.
4. Device decodes COBS, converts to 9-bit BCM, and displays.

## Performance

| Metric | Value |
|--------|-------|
| Theoretical max fps (9-bit) | ~122 fps |
| Target fps | **60 fps** (50% safety margin) |
| DMA words per row | 27 (was 128 in v2) |
| CPU overhead per row | ~200 ns (DMA setup only) |
| Frame buffer memory | ~45 KB (3× triple buffer) |

## Future Expansion (PIO1)

PIO1's 4 SMs are reserved for:
- Second panel data shifter (dual-panel setup)
- GSCLK generation (external PWM driver)
- Debug pulse / frame sync output
- Status LED

## Licence

Same as parent project.
