import { DISPLAY_WIDTH, DISPLAY_HEIGHT } from '../types';

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
 */
export class VideoPlayer {
  private video: HTMLVideoElement;
  private canvas: HTMLCanvasElement;
  private ctx: CanvasRenderingContext2D;
  private animationId: number | null = null;
  private onFrame: ((imageData: ImageData) => void) | null = null;

  constructor() {
    this.video = document.createElement('video');
    this.video.muted = true;
    this.video.loop = true;

    this.canvas = document.createElement('canvas');
    this.canvas.width = DISPLAY_WIDTH;
    this.canvas.height = DISPLAY_HEIGHT;
    this.ctx = this.canvas.getContext('2d')!;
  }

  async load(file: File): Promise<void> {
    this.video.src = URL.createObjectURL(file);
    await new Promise((resolve, reject) => {
      this.video.onloadedmetadata = resolve;
      this.video.onerror = reject;
    });
  }

  play(onFrame: (imageData: ImageData) => void): void {
    this.onFrame = onFrame;
    this.video.play();
    this.renderLoop();
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

  private renderLoop = (): void => {
    if (!this.onFrame || this.video.paused || this.video.ended) {
      return;
    }

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

    this.animationId = requestAnimationFrame(this.renderLoop);
  };

  isPlaying(): boolean {
    return !this.video.paused && !this.video.ended;
  }
}
