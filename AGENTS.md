# AGENTS.md
This guide is for agentic coding assistants operating in this repository.

## 1) Repository Map
- `application/`: Python CLI sender/controller (`uv` managed).
- `firmware/`: RP2040 firmware (PlatformIO + Arduino core + PIO).
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
- Embedded build system: `PlatformIO` (`pio`).

## 4) Build / Lint / Test Commands
Run commands from each subproject directory unless noted.

### application (Python)
Install (with dev deps):
```bash
cd application
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
- `application/src/led_matrix_controller/*`
- `firmware/src/main.cpp`

### 5.2 Python conventions (`application/`)
- Formatting: Black (`line-length = 100`).
- Lint baseline: Ruff rules (`E`, `F`, `W`).
- Import order: stdlib -> third-party -> local modules.
- Naming: `snake_case` vars/functions, `PascalCase` classes, `UPPER_CASE` constants.
- Types: add type hints for public APIs and non-trivial helpers.
- Error handling: raise explicit exceptions in lower layers; handle user-facing errors in CLI entrypoints.

### 5.3 C/C++ conventions (`firmware/`)
- Include order: standard -> SDK/hardware -> project headers.
- Naming: `snake_case` functions, `UPPER_CASE` macros, `g_` prefix for shared globals.
- Use fixed-width integer types (`uint8_t`, `uint16_t`, `uint32_t`, etc.).
- Keep hot paths allocation-free and explicit about timing side effects.
- Use compile-time guards (`_Static_assert`) for buffer/config assumptions.
- On malformed input, fail closed (drop/resync); avoid undefined state propagation.
- Keep DMA/PIO/IRQ sequencing explicit and conservative.

### 5.4 Formatting, comments, and hygiene
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
