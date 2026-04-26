import React, { useCallback } from 'react';

interface DropZoneProps {
  onFileDrop: (file: File) => void;
  disabled?: boolean;
}

export const DropZone: React.FC<DropZoneProps> = ({ onFileDrop, disabled }) => {
  const [isDragging, setIsDragging] = React.useState(false);

  const handleDragOver = useCallback((e: React.DragEvent) => {
    e.preventDefault();
    if (!disabled) {
      setIsDragging(true);
    }
  }, [disabled]);

  const handleDragLeave = useCallback((e: React.DragEvent) => {
    e.preventDefault();
    setIsDragging(false);
  }, []);

  const handleDrop = useCallback(
    (e: React.DragEvent) => {
      e.preventDefault();
      setIsDragging(false);

      if (disabled) return;

      const files = Array.from(e.dataTransfer.files);
      const file = files[0];

      if (file && (file.type.startsWith('image/') || file.type.startsWith('video/'))) {
        onFileDrop(file);
      }
    },
    [onFileDrop, disabled]
  );

  const handleFileInput = useCallback(
    (e: React.ChangeEvent<HTMLInputElement>) => {
      const file = e.target.files?.[0];
      if (file) {
        onFileDrop(file);
      }
    },
    [onFileDrop]
  );

  return (
    <div
      onDragOver={handleDragOver}
      onDragLeave={handleDragLeave}
      onDrop={handleDrop}
      className={`
        border-2 border-dashed rounded-lg p-8 text-center transition-colors
        ${isDragging ? 'border-blue-500 bg-blue-50' : 'border-gray-300 bg-gray-50'}
        ${disabled ? 'opacity-50 cursor-not-allowed' : 'cursor-pointer hover:border-gray-400'}
      `}
    >
      <input
        type="file"
        accept="image/*,video/*"
        onChange={handleFileInput}
        disabled={disabled}
        className="hidden"
        id="file-input"
      />
      <label htmlFor="file-input" className={disabled ? 'cursor-not-allowed' : 'cursor-pointer'}>
        <div className="text-gray-600">
          <p className="text-lg font-semibold mb-2">
            画像または動画をドロップ
          </p>
          <p className="text-sm">
            クリックしてファイルを選択
          </p>
        </div>
      </label>
    </div>
  );
};
