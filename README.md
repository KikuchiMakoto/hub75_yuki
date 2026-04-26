# HUB75 LED Matrix Controller System

128x32 HUB75 LEDマトリックスパネルを制御する完全なシステムです。RP2040 (Raspberry Pi Pico) とPythonアプリケーションで構成されています。

## プロジェクト構成

このプロジェクトは、**Application**、**Web Application**、**Firmware**、**KiCad PCB** の4つのコンポーネントで構成されています：

```
├── application/        # Python制御アプリケーション (PC側)
│   └── src/           # LED Matrix Controller (画像/動画/カメラ/テキスト表示)
├── web_application/   # Webアプリケーション (ブラウザ側)
│   └── src/           # React + TypeScript (Web Serial API対応)
├── firmware/          # RP2040ファームウェア (Arduino core、安定動作)
│   └── src/           # HUB75ドライバ (PlatformIO/Arduino)
├── firmware_v2/       # RP2040ファームウェア (pico-sdk + TinyUSB、WIPだが動作可能性高)
│   └── src/
├── firmware_v3/       # RP2040ファームウェア (pico-sdk + TinyUSB、WIPだが動作可能性高)
│   └── src/
└── kicad_pcb/         # KiCad設計データ (基板レイアウト/ライブラリ)
```

### 役割分担

- **Firmware** (`firmware/`): RP2040上で動作するC++コード。HUB75パネルの駆動、PIO/DMAによる高速出力、USB CDCによるCOBSデータ受信、RGB565からBCM変換を担当。現在もっとも安定した実装
- **Firmware v2** (`firmware_v2/`): pico-sdk + TinyUSBベース。最適化されたLED表示fps向上機能改善版
- **Firmware v3** (`firmware_v3/`): pico-sdk + TinyUSBベース。9bit native BCM高画質版
- **Application** (`application/`): PC上で動作するPythonコード。画像/動画読み込み、リサイズ、RGB565変換、COBSエンコード、シリアル通信を担当
- **Web Application** (`web_application/`): ブラウザで動作するWebアプリケーション。Web Serial APIを使用してUSB経由で制御
- **KiCad PCB** (`kicad_pcb/`): 回路図シンボル/フットプリント/基板レイアウトなどのハードウェア設計データを管理

### 通信プロトコル（絶対的ルール）

すべてのファームウェア・クライアント間で、以下のプロトコルは**絶対的な不変条件**です：

- **インターフェース**: USB CDC ACM
- **ペイロード形式**: RGB565 リトルエンディアン
- **エンコーディング**: COBS (Consistent Overhead Byte Stuffing)
- **フレーム区切り**: 末尾 `0x00`
- **解像度**: **128 × 32**（デフォルト。変更する場合はファームウェアとクライアント双方を同期すること）

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
- **Application用**: [uv](https://docs.astral.sh/uv/) - Pythonパッケージマネージャー

### セットアップ手順

#### 1. Firmwareをビルド・書き込み (初回のみ)

```bash
cd firmware
pio run -t upload
```

RP2040にファームウェアが書き込まれ、HUB75パネルの駆動が可能になります。

#### 2. Applicationを実行 (PC側)

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
uv run led-matrix --image photo.jpg

# ヘルプ表示
uv run led-matrix --help
```

#### 3. Web Applicationを使用 (ブラウザ側)

Web Serial APIを使用してブラウザから直接制御することもできます：

```bash
cd web_application
bun install
bun run dev
```

ブラウザで `http://localhost:5173` を開き、画像・動画のドラッグアンドドロップやデモアニメーションを実行できます。

詳細は各ディレクトリのREADMEを参照してください:
- [Application README](application/README.md)
- [Web Application README](web_application/README.md)
- [Firmware README](firmware/README.md)
- [KiCad PCB データ](kicad_pcb/)

## ハードウェア要件

- Raspberry Pi Pico (RP2040)
- HUB75 LED パネル 64x32 x 2枚 (合計128x32)
- USBケーブル
- 5V電源 (LEDパネル用)
