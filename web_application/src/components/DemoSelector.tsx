import React from 'react';
import type { DemoType } from '../types';

interface DemoSelectorProps {
  onSelectDemo: (demo: DemoType) => void;
  disabled?: boolean;
}

const demos: { type: DemoType; name: string; description: string }[] = [
  { type: 'rainbow', name: 'Rainbow', description: '虹のグラデーション' },
  { type: 'gradient', name: 'Gradient', description: 'カラーグラデーション' },
  { type: 'plasma', name: 'Plasma', description: 'プラズマエフェクト' },
  { type: 'fire', name: 'Fire', description: '炎のエフェクト' },
  { type: 'matrix', name: 'Matrix', description: 'マトリックス風' },
  { type: 'clock', name: 'Clock', description: 'デジタル時計' },
];

export const DemoSelector: React.FC<DemoSelectorProps> = ({ onSelectDemo, disabled }) => {
  return (
    <div className="grid grid-cols-2 md:grid-cols-3 gap-3">
      {demos.map((demo) => (
        <button
          key={demo.type}
          onClick={() => onSelectDemo(demo.type)}
          disabled={disabled}
          className={`
            p-4 rounded-lg border-2 transition-all
            ${disabled ? 'opacity-50 cursor-not-allowed' : 'hover:border-blue-500 hover:shadow-md cursor-pointer'}
            border-gray-200 bg-white
          `}
        >
          <div className="font-semibold text-lg mb-1">{demo.name}</div>
          <div className="text-sm text-gray-600">{demo.description}</div>
        </button>
      ))}
    </div>
  );
};
