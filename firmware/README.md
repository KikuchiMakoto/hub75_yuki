# HUB75 LED Panel Controller (PlatformIO版)

RP2040でHUB75 LEDパネルを駆動するPlatformIOプロジェクトです。

## 特徴

- **PlatformIO + Arduino**: 簡単なビルド環境
- **デュアルコア**: Core0でUSB受信、Core1でパネル駆動
- **PIO使用**: 高速シフト出力
- **BCM**: 6bitカラー深度

## ピン接続

```
RP2040 GPIO    HUB75信号
-----------    ---------
GP0            R0
GP1            G0
GP2            B0
GP3            R1
GP4            G1
GP5            B1
GP6            CLK
GP7            LAT
GP8            OE
GP9            A
GP10           B
GP11           C
GP12           D
GND            GND
```

## HUB75コネクタ (16ピン)

```
┌─────────┐
│ R0  G0  │ 01-02
│ B0  GND │ 03-04
│ R1  G1  │ 05-06
│ B1  E   │ 07-08
│ A   B   │ 09-10
│ C   D   │ 11-12
│ CLK LAT │ 13-14
│ OE  GND | 15-16
└─────────┘
```

## パネル接続

```
Pico → [Panel1 64x32] OUT→IN [Panel2 64x32]
```

## ビルド・書き込み

```bash
# ビルド
pio run

# 書き込み
pio run -t upload

# シリアルモニタ
pio device monitor
```

## アプリケーション側

ファームウェアの書き込み後、アプリケーション側から制御します。
詳細は [../application/README.md](../application/README.md) を参照してください。

```bash
cd ../application

# デモアニメーション
uv run led-matrix --demo rainbow

# 画像表示
uv run led-matrix --image photo.jpg

# 動画再生
uv run led-matrix --video movie.mp4
```

## 通信プロトコル

```
PC → Pico: [Base64(RGB565)]\n
Pico → PC: 'K' (ACK) / 'E' (Error)
```

## ファイル構成

```
firmware/
├── platformio.ini
├── include/
│   ├── hub75_config.h
│   └── hub75.pio.h
├── src/
│   └── main.cpp
└── README.md
```
