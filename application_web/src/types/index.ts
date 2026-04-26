export const DISPLAY_WIDTH = 128;
export const DISPLAY_HEIGHT = 32;

export type DemoType = 'rainbow' | 'gradient' | 'plasma' | 'fire' | 'matrix' | 'clock';

export type TransportType = 'webserial' | 'webusb';

/**
 * Common transport interface used by both Web Serial and Web USB backends.
 */
export interface DeviceTransport {
  readonly type: TransportType;
  connect(): Promise<void>;
  disconnect(): Promise<void>;
  write(data: Uint8Array): Promise<void>;
  isConnected(): boolean;
}

/**
 * High-level controller interface exposed to UI components.
 */
export interface LEDMatrixController {
  connect(): Promise<void>;
  disconnect(): Promise<void>;
  sendFrame(imageData: ImageData): Promise<boolean>;
  isConnected(): boolean;
  getMetrics(): { sent: number; dropped: number; errors: number };
  resetMetrics(): void;
}
