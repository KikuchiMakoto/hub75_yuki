# HUB75 LED Matrix Controller System

128x32 HUB75 LEDマトリックスパネルを制御する完全なシステムです。RP2040 (Raspberry Pi Pico) とPythonアプリケーションで構成されています。

## プロジェクト構成

このプロジェクトは、**Application (Python)**、**Firmware**、**KiCad PCB** の3つのコンポーネントで構成されています：

```
├── application/       # Python制御アプリケーション (PC側)
│   └── src/           # LED Matrix Controller (画像/動画/カメラ/テキスト表示)
├── firmware/          # RP2040ファームウェア (Arduino core + PIO、COBSバイナリ通信)
│   └── src/           # HUB75ドライバ (PlatformIO/Arduino)
└── kicad_pcb/         # KiCad設計データ (基板レイアウト/ライブラリ)
```

### 役割分担

- **Firmware** (`firmware/`): RP2040上で動作するC++コード。HUB75パネルの駆動、PIO/DMAによる高速出力、USB CDCによるCOBSデータ受信、RGB565からBCM変換を担当。
- **Application (Python)** (`application/`): PC上で動作するPythonコード。画像/動画読み込み、リサイズ、RGB565変換、COBSエンコード、シリアル通信を担当。
- **KiCad PCB** (`kicad_pcb/`): 回路図シンボル/フットプリント/基板レイアウトなどのハードウェア設計データを管理。

### 通信プロトコル（絶対的ルール）

すべてのファームウェア・クライアント間で、以下のプロトコルは**絶対的な不変条件**です：

- **インターフェース**: USB CDC ACM
- **ペイロード形式**: RGB565 リトルエンディアン
- **エンコーディング**: COBS (Consistent Overhead Byte Stuffing)
- **フレーム区切り**: 末尾 `0x00`
- **総画素数 / ペイロード長**: **4,096 画素（8,192 バイト）**

### 動作モードとパネル配置（64x32 パネル 2枚構成）

本システムは、64x32 HUB75 パネル 2枚の物理配置と用途に応じて、以下の **2つの公式動作モード** を備えています：

1. **NormalMode (128x32)**:
   - **配置**: 64×32 パネルを左右に連結（横長バナー配置 / デフォルト）
   - **動作**: PC 上の Python コントローラー（`application/`）から USB CDC 経由で COBS 符号化 RGB565 フレームを受信し、Core1 が高速リフレッシュ駆動。動画、画像、テキスト、時計、デモアニメーションの再生に対応。
2. **StandaloneMode (64x64)**:
   - **配置**: 64×32 パネルを上下に積層（正方形 64×64 配置。下段の2枚目パネルはリボン配線に合わせて **180度回転マッピング** されます）
   - **動作**: PC 不要の完全スタンドアローン動作。RP2040 Core0（250MHz）が ADXL335 加速度センサ（`GP26` X / `GP27` Y）を読み取り、純粋な `float` (f32) 2D-DEM 珪砂シミュレーション（Spatial Grid 近傍探索 $O(N)$ により 30+ FPS）をリアルタイム計算。Core1 が 64×64 パネルを駆動する「電子珪砂時計 / 回転ドラム」モード。起動時の突入電流を抑える省電力ブート（即時ブラックアウト）を搭載。

> **更新レート（FPS）について**: これは**緩いルール**です。目標FPSはファームウェアの実装・最適化状況次第で変化します。クライアント側はできる限りのレートで送信し、ファームウェア側が受信・描画可能なタイミングで処理します。プロトコル形式（RGB565+COBS+0x00）を守ることが最優先です。

## 特徴

- **複数入力対応**: 画像、動画、デモアニメーション、カメラ入力
- **高速表示**: PIO (Programmable I/O) + DMAによる高速シフト出力
- **デュアルコア設計**: Core0でUSB受信+BCM変換、Core1でパネル駆動のみ（フリッカー防止）
- **RGB565フォーマット**: 16ビットカラー（5-6-5ビット）による効率的な転送
- **COBSエンコーディング**: 0x00区切りのバイナリフレーム転送
- **高画質**: 6ビットBCM（64階調）+ Gamma補正（2.2）
- **柔軟なビルド**: PIOモードとCPU GPIOモードの切り替え可能
- **128x32/128x64対応**: ビルドフラグでパネルサイズ変更可能
- **簡単セットアップ**: PlatformIOとuvで簡単にビルド・実行

## クイックスタート

### 必要なツール

- **Firmware用**: [PlatformIO](https://platformio.org/) - RP2040へのファームウェア書き込み
- **Application (Python) 用**: [uv](https://docs.astral.sh/uv/) - Pythonパッケージマネージャー

### セットアップ手順

#### 1. Firmwareをビルド・書き込み (初回のみ)

```bash
cd firmware
pio run -t upload
```

RP2040にファームウェアが書き込まれ、HUB75パネルの駆動が可能になります。

#### 2. Application (Python) を実行 (PC側)

uvをインストール:
```bash
# macOS / Linux
curl -LsSf https://astral.sh/uv/install.sh | sh

# Windows (PowerShell)
powershell -ExecutionPolicy ByPass -c "irm https://astral.sh/uv/install.ps1 | iex"
```

アプリケーションを実行:
```bash
cd application

# デモアニメーション
uv run led-matrix --demo rainbow

# 画像表示
uv run led-matrix --image sample.png

# 2D-DEM 珪砂シミュレーション (Taichi GPU, f32)
uv run led-matrix --dem

# 2D-DEM 64x64 Pixel/Voxel マトリクス表示
uv run led-matrix --dem-matrix

# ヘルプ表示
uv run led-matrix --help
```

詳細は各ディレクトリのREADMEを参照してください:
- [Application (Python) README](application/README.md)
- [Firmware README](firmware/README.md)
- [KiCad PCB データ](kicad_pcb/)

## ハードウェア要件

- Raspberry Pi Pico (RP2040)
- HUB75 LED パネル 64x32 x 2枚
- 3軸加速度センサ ADXL335 (3.3V電源)
  - **X軸**: `GP26` (ADC0)
  - **Y軸**: `GP27` (ADC1)
  - **Z軸**: 未使用（2D DEMのため不要）
  - **VCC**: `3V3` (3.3V)
  - **GND**: `GND`
- USBケーブル
- 5V電源 (LEDパネル用)
