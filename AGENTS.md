# AGENTS.md
This guide is for agentic coding assistants operating in this repository.

## 1) Repository Map
- `application_py/`: Python CLI sender/controller (`uv` managed).
- `application_web/`: React + TypeScript + Bun + Vite.
- `firmware/`: RP2040 firmware (PlatformIO + Arduino core). 現在もっとも安定した実装。
- `firmware_v2/`: RP2040 firmware (PlatformIO + pico-sdk + TinyUSB). WIP だが動作する可能性が高い。
- `firmware_v3/`: RP2040 firmware (PlatformIO + pico-sdk + TinyUSB). WIP だが動作する可能性が高い。
- `kicad_pcb/`: hardware design assets (not software build targets).

## 2) Cursor / Copilot Rule Files
Checked these paths:
- `.cursor/rules/`
- `.cursorrules`
- `.github/copilot-instructions.md`

Current status: none are present.
If added later, treat them as higher-priority repository-local instructions.

## 3) Tooling Baseline
- Python env/pkg manager: `uv`.
- JS/TS runtime/pkg manager: `bun`.
- Embedded build system: `PlatformIO` (`pio`).
- `firmware_v2` additionally supports `cmake` + pico-sdk builds.

## 4) Build / Lint / Test Commands
Run commands from each subproject directory unless noted.

### application_py (Python)
Install (with dev deps):
```bash
cd application_py
uv sync --extra dev
```
Run app:
```bash
uv run led-matrix --help
uv run led-matrix --demo rainbow
uv run led-matrix --image sample.png
```
Lint/format:
```bash
uv run ruff check .
uv run black --check .
uv run black .
```
Tests:
```bash
uv run pytest -q
```
Run a single test:
```bash
uv run pytest tests/test_file.py::test_name -q
```

### application_web (Bun + React + TypeScript)
Install and start dev server:
```bash
cd application_web
bun install
bun run dev
```
Build/preview/typecheck:
```bash
bun run build
bun run preview
bunx tsc --noEmit
```
Tests:
- No test script currently exists in `application_web/package.json`.
- If tests are added, add a script and document single-test invocation.

### firmware (RP2040 Arduino + PlatformIO)
Build:
```bash
cd firmware
pio run -e pico
pio run -e pico_gpio
pio run -e pico_64
```
Upload/monitor:
```bash
pio run -t upload -e pico
pio device monitor
```

### firmware_v2 (RP2040 pico-sdk + TinyUSB)
PlatformIO profiles:
```bash
cd firmware_v2
pio run -e pico_picosdk
pio run -e pico_picosdk_debugsafe
pio run -e pico_picosdk_fastcheck
```
Upload:
```bash
pio run -t upload -e pico_picosdk
```
CMake alternative:
```bash
set PICO_SDK_PATH=C:\path\to\pico-sdk
cmake -S . -B build -G Ninja
cmake --build build -j
```
Focused runtime smoke check:
```bash
python tools/bench_stream.py --port COM5 --fps 145 --duration 10 --cache-frames 64
```

### firmware_ch32v305
- Treat current tree as artifacts unless source files are added.

## 5) Code Style Guidelines

### 5.1 Protocol invariants (cross-project, critical)
以下のワイヤーレベル条件は**絶対的な不変条件**です。明示的な要求がない限り、これらを変更しないでください：

- **インターフェース**: USB CDC ACM
- **ペイロード形式**: RGB565 リトルエンディアン
- **エンコーディング**: COBS (Consistent Overhead Byte Stuffing)
- **フレーム区切り**: 末尾 `0x00`
- **デフォルト解像度**: `128x32`

> **更新レート（FPS）について**: これは**緩いルール**です。目標FPSはファームウェアの実装・最適化状況次第で変化します。クライアント側はできる限りのレートで送信し、ファームウェア側が受信・描画可能なタイミングで処理します。プロトコル形式（RGB565 + COBS + 0x00）を守ることが最優先です。

プロトコル動作を変更する場合、以下の関連パスをすべて同時に更新してください：
- `application_py/src/led_matrix_controller/*`
- `application_web/src/lib/{cobs,serial,media}.ts`
- `firmware/src/main.cpp`
- `firmware_v2/src/{main.c,cobs.c}`
- `firmware_v3/src/{main.c,cobs.c}`

### 5.2 Python conventions (`application_py/`)
- Formatting: Black (`line-length = 100`).
- Lint baseline: Ruff rules (`E`, `F`, `W`).
- Import order: stdlib -> third-party -> local modules.
- Naming: `snake_case` vars/functions, `PascalCase` classes, `UPPER_CASE` constants.
- Types: add type hints for public APIs and non-trivial helpers.
- Error handling: raise explicit exceptions in lower layers; handle user-facing errors in CLI entrypoints.

### 5.3 TypeScript/React conventions (`application_web/`)
- Keep strict typing behavior (`strict: true`).
- Prefer explicit return types on exported functions/classes.
- Import order: external -> internal -> `import type`.
- Naming: `camelCase` values/functions, `PascalCase` components/types, `UPPER_CASE` constants.
- Prefer narrow union/literal types over broad `string` when practical.
- Wrap async UI operations in `try/catch`; surface actionable status/error messages.

### 5.4 C/C++ conventions (`firmware/`, `firmware_v2/`)
- Include order: standard -> SDK/hardware -> project headers.
- Naming: `snake_case` functions, `UPPER_CASE` macros, `g_` prefix for shared globals.
- Use fixed-width integer types (`uint8_t`, `uint16_t`, `uint32_t`, etc.).
- Keep hot paths allocation-free and explicit about timing side effects.
- Use compile-time guards (`_Static_assert`) for buffer/config assumptions.
- On malformed input, fail closed (drop/resync); avoid undefined state propagation.
- Keep DMA/PIO/IRQ sequencing explicit and conservative.

### 5.5 Formatting, comments, and hygiene
- Match existing file style; avoid unrelated reformatting.
- Add comments only for non-obvious hardware/protocol constraints.
- Keep logs short and diagnostic; avoid heavy logs in tight loops.
- Do not commit generated outputs (`.pio/`, `build/`, `dist/`, `node_modules/`, caches).

## 6) Agent Completion Checklist
- Build every subproject you changed.
- Run lint/typecheck where available.
- Run tests if present; otherwise run one focused smoke check.
- For protocol-path edits, run end-to-end send/display sanity validation.
- Update docs when commands, behavior, or build profiles change.
