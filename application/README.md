# LED Matrix Controller

128x32 HUB75 LEDマトリックスパネルを制御するPythonアプリケーションです。

## 特徴

- **複数入力対応**: 画像、動画、デモアニメーション、単色塗りつぶし
- **アスペクト比保持リサイズ**: 入力画像を128x32に自動変換
- **RGB565エンコーディング**: 16bitカラーでメモリ効率化
- **COBSプロトコル**: データ転送の信頼性確保
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

# 単色塗りつぶし (赤色)
uv run led-matrix --fill 255,0,0
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

# ターミナルシミュレータ (ハードウェアなしで動作確認)
uv run led-matrix --device terminal --demo rainbow

# 画像出力デバイス (フレームをPNGとして保存)
uv run led-matrix --device image --demo rainbow --output-dir frames
```

## コマンドラインオプション

```
デバイスオプション:
  --device, -d {serial,terminal,image}  出力デバイス (default: serial)
  --port, -p PORT                       シリアルポート (自動検出)
  --baudrate, -b BAUDRATE               ボーレート (default: 115200)
  --output-dir                          画像出力ディレクトリ (default: output)

入力オプション (排他):
  --image, -i FILE                  画像ファイル表示
  --video, -v FILE                  動画ファイル再生
  --demo {rainbow,gradient,plasma,fire,matrix,clock}  デモアニメーション
  --fill, -f R,G,B                  単色塗りつぶし (例: 255,0,0)

表示オプション:
  --loop                            動画ループ再生
  --fps FPS                         デモFPS (default: 30)
  --brightness BRIGHTNESS           明るさ 0.0-1.0 (default: 1.0)
```

## 通信プロトコル

### データフォーマット
- **RGB565**: 16bitカラー形式
  - 赤: 5bit (0-31)
  - 緑: 6bit (0-63)
  - 青: 5bit (0-31)
  - データサイズ: 128 × 32 × 2 = 8192 bytes

### エンコーディング
- **COBS (Consistent Overhead Byte Stuffing)**: プロトコルのエンコーディング方式
  - ゼロバイトをデータストリームから除去し、オーバーヘッドコードに置換
  - パケットはゼロバイトで終了
  - バイナリデータの安全な転送を実現

### 転送フロー
```
PC → RP2040:
    [COBSエンコードされたRGB565データ][0x00]

RP2040 → PC:
    なし（非同期転送）
```

### COBSエンコーディングの利点
- ストリーミング転送に適した明確なパケット境界
- 小さなオーバーヘッド（最大約25%）
- データ内の任意のバイナリ値を安全に転送可能

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
