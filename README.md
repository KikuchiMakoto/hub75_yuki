# HUB75 LED Matrix Controller System

128x32 HUB75 LEDマトリックスパネルを制御する完全なシステムです。RP2040 (Raspberry Pi Pico) とPythonアプリケーションで構成されています。

## プロジェクト構成

このプロジェクトは、**Application**、**Web Application**、**Firmware**、**Firmware(CH32V305)**、**KiCad PCB** の5つのコンポーネントで構成されています：

```
├── application/        # Python制御アプリケーション (PC側)
│   └── src/           # LED Matrix Controller (画像/動画/カメラ/テキスト表示)
├── web_application/   # Webアプリケーション (ブラウザ側)
│   └── src/           # React + TypeScript (Web Serial API対応)
├── firmware_ch32v305/ # CH32V305ファームウェア (高速化アーキテクチャ)
│   └── src/           # USBHS受信/COBS復元/BCM変換パイプライン
├── kicad_pcb/         # KiCad設計データ (基板レイアウト/ライブラリ)
└── firmware/          # RP2040ファームウェア (マイコン側)
    └── src/           # HUB75ドライバ (PlatformIO/Arduino)
```

### 役割分担

- **Firmware** (`firmware/`): RP2040上で動作するC++コード。HUB75パネルの駆動、PIO/DMAによる高速出力、USB CDCによるCOBSデータ受信、RGB565からBCM変換を担当
- **Application** (`application/`): PC上で動作するPythonコード。画像/動画読み込み、リサイズ、RGB565変換、COBSエンコード、シリアル通信を担当
- **Web Application** (`web_application/`): ブラウザで動作するWebアプリケーション。Web Serial APIを使用してUSB経由で制御
- **KiCad PCB** (`kicad_pcb/`): 回路図シンボル/フットプリント/基板レイアウトなどのハードウェア設計データを管理

## 特徴

- **複数入力対応**: 画像、動画、デモアニメーション、カメラ入力
- **高速表示**: PIO (Programmable I/O) + DMAによる高速シフト出力
- **デュアルコア設計**: Core0でUSB受信+BCM変換、Core1でパネル駆動のみ（フリッカー防止）
- **RGB565フォーマット**: 16ビットカラー（5-6-5ビット）による効率的な転送
- **COBSエンコーディング**: Base64より効率的なデータ転送
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
