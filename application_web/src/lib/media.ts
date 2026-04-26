import { DISPLAY_WIDTH, DISPLAY_HEIGHT } from '../types';

// Maximum FPS for video playback (internal limit)
const MAX_VIDEO_FPS = 18;

/**
 * Load image file and convert to ImageData
 */
export async function loadImage(file: File): Promise<ImageData> {
  return new Promise((resolve, reject) => {
    const img = new Image();
    img.onload = () => {
      const canvas = document.createElement('canvas');
      canvas.width = DISPLAY_WIDTH;
      canvas.height = DISPLAY_HEIGHT;
      const ctx = canvas.getContext('2d')!;

      // Draw image with aspect ratio preservation (letterbox)
      const scale = Math.min(
        DISPLAY_WIDTH / img.width,
        DISPLAY_HEIGHT / img.height
      );
      const w = img.width * scale;
      const h = img.height * scale;
      const x = (DISPLAY_WIDTH - w) / 2;
      const y = (DISPLAY_HEIGHT - h) / 2;

      ctx.fillStyle = 'black';
      ctx.fillRect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
      ctx.drawImage(img, x, y, w, h);

      resolve(ctx.getImageData(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT));
    };
    img.onerror = reject;
    img.src = URL.createObjectURL(file);
  });
}

/**
 * Video player for HUB75 display
 * With simple frame rate control for stable serial transmission
 * Maximum frame rate is limited to MAX_VIDEO_FPS (15fps)
 */
export class VideoPlayer {
  private video: HTMLVideoElement;
  private canvas: HTMLCanvasElement;
  private ctx: CanvasRenderingContext2D;
  private animationId: number | null = null;
  private onFrame: ((imageData: ImageData) => void) | null = null;
  private lastFrameTime: number = 0;
  private targetFps: number = MAX_VIDEO_FPS; // Target fps limited to MAX_VIDEO_FPS (15fps)

  constructor() {
    this.video = document.createElement('video');
    this.video.muted = true;
    this.video.loop = true;

    this.canvas = document.createElement('canvas');
    this.canvas.width = DISPLAY_WIDTH;
    this.canvas.height = DISPLAY_HEIGHT;
    this.ctx = this.canvas.getContext('2d')!;
  }

  /**
   * Set target frame rate (fps)
   * Maximum is limited to MAX_VIDEO_FPS (15fps)
   */
  setTargetFps(fps: number): void {
    this.targetFps = Math.max(1, Math.min(MAX_VIDEO_FPS, fps));
  }

  async load(file: File | Blob): Promise<void> {
    this.video.src = URL.createObjectURL(file);
    await new Promise((resolve, reject) => {
      this.video.onloadedmetadata = resolve;
      this.video.onerror = reject;
    });
    // Reset target FPS to maximum allowed (18fps)
    // Video playback will be limited to this rate
    this.targetFps = MAX_VIDEO_FPS;
  }

  play(onFrame: (imageData: ImageData) => void): void {
    this.onFrame = onFrame;
    this.lastFrameTime = 0;
    this.video.play();
    this.animationId = requestAnimationFrame(this.renderLoop);
  }

  pause(): void {
    this.video.pause();
    if (this.animationId !== null) {
      cancelAnimationFrame(this.animationId);
      this.animationId = null;
    }
  }

  stop(): void {
    this.pause();
    this.video.currentTime = 0;
    this.onFrame = null;
  }

  private renderLoop = (timestamp: number): void => {
    if (!this.onFrame || this.video.paused || this.video.ended) {
      return;
    }

    // Simple frame rate limiting
    const frameInterval = 1000 / this.targetFps;
    const elapsed = timestamp - this.lastFrameTime;

    if (elapsed >= frameInterval) {
      this.lastFrameTime = timestamp - (elapsed % frameInterval);

      // Draw video frame to canvas
      const scale = Math.min(
        DISPLAY_WIDTH / this.video.videoWidth,
        DISPLAY_HEIGHT / this.video.videoHeight
      );
      const w = this.video.videoWidth * scale;
      const h = this.video.videoHeight * scale;
      const x = (DISPLAY_WIDTH - w) / 2;
      const y = (DISPLAY_HEIGHT - h) / 2;

      this.ctx.fillStyle = 'black';
      this.ctx.fillRect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
      this.ctx.drawImage(this.video, x, y, w, h);

      const imageData = this.ctx.getImageData(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
      this.onFrame(imageData);
    }

    this.animationId = requestAnimationFrame(this.renderLoop);
  };

  isPlaying(): boolean {
    return !this.video.paused && !this.video.ended;
  }
}
