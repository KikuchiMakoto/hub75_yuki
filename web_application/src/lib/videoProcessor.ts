import { FFmpeg } from '@ffmpeg/ffmpeg';
import { fetchFile, toBlobURL } from '@ffmpeg/util';
import { DISPLAY_WIDTH, DISPLAY_HEIGHT } from '../types';

/**
 * Video processor using ffmpeg.wasm
 * Converts videos to OpenCV2-friendly formats (128x32 or 64x64)
 */
export class VideoProcessor {
  private ffmpeg: FFmpeg | null = null;
  private loaded: boolean = false;

  /**
   * Load FFmpeg.wasm
   */
  async load(): Promise<void> {
    if (this.loaded) return;

    this.ffmpeg = new FFmpeg();

    // Load FFmpeg from CDN
    const baseURL = 'https://unpkg.com/@ffmpeg/core@0.12.6/dist/esm';

    await this.ffmpeg.load({
      coreURL: await toBlobURL(`${baseURL}/ffmpeg-core.js`, 'text/javascript'),
      wasmURL: await toBlobURL(`${baseURL}/ffmpeg-core.wasm`, 'application/wasm'),
    });

    this.loaded = true;
  }

  /**
   * Resize video to target resolution using ffmpeg.wasm
   * Converts to ts2 (MPEG-2 TS) format for better OpenCV2 compatibility
   *
   * @param file - Input video file
   * @param width - Target width (default: 128)
   * @param height - Target height (default: 32)
   * @returns Processed video as Blob
   */
  async resizeVideo(
    file: File,
    width: number = DISPLAY_WIDTH,
    height: number = DISPLAY_HEIGHT
  ): Promise<Blob> {
    if (!this.ffmpeg || !this.loaded) {
      throw new Error('FFmpeg not loaded. Call load() first.');
    }

    // Write input file to FFmpeg virtual filesystem
    await this.ffmpeg.writeFile('input.mp4', await fetchFile(file));

    // Resize video with proper aspect ratio handling
    // Using libx264 codec for better compatibility
    // scale filter maintains aspect ratio and pads with black
    // -r 18 limits fps to MAX_VIDEO_FPS (18)
    await this.ffmpeg.exec([
      '-i', 'input.mp4',
      '-vf', `scale=${width}:${height}:force_original_aspect_ratio=decrease,pad=${width}:${height}:(ow-iw)/2:(oh-ih)/2:black`,
      '-r', '18',  // Limit to 18 fps
      '-c:v', 'libx264',
      '-preset', 'ultrafast',
      '-crf', '23',
      '-pix_fmt', 'yuv420p',
      '-an',  // Remove audio
      'output.mp4'
    ]);

    // Read output file
    const data = await this.ffmpeg.readFile('output.mp4');

    // Clean up virtual filesystem
    await this.ffmpeg.deleteFile('input.mp4');
    await this.ffmpeg.deleteFile('output.mp4');

    // Convert to Blob
    return new Blob([data], { type: 'video/mp4' });
  }

  /**
   * Check if video needs resizing
   */
  async shouldResize(file: File): Promise<boolean> {
    return new Promise((resolve) => {
      const video = document.createElement('video');
      video.preload = 'metadata';

      video.onloadedmetadata = () => {
        URL.revokeObjectURL(video.src);

        // Resize if dimensions don't match or if fps > 18
        const needsResize =
          video.videoWidth !== DISPLAY_WIDTH ||
          video.videoHeight !== DISPLAY_HEIGHT;

        resolve(needsResize);
      };

      video.onerror = () => {
        URL.revokeObjectURL(video.src);
        resolve(true); // Assume resize needed on error
      };

      video.src = URL.createObjectURL(file);
    });
  }

  /**
   * Get video metadata
   */
  async getVideoMetadata(file: File): Promise<{
    width: number;
    height: number;
    duration: number;
  }> {
    return new Promise((resolve, reject) => {
      const video = document.createElement('video');
      video.preload = 'metadata';

      video.onloadedmetadata = () => {
        const metadata = {
          width: video.videoWidth,
          height: video.videoHeight,
          duration: video.duration,
        };
        URL.revokeObjectURL(video.src);
        resolve(metadata);
      };

      video.onerror = () => {
        URL.revokeObjectURL(video.src);
        reject(new Error('Failed to load video metadata'));
      };

      video.src = URL.createObjectURL(file);
    });
  }
}
