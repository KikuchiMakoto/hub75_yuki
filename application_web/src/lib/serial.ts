import { cobsEncode } from './cobs';
import { DISPLAY_WIDTH, DISPLAY_HEIGHT } from '../types';
import type { LEDMatrixController } from '../types';

/**
 * Lazy-load the web-serial-polyfill only when Web Serial API is missing.
 * This keeps the bundle smaller for browsers that natively support it.
 */
let webSerialPolyfill: unknown = null;
async function getSerialApi(): Promise<unknown> {
  if ('serial' in navigator && navigator.serial) {
    return navigator.serial;
  }
  // Fallback: web-serial-polyfill uses WebUSB to talk to CDC-ACM devices.
  // This works on Android Chrome where OS does not bind a CDC driver.
  if (!webSerialPolyfill) {
    const mod = await import('web-serial-polyfill');
    webSerialPolyfill = mod.serial;
  }
  return webSerialPolyfill;
}

/**
 * Web Serial API transport for HUB75 LED Matrix communication.
 *
 * Design notes:
 * - Uses native Web Serial API when available; falls back to web-serial-polyfill
 *   (WebUSB-based) on browsers without native support (e.g. Android Chrome).
 * - USB CDC on RP2040 ignores baud rate, but we keep the default 115200 for
 *   compatibility with physical UART bridges if they are ever used.
 * - We assert DTR after open because some CDC stacks stall TX until DTR is set.
 * - A single-frame overwrite queue is used: if the previous frame is still
 *   being encoded / handed to the browser, the newest frame wins.  This
 *   keeps latency minimal and avoids stale-frame pile-up.
 * - Canvas elements are reused to reduce GC pressure in animation loops.
 */
export class WebSerialTransport implements LEDMatrixController {
  private port: SerialPort | null = null;
  private writer: WritableStreamDefaultWriter<Uint8Array> | null = null;

  // Reusable off-screen canvases (lazy-initialised)
  private srcCanvas: HTMLCanvasElement | null = null;
  private srcCtx: CanvasRenderingContext2D | null = null;
  private resizeCanvas: HTMLCanvasElement | null = null;
  private resizeCtx: CanvasRenderingContext2D | null = null;
  private flipCanvas: HTMLCanvasElement | null = null;
  private flipCtx: CanvasRenderingContext2D | null = null;

  // Single-frame overwrite slot (null = no pending frame)
  private pendingFrame: ImageData | null = null;

  // true while an async send is in flight
  private sending = false;

  // Metrics
  private _sent = 0;
  private _dropped = 0;
  private _errors = 0;

  async connect(): Promise<void> {
    const serialApi = await getSerialApi() as any;

    // Request a port.  Filtering by Raspberry Pi VID makes the picker friendlier.
    const port = await serialApi.requestPort({
      filters: [
        { usbVendorId: 0x2e8a }, // Raspberry Pi Foundation
        { usbVendorId: 0x2e8a, usbProductId: 0x0101 }, // HUB75 Controller v2
      ],
    }) as any;

    // Open with explicit settings.  bufferSize reduces the chance of
    // browser-level back-pressure stalling animation loops.
    await port.open({
      baudRate: 115200,
      dataBits: 8,
      stopBits: 1,
      parity: 'none',
      flowControl: 'none',
      bufferSize: 16384,
    });

    // Some CDC stacks (including certain TinyUSB builds) will not begin
    // sending data upstream until DTR is asserted by the host.
    if (typeof port.setSignals === 'function') {
      await port.setSignals({
        dataTerminalReady: true,
        requestToSend: false,
      });
    }

    if (!port.writable) {
      await port.close();
      throw new Error('Port opened but has no writable stream');
    }

    this.port = port as SerialPort;
    this.writer = port.writable.getWriter();
    console.log('[WebSerial] Connected');
  }

  async disconnect(): Promise<void> {
    // Abort any in-flight encoding / write logic by dropping the pending slot
    this.pendingFrame = null;

    // Give the writer a moment to drain if it is mid-flight
    if (this.writer) {
      try {
        await this.writer.ready;
      } catch {
        // ignore
      }
      this.writer.releaseLock();
      this.writer = null;
    }

    if (this.port) {
      try {
        await this.port.close();
      } catch {
        // ignore
      }
      this.port = null;
    }

    this.sending = false;
    this.pendingFrame = null;
    console.log('[WebSerial] Disconnected');
  }

  isConnected(): boolean {
    return this.port !== null && this.writer !== null;
  }

  /**
   * Encode an ImageData frame into a COBS + 0x00 packet and hand it to the
   * transport.  If a previous frame is still being processed, the new frame
   * overwrites it (latest-frame-wins).  This prevents stale-frame backlog.
   */
  async sendFrame(imageData: ImageData): Promise<boolean> {
    if (!this.writer) {
      return false;
    }

    // If we are currently encoding / writing, stash the new frame and bail.
    // The in-flight send will pick up the pending frame afterwards.
    if (this.sending) {
      this.pendingFrame = imageData;
      this._dropped++;
      return true;
    }

    this.sending = true;

    try {
      const packet = this.encodeFrame(imageData);
      await this.write(packet);
      this._sent++;

      // If another frame arrived while we were busy, send it now.
      while (this.pendingFrame) {
        const next = this.pendingFrame;
        this.pendingFrame = null;
        const nextPacket = this.encodeFrame(next);
        await this.write(nextPacket);
        this._sent++;
      }

      return true;
    } catch (err) {
      console.error('[WebSerial] sendFrame error:', err);
      this._errors++;
      this.pendingFrame = null;
      return false;
    } finally {
      this.sending = false;
    }
  }

  /**
   * Low-level write.  Waits for back-pressure to clear before writing.
   */
  async write(data: Uint8Array): Promise<void> {
    if (!this.writer) {
      throw new Error('Not connected');
    }
    await this.writer.ready;
    await this.writer.write(data);
  }

  getMetrics(): { sent: number; dropped: number; errors: number } {
    return { sent: this._sent, dropped: this._dropped, errors: this._errors };
  }

  resetMetrics(): void {
    this._sent = 0;
    this._dropped = 0;
    this._errors = 0;
  }

  // ------------------------------------------------------------------
  // Private helpers
  // ------------------------------------------------------------------

  /**
   * Full pipeline: prepare (resize + flip) -> RGB565 -> COBS -> 0x00
   */
  private encodeFrame(imageData: ImageData): Uint8Array {
    const prepared = this.prepareImage(imageData);
    const rgb565 = this.imageDataToRGB565(prepared);
    const encoded = cobsEncode(rgb565);

    const packet = new Uint8Array(encoded.length + 1);
    packet.set(encoded, 0);
    packet[encoded.length] = 0x00;
    return packet;
  }

  /**
   * Resize image to display dimensions and horizontally flip it for the
   * HUB75 shift-register order.  Reuses canvas elements to avoid GC churn.
   */
  private prepareImage(imageData: ImageData): ImageData {
    const { width: w, height: h } = imageData;
    const needsResize = w !== DISPLAY_WIDTH || h !== DISPLAY_HEIGHT;

    // Lazy-init source canvas
    if (!this.srcCanvas) {
      this.srcCanvas = document.createElement('canvas');
      this.srcCtx = this.srcCanvas.getContext('2d', {
        willReadFrequently: true,
      })!;
    }
    this.srcCanvas.width = w;
    this.srcCanvas.height = h;
    this.srcCtx!.putImageData(imageData, 0, 0);

    let srcForFlip: CanvasImageSource = this.srcCanvas;

    if (needsResize) {
      if (!this.resizeCanvas) {
        this.resizeCanvas = document.createElement('canvas');
        this.resizeCanvas.width = DISPLAY_WIDTH;
        this.resizeCanvas.height = DISPLAY_HEIGHT;
        this.resizeCtx = this.resizeCanvas.getContext('2d', {
          willReadFrequently: true,
        })!;
      }
      this.resizeCtx!.clearRect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
      this.resizeCtx!.drawImage(
        this.srcCanvas,
        0,
        0,
        w,
        h,
        0,
        0,
        DISPLAY_WIDTH,
        DISPLAY_HEIGHT
      );
      srcForFlip = this.resizeCanvas;
    }

    if (!this.flipCanvas) {
      this.flipCanvas = document.createElement('canvas');
      this.flipCanvas.width = DISPLAY_WIDTH;
      this.flipCanvas.height = DISPLAY_HEIGHT;
      this.flipCtx = this.flipCanvas.getContext('2d', {
        willReadFrequently: true,
      })!;
    }
    this.flipCtx!.clearRect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    this.flipCtx!.save();
    this.flipCtx!.scale(-1, 1);
    this.flipCtx!.drawImage(srcForFlip, -DISPLAY_WIDTH, 0);
    this.flipCtx!.restore();

    return this.flipCtx!.getImageData(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
  }

  private imageDataToRGB565(imageData: ImageData): Uint8Array {
    const { data, width, height } = imageData;
    const rgb565 = new Uint8Array(width * height * 2);

    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const i = (y * width + x) * 4;
        const r = data[i]! >> 3;
        const g = data[i + 1]! >> 2;
        const b = data[i + 2]! >> 3;
        const val = (r << 11) | (g << 5) | b;
        const o = (y * width + x) * 2;
        rgb565[o] = val & 0xff;
        rgb565[o + 1] = (val >> 8) & 0xff;
      }
    }
    return rgb565;
  }
}

/**
 * Convenience wrapper that exposes the same API as the original SerialDevice
 * so existing UI code needs no changes.
 */
export class SerialDevice extends WebSerialTransport {}
