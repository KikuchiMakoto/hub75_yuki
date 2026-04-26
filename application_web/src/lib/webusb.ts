import { cobsEncode } from './cobs';
import { DISPLAY_WIDTH, DISPLAY_HEIGHT } from '../types';
import type { DeviceTransport } from '../types';

/**
 * WebUSB transport for HUB75 LED Matrix communication.
 *
 * IMPORTANT: This transport requires the RP2040 firmware to expose a
 * Vendor-specific USB interface with a single bulk OUT endpoint (and
 * optionally a bulk IN endpoint).  The standard CDC ACM interface used by
 * the current firmware_v2/v3 cannot be accessed via WebUSB.
 *
 * When the firmware is updated to include a WebUSB vendor interface,
 * this class provides a low-latency fallback for browsers that do not
 * support Web Serial (e.g. Firefox) or for users who prefer WebUSB.
 *
 * Protocol over the wire is identical to the serial path:
 *   RGB565 little-endian -> COBS -> trailing 0x00
 */
export class WebUSBTransport implements DeviceTransport {
  readonly type = 'webusb' as const;

  private device: USBDevice | null = null;
  private ifaceNum = -1;
  private epOut = -1;
  private epIn = -1;

  // Reusable canvases (same as WebSerialTransport)
  private srcCanvas: HTMLCanvasElement | null = null;
  private srcCtx: CanvasRenderingContext2D | null = null;
  private resizeCanvas: HTMLCanvasElement | null = null;
  private resizeCtx: CanvasRenderingContext2D | null = null;
  private flipCanvas: HTMLCanvasElement | null = null;
  private flipCtx: CanvasRenderingContext2D | null = null;

  // Single-frame overwrite slot
  private pendingFrame: ImageData | null = null;
  private sending = false;

  private _sent = 0;
  private _dropped = 0;
  private _errors = 0;

  async connect(): Promise<void> {
    if (!('usb' in navigator)) {
      throw new Error(
        'WebUSB is not supported in this browser. ' +
          'Please use Chrome or Edge.'
      );
    }

    const device = await navigator.usb.requestDevice({
      filters: [
        { vendorId: 0x2e8a }, // Raspberry Pi Foundation
        { vendorId: 0x2e8a, productId: 0x0101 },
      ],
    });

    await device.open();

    if (device.configuration === null) {
      await device.selectConfiguration(1);
    }

    // Locate the vendor-class interface (bInterfaceClass == 0xFF)
    let claimed = false;
    const config = device.configuration!;
    for (const iface of config.interfaces) {
      const alt = iface.alternate;
      if (alt.interfaceClass === 0xff) {
        await device.claimInterface(iface.interfaceNumber);
        this.ifaceNum = iface.interfaceNumber;

        for (const ep of alt.endpoints) {
          if (ep.direction === 'out') this.epOut = ep.endpointNumber;
          if (ep.direction === 'in') this.epIn = ep.endpointNumber;
        }
        claimed = true;
        break;
      }
    }

    if (!claimed) {
      await device.close();
      throw new Error(
        'No WebUSB vendor interface found on this device. ' +
          'Please flash firmware with WebUSB support.'
      );
    }

    if (this.epOut < 0) {
      await device.releaseInterface(this.ifaceNum);
      await device.close();
      throw new Error('Vendor interface has no OUT endpoint');
    }

    this.device = device;
    console.log('[WebUSB] Connected');
  }

  async disconnect(): Promise<void> {
    this.pendingFrame = null;
    const dev = this.device;
    this.device = null;
    this.epOut = -1;
    this.epIn = -1;
    this.sending = false;

    if (dev) {
      try {
        if (this.ifaceNum >= 0) {
          await dev.releaseInterface(this.ifaceNum);
        }
      } catch {
        // ignore
      }
      try {
        await dev.close();
      } catch {
        // ignore
      }
    }
    this.ifaceNum = -1;
    console.log('[WebUSB] Disconnected');
  }

  isConnected(): boolean {
    return this.device !== null && this.epOut >= 0;
  }

  async sendFrame(imageData: ImageData): Promise<boolean> {
    if (!this.device) return false;

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

      while (this.pendingFrame) {
        const next = this.pendingFrame;
        this.pendingFrame = null;
        const nextPacket = this.encodeFrame(next);
        await this.write(nextPacket);
        this._sent++;
      }
      return true;
    } catch (err) {
      console.error('[WebUSB] sendFrame error:', err);
      this._errors++;
      this.pendingFrame = null;
      return false;
    } finally {
      this.sending = false;
    }
  }

  async write(data: Uint8Array): Promise<void> {
    if (!this.device || this.epOut < 0) {
      throw new Error('Not connected');
    }
    // WebUSB transferOut accepts ArrayBuffer / ArrayBufferView directly.
    const result = await this.device.transferOut(this.epOut, data);
    if (result.status !== 'ok') {
      throw new Error(`WebUSB transfer failed: ${result.status}`);
    }
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
  // Private helpers (duplicated from WebSerialTransport – shared later)
  // ------------------------------------------------------------------

  private encodeFrame(imageData: ImageData): Uint8Array {
    const prepared = this.prepareImage(imageData);
    const rgb565 = this.imageDataToRGB565(prepared);
    const encoded = cobsEncode(rgb565);
    const packet = new Uint8Array(encoded.length + 1);
    packet.set(encoded, 0);
    packet[encoded.length] = 0x00;
    return packet;
  }

  private prepareImage(imageData: ImageData): ImageData {
    const { width: w, height: h } = imageData;
    const needsResize = w !== DISPLAY_WIDTH || h !== DISPLAY_HEIGHT;

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
