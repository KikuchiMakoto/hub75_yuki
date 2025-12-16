# LED Matrix Controller

128x32 HUB75 LEDマトリックスパネルを制御するPythonアプリケーションです。

## 特徴

- **複数入力対応**: 画像、動画、Webカメラ、テキスト、デモアニメーション
- **アスペクト比保持リサイズ**: 入力画像を128x32に自動変換
- **RGB565エンコード**: Base64 + 改行でRP2040に送信
- **複数出力デバイス**: シリアル、ターミナルシミュレータ、画像出力

## 必要条件

- Python 3.9以上
- [uv](https://docs.astral.sh/uv/) (推奨パッケージマネージャー)

### uvのインストール

```bash
# macOS / Linux
curl -LsSf https://astral.sh/uv/install.sh | sh

# Windows (PowerShell)
powershell -ExecutionPolicy ByPass -c "irm https://astral.sh/uv/install.ps1 | iex"

# pipでインストール
pip install uv
```

## 使い方

### 基本

```bash
# ヘルプ表示
uv run led-matrix --help

# 画像表示
uv run led-matrix --image photo.jpg

# 動画再生
uv run led-matrix --video movie.mp4

# 動画ループ再生
uv run led-matrix --video movie.mp4 --loop

# Webカメラ
uv run led-matrix --camera
```

### テキスト表示

```bash
# 静的テキスト
uv run led-matrix --text "Hello World"

# スクロールテキスト
uv run led-matrix --text "スクロールメッセージ" --scroll

# スクロール速度指定
uv run led-matrix --text "Fast!" --scroll --scroll-speed 0.01
```

### デモアニメーション

```bash
# レインボー
uv run led-matrix --demo rainbow

# プラズマ
uv run led-matrix --demo plasma

# 炎エフェクト
uv run led-matrix --demo fire

# マトリックス風
uv run led-matrix --demo matrix

# 時計
uv run led-matrix --demo clock
```

### 単色塗りつぶし

```bash
# 赤
uv run led-matrix --fill 255,0,0

# 緑
uv run led-matrix --fill 0,255,0
```

### デバイス指定

```bash
# シリアルポート指定
uv run led-matrix --port COM3 --demo rainbow

# ターミナルシミュレータ (ハードウェアなしでテスト)
uv run led-matrix --device terminal --demo rainbow

# 画像出力 (PNG保存)
uv run led-matrix --device image --image photo.jpg
```

### その他のオプション

```bash
# 明るさ調整 (0.0-1.0)
uv run led-matrix --brightness 0.5 --demo rainbow

# FPS指定
uv run led-matrix --fps 60 --demo plasma
```

## コマンドラインオプション

```
デバイスオプション:
  --device {serial,terminal,image}  出力デバイス (default: serial)
  --port PORT                       シリアルポート (自動検出)
  --baudrate BAUDRATE               ボーレート (default: 115200)
  --output-dir DIR                  画像出力ディレクトリ (default: output)

入力オプション (排他):
  --image FILE                      画像ファイル
  --video FILE                      動画ファイル
  --camera                          Webカメラ
  --text TEXT                       テキスト
  --demo {rainbow,gradient,plasma,fire,matrix,clock}  デモ
  --fill R,G,B                      単色塗りつぶし

表示オプション:
  --scroll                          テキストスクロール
  --scroll-speed SPEED              スクロール速度 (default: 0.03)
  --camera-id ID                    カメラID (default: 0)
  --loop                            動画ループ
  --fps FPS                         デモFPS (default: 30)
  --brightness BRIGHTNESS           明るさ 0.0-1.0 (default: 1.0)
```

## 通信プロトコル

```
PC → RP2040:
    [Base64(RGB565データ)]\n

    データサイズ: 128 × 32 × 2 = 8192 bytes
    Base64後: 約 10924 bytes + 改行

RP2040 → PC:
    'K' = ACK (成功)
    'E' = Error (失敗)
```

## プロジェクト構造

```
application/
├── pyproject.toml      # プロジェクト設定 (uv/pip互換)
├── README.md
└── src/
    └── led_matrix_controller/
        ├── __init__.py
        ├── main.py              # エントリーポイント
        ├── controller.py        # メインコントローラー
        └── devices/
            ├── __init__.py
            ├── base.py          # デバイス基底クラス
            ├── serial_device.py # シリアル通信
            └── simulator.py     # ターミナル/画像出力
```

## ファームウェア

このアプリケーションは以下のRP2040ファームウェアと連携します:
- [firmware/](../firmware/) ディレクトリ参照

## ライセンス

MIT License
