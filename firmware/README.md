# HUB75 LED Panel Controller (PlatformIO版)

RP2040でHUB75 LEDパネルを駆動するPlatformIOプロジェクトです。

## 特徴

- **PlatformIO + Arduino**: 簡単なビルド環境 (Earle Philhower core)
- **デュアルコア**: Core0でUSB受信/DEM演算+BCM変換、Core1で100%パネル駆動専任（フリッカー完全防止）
- **PIO+DMA高速駆動**: 7.14 MHz シフトクロックによる約 350 Hz の高速内部リフレッシュ
- **ハードウェア除算アクセラレーション**: SIO ハードウェア除算器による DEM 物理演算の高速化（約 250 FPS）
- **過大評価描画**: 2x2 box pixel の黒抜けゼロ化による高密度・高輝度砂粒表示
- **即時起動**: 余計なスプラッシュやセルフテスト待ちを全廃
- **RGB565フォーマット**: 16ビットカラー（5-6-5ビット）
- **BCM**: 6bitカラー深度（64階調）
- **Gamma補正**: 2.2固定で輝度補正
- **COBSエンコーディング**: 0x00区切りのバイナリフレーム転送

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

## ビルド環境

ビルド環境は標準の **`pico`** に一本化されています：

### pico (デフォルト / 推奨)
- PIO (7.14 MHz 最適化) + DMA ダブルバッファリングによる高速シフト出力
- 128x32パネル（64x32 パネル 2枚連結）専用
- 内部リフレッシュレート 約 350 Hz（フリッカーフリー）
- 動作クロック 250 MHz (USB CDC 安定通信)

```bash
pio run -e pico
```

## 書き込み・モニタ

```bash
# 書き込み (デフォルト環境)
pio run -t upload

# 書き込み (特定環境)
pio run -t upload -e pico_gpio

# シリアルモニタ (115200 baud)
pio device monitor

# ビルドのみ (書き込みなし)
pio run
```

## アプリケーション側

ファームウェアの書き込み後、アプリケーション側から制御します。
詳細は [../application_py/README.md](../application_py/README.md) を参照してください。

```bash
cd ../application_py

# デモアニメーション
uv run led-matrix --demo rainbow

# 画像表示
uv run led-matrix --image photo.jpg

# 動画再生
uv run led-matrix --video movie.mp4
```

## 通信プロトコル

### COBSエンコーディング
PCからPicoへのデータ転送にはCOBS (Consistent Overhead Byte Stuffing) を使用します：

```
PC → Pico: [COBS(RGB565 frame)] 0x00
```

- **フォーマット**: COBSエンコードされたRGB565データ + 0x00デリミタ
- **フレームサイズ**: 128x32 = 8KB (8192 bytes), 128x64 = 16KB (16384 bytes)
- **エンコードオーバーヘッド**: 約1バイト/254バイト + 1バイト
- **最大バッファサイズ**: RECV_BUFFER_SIZE = FRAME_SIZE + overhead + margin

### 受信処理
- Core0がUSB CDCからデータを受信
- 0x00バイトを受信した時点でパケット完了と判定
- COBSデコードを実行し、サイズ検証
- 正常にデコードできたら即座にframe_bufferへコピーしBCM変換
- 不正パケットはサイレントに破棄

### レスポンス
現在の実装では、PicoからPCへのレスポンスは送信しません（高速化のため）。

## アーキテクチャ

### デュアルコア設計

**Core0 (USB受信 + BCM変換)**:
- USB CDCからのシリアル受信
- COBSデコード処理
- RGB565からBCMプレーンへの変換
- Gamma補正 (2.2)
- フレームバッファの更新

**Core1 (パネル駆動のみ)**:
- `hub75_refresh()`関数の実行のみ
- 他の処理は一切行わない（フリッカー防止）
- BCMタイミングによるパネルの駆動

### 表示技術

**BCM (Binary Code Modulation)**:
- 6ビットカラー深度 = 64階調/色
- 各ビットプレーンで異なる表示時間 (1us, 2us, 4us, 8us, 16us, 32us)
- 最大63us/サイクルでフルカラー表示
- スキャン行数: 16行 (32列パネル) または 32行 (64列パネル)

**PIO + DMAパイプライン (Phase 2)**:
- PIOがシフトアウトをハードウェア的に実行
- DMAがPIO FIFOへデータを供給
- ダブルバッファリングで転送効率化
- DMA転送中に次の行を準備するパイプライン処理

### 起動画面

起動時には以下の色表示で初期化確認：
1. 赤色 (500ms)
2. 緑色 (500ms)
3. 青色 (500ms)
4. 白色 (300ms)
5. 画面クリア

この時は直接GPIO制御で表示（BCM未使用）。
起動完了後にPIO/DMAを有効化します。

## ファイル構成

```
firmware/
├── platformio.ini          # ビルド設定（3つの環境定義）
├── README.md              # このドキュメント
├── include/
│   ├── hub75_config.h     # ピン設定・バッファサイズ・コンフィグ
│   ├── hub75.pio.h        # PIOプログラム（シフトアウト）
│   └── tusb_config.h      # TinyUSB設定（高スループットCDC）
├── src/
│   └── main.cpp           # メイン処理（デュアルコア・BCM・COBS）
└── .gitignore
```
