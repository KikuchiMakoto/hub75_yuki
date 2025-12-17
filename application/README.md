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

### デバイス指定

```bash
# シリアルポート指定
uv run led-matrix --port COM3 --demo rainbow
```

## コマンドラインオプション

```
デバイスオプション:
  --port PORT                       シリアルポート (自動検出)
  --baudrate BAUDRATE               ボーレート (default: 115200)

入力オプション (排他):
  --image FILE                      画像ファイル
  --video FILE                      動画ファイル
  --demo {rainbow,gradient,plasma,fire,matrix,clock}  デモ

表示オプション:
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

## ffmpeg動画変換コマンドサンプル

128x32用
```powershell
ffmpeg -i input.mp4 `
-vf "scale=128:-1,crop=128:32" `
-an `
-c:v libx264 -qp 0 -preset ultrafast `
-y `
output_128x32.mp4
```

64x64用（右側パネルを180度回転して下付け）
```powershell
ffmpeg -i input.mp4 -filter_complex " `
[0:v]scale=64:64:force_original_aspect_ratio=increase,crop=64:64[base]; `
[base]split[top][bottom]; `
[top]crop=64:32:0:0[left]; `
[bottom]crop=64:32:0:32,hflip,vflip[right]; `
[left][right]hstack" `
-an `
-c:v libx264 -qp 0 -preset ultrafast `
-y `
output_64x64.mp4
```
