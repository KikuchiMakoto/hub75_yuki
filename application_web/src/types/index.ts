export const DISPLAY_WIDTH = 128;
export const DISPLAY_HEIGHT = 32;

export type DemoType = 'rainbow' | 'gradient' | 'plasma' | 'fire' | 'matrix' | 'clock';

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
