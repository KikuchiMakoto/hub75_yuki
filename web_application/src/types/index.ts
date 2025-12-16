export const DISPLAY_WIDTH = 128;
export const DISPLAY_HEIGHT = 32;

export type DemoType = 'rainbow' | 'gradient' | 'plasma' | 'fire' | 'matrix' | 'clock';

export interface SerialDeviceOptions {
  baudRate?: number;
}

export interface LEDMatrixController {
  connect(): Promise<void>;
  disconnect(): Promise<void>;
  sendFrame(imageData: ImageData): Promise<boolean>;
  isConnected(): boolean;
}
