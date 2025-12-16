# HUB75 LED Matrix Controller System

128x32 HUB75 LEDマトリックスパネルを制御する完全なシステムです。RP2040 (Raspberry Pi Pico) とPythonアプリケーションで構成されています。

## 📁 プロジェクト構成

```
├── application/    # Python制御アプリケーション
│   └── src/       # LED Matrix Controller (画像/動画/カメラ/テキスト表示)
└── firmware/      # RP2040ファームウェア
    └── src/       # HUB75ドライバ (PlatformIO/Arduino)
```

## ✨ 特徴

- **複数入力対応**: 画像、動画、Webカメラ、テキスト、デモアニメーション
- **高速表示**: PIO (Programmable I/O) による高速シフト出力
- **デュアルコア**: Core0でUSB受信、Core1でパネル駆動
- **簡単セットアップ**: PlatformIOとPythonで簡単にビルド・実行

## 🚀 クイックスタート

### 1. ファームウェアをRP2040に書き込む

```bash
cd firmware
pio run -t upload
```

### 2. Pythonアプリケーションを実行

```bash
cd application
uv run led-matrix --image photo.jpg
```

詳細は各ディレクトリのREADMEを参照してください:
- [Application README](application/README.md)
- [Firmware README](firmware/README.md)

## 🔌 ハードウェア要件

- Raspberry Pi Pico (RP2040)
- HUB75 LED パネル 64x32 x 2枚 (合計128x32)
- USBケーブル
- 5V電源 (LEDパネル用)

## 📝 ライセンス

MIT License

## 🤝 貢献

Issue、Pull Requestを歓迎します！
