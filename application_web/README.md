# HUB75 LED Matrix Web Controller

Web Serial APIを使用してHUB75 LEDマトリックスパネルをブラウザから直接制御するWebアプリケーションです。

## 特徴

- **Web Serial API**: USBシリアル経由でRP2040と直接通信
- **画像表示**: ドラッグアンドドロップで簡単に表示（アスペクト比保持・レターボックス）
- **動画再生**: FFmpeg.wasmでリアルタイムリサイズ（最大18fps）
- **デモアニメーション**: Rainbow, Gradient, Plasma, Fire, Matrix, Clock
- **リアルタイムFPS表示**: パフォーマンスモニタリング
- **フレームドロップ対策**: シリアル送信中は新しいフレームをスキップ
- **RGB565フォーマット**: 16-bitカラーフォーマットを使用
- **COBSエンコーディング**: Consistent Overhead Byte Stuffingでパケット境界を明確化

## 技術スタック

- **TypeScript**: 型安全な開発
- **Bun**: 高速なJavaScriptランタイム
- **React 19**: UIフレームワーク
- **Vite 7**: ビルドツール
- **Tailwind CSS 4**: スタイリング
- **FFmpeg.wasm**: 動画処理
- **Web Serial API**: シリアル通信

## 開発

### 前提条件

- [Bun](https://bun.sh/) がインストールされていること
- Chrome または Edge ブラウザ (Web Serial API対応)

### セットアップ

```bash
cd web_application
bun install
```

### 開発サーバーの起動

```bash
bun run dev
```

ブラウザで `http://localhost:5173` を開きます。

### ビルド

```bash
bun run build
```

ビルドされたファイルは `dist` ディレクトリに出力されます。

### プレビュー

```bash
bun run preview
```

## 使用方法

1. **接続**: "接続"ボタンをクリックしてシリアルポートを選択
2. **画像**: 画像ファイルをドラッグアンドドロップまたはクリックして選択
3. **動画**: 動画ファイルをドラッグアンドドロップ（自動で128x32にリサイズされます）
4. **デモ**: デモボタンをクリックしてアニメーションを実行
5. **停止**: "停止"ボタンで現在の表示を停止
6. **切断**: "切断"ボタンでデバイスから切断

## ブラウザ対応

Web Serial APIをサポートするブラウザが必要です:

- Chrome 89+
- Edge 89+
- Opera 76+

## Linux でのシリアルポート権限設定

Linux で Web Serial API を使うと「アクセスが拒否された」「ポートが見つからない」などのエラーが出ることがあります。以下をコピーしてターミナルで一括実行してください。

```bash
sudo systemctl stop brltty-usb.service brltty.service serial-getty@ttyACM0.service serial-getty@ttyUSB0.service 2>/dev/null || true
sudo systemctl disable brltty-usb.service serial-getty@ttyACM0.service serial-getty@ttyUSB0.service 2>/dev/null || true
sudo usermod -aG dialout $USER
echo 'KERNEL=="ttyACM[0-9]*", GROUP="dialout", MODE="0660"
KERNEL=="ttyUSB[0-9]*", GROUP="dialout", MODE="0660"' | sudo tee /etc/udev/rules.d/99-usb-serial.rules >/dev/null
sudo udevadm control --reload-rules && sudo udevadm trigger
echo "完了。再ログインまたは newgrp dialout で権限を反映してください。"
```

**このスクリプトがやっていること**
1. `brltty`（点字支援サービス）がシリアルポートを掴むのを止める
2. `serial-getty`（シリアルコンソールログイン）がポートを占有するのを止める
3. あなたを `dialout` グループに追加 → `/dev/ttyACM*` や `/dev/ttyUSB*` の読み書きが可能に
4. udev で「CDC-ACM / USB-シリアル デバイス全体」に対して自動で `dialout` 権限を付与

> **再ログインが必要**: グループ変更は新しいセッションで初めて反映されます。
>
> **ModemManager を使っている場合**: `sudo systemctl stop ModemManager.service` も追加で実行してください。

## 通信プロトコル

既存のPythonアプリケーションと同じプロトコルを使用:

- **データフォーマット**: **RGB565** (16-bitカラー)
- **解像度**: 128x32
- **エンコーディング**: **COBS** (Consistent Overhead Byte Stuffing)
- **ボーレート**: 115200
- **パケット終端**: 0x00バイト

### RGB565フォーマット

RGBA (8bit x 4) を **RGB565** (16-bit) に変換して送信:
- Red: 8bit → 5bit (上位3bitを破棄)
- Green: 8bit → 6bit (上位2bitを破棄)
- Blue: 8bit → 5bit (上位3bitを破棄)
- フォーマット: RRRRRGGG GGGBBBBB (Little-endian)

### COBSエンコーディング

**COBS (Consistent Overhead Byte Stuffing)** を使用してパケット化:
- データ中の0x00バイトをオーバーヘッドコードに置換
- パケット終端を0x00バイトで明確化
- パケット境界を確実に識別可能
- オーバーヘッドは最大1%程度

## プロジェクト構成

```
src/
├── components/
│   ├── DropZone.tsx      # ファイルドロップコンポーネント
│   └── DemoSelector.tsx  # デモ選択コンポーネント
├── lib/
│   ├── cobs.ts           # COBSエンコーディング実装
│   ├── demos.ts          # デモアニメーション生成
│   ├── media.ts          # 画像・動画再生
│   ├── serial.ts         # Web Serial APIラッパー
│   └── videoProcessor.ts # FFmpeg.wasmによる動画処理
├── types/
│   └── index.ts          # 型定義
├── App.tsx               # メインアプリケーション
└── main.tsx              # エントリーポイント
```

## デモアニメーション

- **Rainbow**: 水平方向のグラデーション（時間で変化）
- **Gradient**: 2Dグラデーション
- **Plasma**: 複数の正弦波によるプラズマ効果
- **Fire**: 下から上への炎のアニメーション
- **Matrix**: マトリックス風の落下文字
- **Clock**: 現在時刻の表示（色パルス効果）

## 画像・動画処理

### 画像処理

- アスペクト比を維持して128x32にリサイズ
- 余白は黒で埋める（レターボックス）
- 水平方向のフリップ（HUB75シフトレジスタ順対応）

### 動画処理

- FFmpeg.wasmを使用してブラウザ内で処理
- 入力動画を128x32にリサイズ
- フレームレートを18fpsに制限
- 音声を削除
- H.264コーデックでエンコード

## ライセンス

MIT License
