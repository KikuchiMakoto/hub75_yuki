# HUB75 LED Matrix Controller System

128x32 HUB75 LEDマトリックスパネルを制御する完全なシステムです。RP2040 (Raspberry Pi Pico) とPythonアプリケーションで構成されています。

## プロジェクト構成

このプロジェクトは、**Application** と **Firmware** の2つの独立したコンポーネントで構成されています：

```
├── application/    # Python制御アプリケーション (PC側)
│   └── src/       # LED Matrix Controller (画像/動画/カメラ/テキスト表示)
└── firmware/      # RP2040ファームウェア (マイコン側)
    └── src/       # HUB75ドライバ (PlatformIO/Arduino)
```

### 役割分担

- **Firmware** (`firmware/`): RP2040上で動作するC++コード。HUB75パネルの駆動とUSB通信のみを担当
- **Application** (`application/`): PC上で動作するPythonコード。画像処理、動画再生、デモ生成などを担当

## 特徴

- **複数入力対応**: 画像、動画、デモアニメーション
- **高速表示**: PIO (Programmable I/O) による高速シフト出力
- **デュアルコア**: Core0でUSB受信、Core1でパネル駆動
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

詳細は各ディレクトリのREADMEを参照してください:
- [Application README](application/README.md)
- [Firmware README](firmware/README.md)

## ハードウェア要件

- Raspberry Pi Pico (RP2040)
- HUB75 LED パネル 64x32 x 2枚 (合計128x32)
- USBケーブル
- 5V電源 (LEDパネル用)

## ライセンス

MIT License

## 貢献

Issue、Pull Requestを歓迎します!
