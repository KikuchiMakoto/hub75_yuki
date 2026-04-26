# firmware_v2 (pico-sdk + TinyUSB)

`firmware_v2` is a full pico-sdk rebuild that keeps the existing wire protocol unchanged:

- USB CDC ACM
- COBS encoded payload
- `0x00` frame delimiter
- RGB565 little-endian frame payload (`128x32x2 = 8192 bytes`)

## Architecture

- Core0: TinyUSB task service, CDC bulk reads, stream parser, packet-queue publish only
- Core1: HUB75 refresh + COBS decode + RGB565->BCM conversion (heavy path moved to Core1)
- Frame swap: triple BCM buffer handoff with latest-wins pending update (new frame can replace older pending frame)
- Frame update limiter: panel content swap is capped to about `180 fps` (`MAX_FRAME_UPDATE_FPS`) to avoid over-fast catch-up when packets backlog
- USB ingest path uses Core0 double-buffer blocks (SPSC) to avoid ring write blocking
- Runtime counters include bottleneck diagnostics: `rx_Bps`, `usb_drop_Bps`, `dec_fps`, `disp_fps`, `scan_fps`, `drop_ps`, `swap_replace`, `ovf_drop`, `qovf_drop`, `usb_hw`, `pkt_hw`, `tud_gap_us`, `dec_us`, `conv_us`, `dma_to`

Current implementation targets `128x32` default operation and keeps HUB75 scan on `pio0/sm0` + single display DMA channel.

## Build (Windows PowerShell)

```powershell
$env:PICO_SDK_PATH = "C:\path\to\pico-sdk"
cmake -S firmware_v2 -B firmware_v2/build -G Ninja
cmake --build firmware_v2/build -j
```

Clock and update cap tuning (default is now `CPU_CLOCK_KHZ=200000`):

```powershell
cmake -S firmware_v2 -B firmware_v2/build_oc -G Ninja -DCPU_CLOCK_KHZ=250000 -DMAX_FRAME_UPDATE_FPS=180
cmake --build firmware_v2/build_oc -j
```

Artifacts are generated under `firmware_v2/build` (`.uf2`, `.elf`, `.bin`, `.hex`).

## Build (PlatformIO + pico-sdk)

`firmware_v2/platformio.ini` is provided for PlatformIO pico-sdk builds.

```powershell
cd firmware_v2
pio run -e pico_picosdk
pio run -t upload -e pico_picosdk
```

Validation-oriented profiles:

```powershell
pio run -e pico_picosdk_debugsafe
pio run -e pico_picosdk_fastcheck
```

Notes for PlatformIO build:

- Uses `framework = picosdk` from `https://github.com/maxgerhardt/platform-raspberrypi.git`.
- `platformio_picosdk_tinyusb.py` forces TinyUSB + required Pico SDK components so `tud_*` API and CDC descriptors are available.
- `usb_descriptors.c` remains enabled and provides the CDC descriptor set used by firmware protocol.
- `CFG_TUSB_CONFIG_FILE` is set to `tusb_config.h` so CDC buffer settings remain project-controlled.
- Build flags are speed-oriented (`-Ofast`, frame-pointer/unwind disabled) and `PIO_STDIO_NONE` is used to disable unused stdio backends.
- TinyUSB build is reduced to CDC-device-only source set (host and other classes are excluded).
- `pico_picosdk_debugsafe` enables stronger warnings and debug-oriented checks (`-O2`, `-g3`, stack protector, warning set).
- `pico_picosdk_fastcheck` keeps fast optimization while adding extra warning checks for overflow/bounds-risk patterns.

## Repository Hygiene

- `firmware_v2/.gitignore` excludes build artifacts (`.pio/`, `build/`) and Python cache files.
- `firmware_v2/src/hub75.pio.h` is intentionally tracked because this PlatformIO + pico-sdk path may not always auto-generate a usable header from `hub75.pio`.
- Keep runtime/generated outputs out of version control; only commit source, build scripts, and docs.

## Throughput/Performance Benchmark

`application_py/src/led_matrix_controller/controller.py:29` sets `MAX_VIDEO_FPS = 18`, so the app path is not suitable for measuring firmware ceiling.

Use the dedicated benchmark sender that bypasses this cap:

```bash
python firmware_v2/tools/bench_stream.py --port COM5 --fps 140 --duration 30
```

To reduce host-side Python generation overhead and measure firmware-side limits more cleanly, use pre-encoded frame cache:

```bash
python firmware_v2/tools/bench_stream.py --port COM5 --fps 145 --duration 30 --cache-frames 64
```

Example output line:

```text
HOST tx_Bps=1184200 send_fps=144 send_err=0 | STAT clk_khz=200000 rx_Bps=1179000 usb_drop_Bps=0 dec_fps=141 disp_fps=140 scan_fps=140 drop_ps=0 drop=0 swap_replace=0 ovf_drop=0 qovf_drop=0 cobs=0 usb_hw=2048 pkt_hw=2 tud_gap_us=180 dec_us=220 conv_us=1420 dma_to_ps=0 dma_to=0
```

This gives host transmit rate and firmware-side decode/display rates independently.

## Overclock Measurement Procedure

1. Build and flash baseline firmware (`CPU_CLOCK_KHZ=200000`) and run benchmark.
2. Build and flash overclock firmware (`CPU_CLOCK_KHZ=250000` or your stable value) and run the same benchmark command.
3. Compare `disp_fps`, `drop_ps`, `usb_drop_Bps`, `tud_gap_us`, `dec_us`, `conv_us`.

Recommended fixed workload for comparison:

```bash
python firmware_v2/tools/bench_stream.py --port COM5 --fps 145 --duration 30
```

## Notes

- USB Full-Speed practical ceiling remains below 180 fps for this frame format.
- Current target is stable operation near USB FS practical limit (roughly 120-145 fps range).
