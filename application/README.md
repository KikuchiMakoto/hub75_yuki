# LED Matrix Controller

128x32 HUB75 LEDマトリックスパネルを制御するPythonアプリケーションです。

## 特徴

- **複数入力対応**: 画像、動画、Webカメラ、テキスト、デモアニメーション
- **アスペクト比保持リサイズ**: 入力画像を128x32に自動変換
- **RGB565エンコード**: Base64 + 改行でRP2040に送信
- **複数出力デバイス**: シリアル、ターミナルシミュレータ、画像出力


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
python -m led-matrix --text "Hello World"

# スクロールテキスト
python -m led-matrix --text "スクロールメッセージ" --scroll

# スクロール速度指定
python -m led-matrix --text "Fast!" --scroll --scroll-speed 0.01
```

### デモアニメーション

```bash
# レインボー
python -m led-matrix --demo rainbow

# プラズマ
python -m led-matrix --demo plasma

# 炎エフェクト
python -m led-matrix --demo fire

# マトリックス風
python -m led-matrix --demo matrix

# 時計
python -m led-matrix --demo clock
```

### 単色塗りつぶし

```bash
# 赤
python -m led-matrix --fill 255,0,0

# 緑
python -m led-matrix --fill 0,255,0
```

### デバイス指定

```bash
# シリアルポート指定
python -m led-matrix --port COM3 --demo rainbow

# ターミナルシミュレータ (ハードウェアなしでテスト)
python -m led-matrix --device terminal --demo rainbow

# 画像出力 (PNG保存)
python -m led-matrix --device image --image photo.jpg
```

### その他のオプション

```bash
# 明るさ調整 (0.0-1.0)
python -m led-matrix --brightness 0.5 --demo rainbow

# FPS指定
python -m led-matrix --fps 60 --demo plasma
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
led_matrix_app/
├── pyproject.toml
├── requirements.txt
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
- hub75_platformio プロジェクト

## ライセンス

MIT License
