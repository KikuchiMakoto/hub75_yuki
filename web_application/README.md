# HUB75 LED Matrix Web Controller

Web Serial APIを使用してHUB75 LEDマトリックスパネルをブラウザから直接制御するWebアプリケーションです。

## 特徴

- **Web Serial API**: USBシリアル経由でRP2040と直接通信
- **画像・動画表示**: ドラッグアンドドロップで簡単に表示
- **デモアニメーション**: Rainbow, Gradient, Plasma, Fire, Matrix, Clock
- **リアルタイムFPS表示**: パフォーマンスモニタリング
- **レスポンシブUI**: Tailwind CSSによるモダンなデザイン

## 技術スタック

- **TypeScript**: 型安全な開発
- **Bun**: 高速なJavaScriptランタイム
- **React**: UIフレームワーク
- **Vite**: ビルドツール
- **Tailwind CSS**: スタイリング

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

ビルドされたファイルは `../docs` ディレクトリに出力されます。

### プレビュー

```bash
bun run preview
```

## 使用方法

1. **接続**: "接続"ボタンをクリックしてシリアルポートを選択
2. **画像/動画**: ファイルをドラッグアンドドロップまたはクリックして選択
3. **デモ**: デモボタンをクリックしてアニメーションを実行
4. **停止**: "停止"ボタンで現在の表示を停止
5. **切断**: "切断"ボタンでデバイスから切断

## ブラウザ対応

Web Serial APIをサポートするブラウザが必要です:

- Chrome 89+
- Edge 89+
- Opera 76+

## 通信プロトコル

既存のPythonアプリケーションと同じプロトコルを使用:

- **データフォーマット**: RGB565 (16-bit)
- **解像度**: 128x32
- **エンコーディング**: COBS (Consistent Overhead Byte Stuffing)
- **ボーレート**: 115200

## ライセンス

MIT License
