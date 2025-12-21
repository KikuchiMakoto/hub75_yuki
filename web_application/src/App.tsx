import React, { useState, useEffect, useRef } from 'react';
import { SerialDevice } from './lib/serial';
import { loadImage, VideoPlayer } from './lib/media';
import { VideoProcessor } from './lib/videoProcessor';
import { generateDemoFrame } from './lib/demos';
import { DropZone } from './components/DropZone';
import { DemoSelector } from './components/DemoSelector';
import type { DemoType } from './types';

type Mode = 'idle' | 'image' | 'video' | 'demo';

function App() {
  const [connected, setConnected] = useState(false);
  const [mode, setMode] = useState<Mode>('idle');
  const [status, setStatus] = useState('');
  const [fps, setFps] = useState(0);

  const serialRef = useRef<SerialDevice>(new SerialDevice());
  const videoPlayerRef = useRef<VideoPlayer>(new VideoPlayer());
  const videoProcessorRef = useRef<VideoProcessor>(new VideoProcessor());
  const demoAnimationRef = useRef<number | null>(null);
  const fpsCounterRef = useRef({ count: 0, lastTime: Date.now() });

  useEffect(() => {
    return () => {
      stopCurrentMode();
      if (connected) {
        serialRef.current.disconnect();
      }
    };
  }, []);

  const stopCurrentMode = () => {
    if (mode === 'video') {
      videoPlayerRef.current.stop();
    } else if (mode === 'demo' && demoAnimationRef.current !== null) {
      cancelAnimationFrame(demoAnimationRef.current);
      demoAnimationRef.current = null;
    }
    setMode('idle');
  };

  const updateFps = () => {
    fpsCounterRef.current.count++;
    const now = Date.now();
    const elapsed = now - fpsCounterRef.current.lastTime;
    if (elapsed >= 1000) {
      setFps(Math.round((fpsCounterRef.current.count * 1000) / elapsed));
      fpsCounterRef.current.count = 0;
      fpsCounterRef.current.lastTime = now;
    }
  };

  const handleConnect = async () => {
    try {
      await serialRef.current.connect();
      setConnected(true);
      setStatus('接続成功');
    } catch (error) {
      console.error('Connection failed:', error);
      setStatus(`接続失敗: ${error}`);
    }
  };

  const handleDisconnect = async () => {
    stopCurrentMode();
    await serialRef.current.disconnect();
    setConnected(false);
    setStatus('切断しました');
    setFps(0);
  };

  const handleFileDrop = async (file: File) => {
    if (!connected) {
      setStatus('先にデバイスを接続してください');
      return;
    }

    stopCurrentMode();

    if (file.type.startsWith('image/')) {
      try {
        setStatus('画像を読み込み中...');
        const imageData = await loadImage(file);
        setStatus('画像を表示中');
        setMode('image');
        await serialRef.current.sendFrame(imageData);
        updateFps();
      } catch (error) {
        setStatus(`画像の読み込みに失敗: ${error}`);
      }
    } else if (file.type.startsWith('video/')) {
      try {
        setStatus('FFmpegを読み込み中...');
        await videoProcessorRef.current.load();

        setStatus('動画のメタデータを取得中...');
        const metadata = await videoProcessorRef.current.getVideoMetadata(file);

        setStatus(`動画をリサイズ中... (${metadata.width}x${metadata.height} → 128x32)`);
        const processedBlob = await videoProcessorRef.current.resizeVideo(file);

        setStatus('動画を読み込み中...');
        await videoPlayerRef.current.load(processedBlob);
        setStatus('動画を再生中');
        setMode('video');
        videoPlayerRef.current.play(async (imageData) => {
          await serialRef.current.sendFrame(imageData);
          updateFps();
        });
      } catch (error) {
        setStatus(`動画の読み込みに失敗: ${error}`);
      }
    }
  };

  const handleDemoSelect = (demo: DemoType) => {
    if (!connected) {
      setStatus('先にデバイスを接続してください');
      return;
    }

    stopCurrentMode();
    setStatus(`デモを実行中: ${demo}`);
    setMode('demo');

    let startTime = Date.now();
    const runDemo = async () => {
      const t = (Date.now() - startTime) / 1000;
      const imageData = generateDemoFrame(demo, t);
      await serialRef.current.sendFrame(imageData);
      updateFps();
      demoAnimationRef.current = requestAnimationFrame(runDemo);
    };

    runDemo();
  };

  const handleStop = () => {
    stopCurrentMode();
    setStatus('停止しました');
    setFps(0);
  };

  return (
    <div className="min-h-screen bg-gradient-to-br from-gray-50 to-gray-100">
      <div className="container mx-auto px-4 py-8 max-w-4xl">
        <header className="mb-8">
          <h1 className="text-4xl font-bold text-gray-800 mb-2">
            HUB75 LED Matrix Web Controller
          </h1>
          <p className="text-gray-600">
            Web Serial APIを使用してLEDマトリックスパネルを制御
          </p>
        </header>

        <div className="bg-white rounded-lg shadow-lg p-6 mb-6">
          <div className="flex items-center justify-between mb-4">
            <div>
              <h2 className="text-xl font-semibold text-gray-800">接続状態</h2>
              <p className="text-sm text-gray-600 mt-1">{status || '未接続'}</p>
            </div>
            <div className="flex gap-2 items-center">
              {connected && mode !== 'idle' && (
                <div className="text-sm text-gray-600 mr-2">
                  FPS: <span className="font-mono font-bold">{fps}</span>
                </div>
              )}
              {!connected ? (
                <button
                  onClick={handleConnect}
                  className="px-6 py-2 bg-blue-500 text-white rounded-lg hover:bg-blue-600 transition-colors font-semibold"
                >
                  接続
                </button>
              ) : (
                <>
                  {mode !== 'idle' && (
                    <button
                      onClick={handleStop}
                      className="px-6 py-2 bg-yellow-500 text-white rounded-lg hover:bg-yellow-600 transition-colors font-semibold"
                    >
                      停止
                    </button>
                  )}
                  <button
                    onClick={handleDisconnect}
                    className="px-6 py-2 bg-red-500 text-white rounded-lg hover:bg-red-600 transition-colors font-semibold"
                  >
                    切断
                  </button>
                </>
              )}
            </div>
          </div>
          {connected && (
            <div className="text-xs text-green-600 font-semibold">
              ✓ デバイスに接続済み
            </div>
          )}
        </div>

        <div className="bg-white rounded-lg shadow-lg p-6 mb-6">
          <h2 className="text-xl font-semibold text-gray-800 mb-4">
            画像・動画
          </h2>
          <DropZone onFileDrop={handleFileDrop} disabled={!connected} />
        </div>

        <div className="bg-white rounded-lg shadow-lg p-6">
          <h2 className="text-xl font-semibold text-gray-800 mb-4">
            デモアニメーション
          </h2>
          <DemoSelector onSelectDemo={handleDemoSelect} disabled={!connected} />
        </div>

        <footer className="mt-8 text-center text-sm text-gray-500">
          <p>128x32 HUB75 LED Matrix Controller</p>
          <p className="mt-1">
            Web Serial APIを使用 - Chrome/Edge推奨
          </p>
        </footer>
      </div>
    </div>
  );
}

export default App;
