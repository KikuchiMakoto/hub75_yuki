import { cobsEncode } from './cobs';
import { DISPLAY_WIDTH, DISPLAY_HEIGHT } from '../types';

/**
 * Web Serial API wrapper for HUB75 LED Matrix communication
 */
export class SerialDevice {
  private port: SerialPort | null = null;
  private writer: WritableStreamDefaultWriter<Uint8Array> | null = null;
  private reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  private sending: boolean = false;

  async connect(baudRate: number = 115200): Promise<void> {
    if (!('serial' in navigator)) {
      throw new Error('Web Serial API is not supported in this browser');
    }

    try {
      // Request port from user
      this.port = await navigator.serial.requestPort();

      // Open port
      await this.port.open({ baudRate });

      // Get writer for sending data
      if (this.port.writable) {
        this.writer = this.port.writable.getWriter();
      }

      console.log('Connected to serial device');
    } catch (error) {
      console.error('Failed to connect:', error);
      throw error;
    }
  }

  async disconnect(): Promise<void> {
    if (this.writer) {
      this.writer.releaseLock();
      this.writer = null;
    }

    if (this.reader) {
      this.reader.releaseLock();
      this.reader = null;
    }

    if (this.port) {
      await this.port.close();
      this.port = null;
    }

    console.log('Disconnected from serial device');
  }

  isConnected(): boolean {
    return this.port !== null && this.writer !== null;
  }

  /**
   * Convert RGBA ImageData to RGB565 format
   */
  private imageDataToRGB565(imageData: ImageData): Uint8Array {
    const { data, width, height } = imageData;
    const rgb565 = new Uint8Array(width * height * 2);

    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const i = (y * width + x) * 4;
        const r = data[i] >> 3;     // 8-bit to 5-bit
        const g = data[i + 1] >> 2; // 8-bit to 6-bit
        const b = data[i + 2] >> 3; // 8-bit to 5-bit

        const rgb565Value = (r << 11) | (g << 5) | b;

        // Little-endian
        const outputIndex = (y * width + x) * 2;
        rgb565[outputIndex] = rgb565Value & 0xFF;
        rgb565[outputIndex + 1] = (rgb565Value >> 8) & 0xFF;
      }
    }

    return rgb565;
  }

  /**
   * Resize and flip image to display dimensions
   */
  private prepareImage(imageData: ImageData): ImageData {
    const canvas = document.createElement('canvas');
    canvas.width = DISPLAY_WIDTH;
    canvas.height = DISPLAY_HEIGHT;
    const ctx = canvas.getContext('2d')!;

    // Draw and resize image
    ctx.drawImage(
      createImageBitmap(imageData) as any,
      0,
      0,
      imageData.width,
      imageData.height,
      0,
      0,
      DISPLAY_WIDTH,
      DISPLAY_HEIGHT
    );

    // Horizontal flip for HUB75 shift register order
    const flippedCanvas = document.createElement('canvas');
    flippedCanvas.width = DISPLAY_WIDTH;
    flippedCanvas.height = DISPLAY_HEIGHT;
    const flippedCtx = flippedCanvas.getContext('2d')!;
    flippedCtx.scale(-1, 1);
    flippedCtx.drawImage(canvas, -DISPLAY_WIDTH, 0);

    return flippedCtx.getImageData(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
  }

  /**
   * Check if currently sending a frame
   */
  isSending(): boolean {
    return this.sending;
  }

  /**
   * Send a frame to the display
   * Returns false if already sending (frame drop) or on error
   */
  async sendFrame(imageData: ImageData): Promise<boolean> {
    if (!this.writer) {
      throw new Error('Not connected to serial device');
    }

    // Frame drop: skip if previous send is still in progress
    if (this.sending) {
      return false;
    }

    this.sending = true;

    try {
      // Prepare image (resize and flip)
      const prepared = this.prepareImage(imageData);

      // Convert to RGB565
      const rgb565Data = this.imageDataToRGB565(prepared);

      // COBS encode
      const encoded = cobsEncode(rgb565Data);

      // Add terminator
      const packet = new Uint8Array(encoded.length + 1);
      packet.set(encoded, 0);
      packet[encoded.length] = 0x00;

      // Send
      await this.writer.write(packet);

      return true;
    } catch (error) {
      console.error('Failed to send frame:', error);
      return false;
    } finally {
      this.sending = false;
    }
  }
}
