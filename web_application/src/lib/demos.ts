import { DISPLAY_WIDTH, DISPLAY_HEIGHT, DemoType } from '../types';

/**
 * Generate demo animation frame
 */
export function generateDemoFrame(demo: DemoType, time: number): ImageData {
  const canvas = document.createElement('canvas');
  canvas.width = DISPLAY_WIDTH;
  canvas.height = DISPLAY_HEIGHT;
  const ctx = canvas.getContext('2d')!;

  switch (demo) {
    case 'rainbow':
      return demoRainbow(ctx, time);
    case 'gradient':
      return demoGradient(ctx, time);
    case 'plasma':
      return demoPlasma(ctx, time);
    case 'fire':
      return demoFire(ctx, time);
    case 'matrix':
      return demoMatrix(ctx, time);
    case 'clock':
      return demoClock(ctx, time);
    default:
      return ctx.getImageData(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
  }
}

function hsvToRgb(h: number, s: number, v: number): [number, number, number] {
  const i = Math.floor(h * 6);
  const f = h * 6 - i;
  const p = v * (1 - s);
  const q = v * (1 - f * s);
  const t = v * (1 - (1 - f) * s);

  const mod = i % 6;
  const r = [v, q, p, p, t, v][mod];
  const g = [t, v, v, q, p, p][mod];
  const b = [p, p, t, v, v, q][mod];

  return [Math.round(r * 255), Math.round(g * 255), Math.round(b * 255)];
}

function demoRainbow(ctx: CanvasRenderingContext2D, t: number): ImageData {
  const imageData = ctx.createImageData(DISPLAY_WIDTH, DISPLAY_HEIGHT);
  const data = imageData.data;

  for (let x = 0; x < DISPLAY_WIDTH; x++) {
    const hue = (t * 0.5 + x / DISPLAY_WIDTH) % 1.0;
    const [r, g, b] = hsvToRgb(hue, 1.0, 1.0);

    for (let y = 0; y < DISPLAY_HEIGHT; y++) {
      const i = (y * DISPLAY_WIDTH + x) * 4;
      data[i] = r;
      data[i + 1] = g;
      data[i + 2] = b;
      data[i + 3] = 255;
    }
  }

  return imageData;
}

function demoGradient(ctx: CanvasRenderingContext2D, t: number): ImageData {
  const imageData = ctx.createImageData(DISPLAY_WIDTH, DISPLAY_HEIGHT);
  const data = imageData.data;

  for (let y = 0; y < DISPLAY_HEIGHT; y++) {
    for (let x = 0; x < DISPLAY_WIDTH; x++) {
      const hue = (t * 0.2 + x / DISPLAY_WIDTH * 0.5 + y / DISPLAY_HEIGHT * 0.5) % 1.0;
      const [r, g, b] = hsvToRgb(hue, 1.0, 1.0);

      const i = (y * DISPLAY_WIDTH + x) * 4;
      data[i] = r;
      data[i + 1] = g;
      data[i + 2] = b;
      data[i + 3] = 255;
    }
  }

  return imageData;
}

function demoPlasma(ctx: CanvasRenderingContext2D, t: number): ImageData {
  const imageData = ctx.createImageData(DISPLAY_WIDTH, DISPLAY_HEIGHT);
  const data = imageData.data;

  for (let y = 0; y < DISPLAY_HEIGHT; y++) {
    for (let x = 0; x < DISPLAY_WIDTH; x++) {
      const v1 = Math.sin(x / 16.0 + t * 2);
      const v2 = Math.sin((y / 8.0 + t) * 2);
      const v3 = Math.sin((x / 16.0 + y / 16.0 + t) * 2);
      const v4 = Math.sin(Math.sqrt(x * x + y * y) / 8.0 + t * 3);

      const v = (v1 + v2 + v3 + v4) / 4.0;
      const hue = (v + 1.0) / 2.0;

      const [r, g, b] = hsvToRgb(hue, 1.0, 1.0);

      const i = (y * DISPLAY_WIDTH + x) * 4;
      data[i] = r;
      data[i + 1] = g;
      data[i + 2] = b;
      data[i + 3] = 255;
    }
  }

  return imageData;
}

let fireBuffer: Float32Array | null = null;

function demoFire(ctx: CanvasRenderingContext2D, t: number): ImageData {
  if (!fireBuffer) {
    fireBuffer = new Float32Array((DISPLAY_HEIGHT + 2) * DISPLAY_WIDTH);
  }

  const imageData = ctx.createImageData(DISPLAY_WIDTH, DISPLAY_HEIGHT);
  const data = imageData.data;

  // Generate heat at bottom
  for (let x = 0; x < DISPLAY_WIDTH; x++) {
    fireBuffer[(DISPLAY_HEIGHT + 1) * DISPLAY_WIDTH + x] = Math.random() * 255;
  }

  // Propagate fire upward
  for (let y = DISPLAY_HEIGHT; y > 0; y--) {
    for (let x = 0; x < DISPLAY_WIDTH; x++) {
      const decay = Math.random() * 3;
      const x1 = Math.max(0, x - 1);
      const x2 = Math.min(DISPLAY_WIDTH - 1, x + 1);

      const avg =
        (fireBuffer[y * DISPLAY_WIDTH + x1] +
          fireBuffer[y * DISPLAY_WIDTH + x] +
          fireBuffer[y * DISPLAY_WIDTH + x2] +
          fireBuffer[(y + 1) * DISPLAY_WIDTH + x]) /
        4.0;

      fireBuffer[(y - 1) * DISPLAY_WIDTH + x] = Math.max(0, avg - decay);
    }
  }

  // Convert to RGB
  for (let y = 0; y < DISPLAY_HEIGHT; y++) {
    for (let x = 0; x < DISPLAY_WIDTH; x++) {
      const v = Math.floor(fireBuffer[y * DISPLAY_WIDTH + x]);
      let r = 0, g = 0, b = 0;

      if (v < 85) {
        r = v * 3;
      } else if (v < 170) {
        r = 255;
        g = (v - 85) * 3;
      } else {
        r = 255;
        g = 255;
        b = (v - 170) * 3;
      }

      const i = (y * DISPLAY_WIDTH + x) * 4;
      data[i] = r;
      data[i + 1] = g;
      data[i + 2] = b;
      data[i + 3] = 255;
    }
  }

  return imageData;
}

let matrixDrops: number[] | null = null;
let matrixSpeeds: number[] | null = null;

function demoMatrix(ctx: CanvasRenderingContext2D, t: number): ImageData {
  if (!matrixDrops) {
    matrixDrops = Array.from({ length: DISPLAY_WIDTH / 2 }, () =>
      Math.random() * DISPLAY_HEIGHT - DISPLAY_HEIGHT
    );
    matrixSpeeds = Array.from({ length: DISPLAY_WIDTH / 2 }, () =>
      Math.random() * 2 + 1
    );
  }

  const imageData = ctx.createImageData(DISPLAY_WIDTH, DISPLAY_HEIGHT);
  const data = imageData.data;

  // Clear to black
  data.fill(0);

  for (let i = 0; i < matrixDrops.length; i++) {
    const x = i * 2;
    const dropY = matrixDrops[i];
    const speed = matrixSpeeds[i];

    // Draw trail
    for (let j = 0; j < Math.min(10, DISPLAY_HEIGHT); j++) {
      const y = Math.floor(dropY) - j;
      if (y >= 0 && y < DISPLAY_HEIGHT) {
        const brightness = Math.max(0, 255 - j * 25);
        const idx = (y * DISPLAY_WIDTH + x) * 4;
        data[idx] = 0;
        data[idx + 1] = brightness;
        data[idx + 2] = 0;
        data[idx + 3] = 255;
      }
    }

    // Update position
    matrixDrops[i] = dropY + speed * 0.5;
    if (matrixDrops[i] > DISPLAY_HEIGHT + 10) {
      matrixDrops[i] = Math.random() * -10;
      matrixSpeeds[i] = Math.random() * 2 + 1;
    }
  }

  return imageData;
}

function demoClock(ctx: CanvasRenderingContext2D, t: number): ImageData {
  ctx.fillStyle = 'black';
  ctx.fillRect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);

  const now = new Date();
  const timeStr = now.toLocaleTimeString('ja-JP', { hour12: false });

  // Pulsing color
  const hue = (t * 0.1) % 1.0;
  const [r, g, b] = hsvToRgb(hue, 0.7, 1.0);

  ctx.font = 'bold 10px monospace';
  ctx.fillStyle = `rgb(${r}, ${g}, ${b})`;
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  ctx.fillText(timeStr, DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2);

  return ctx.getImageData(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
}
