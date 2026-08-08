import { memo } from 'react';
import type { BranchInfo } from '../types';

/* Branch-switch pill for a fork point: `‹ 1/k ›`. Hidden when the chain has
 * no alternatives (count <= 1); arrows disabled at the bounds (no
 * wrap-around). Clicking an arrow moves the active chain one step. */
export const BranchPill = memo(function BranchPill({
  info,
  onSwitch,
}: {
  info: BranchInfo;
  onSwitch: (direction: -1 | 1) => void;
}) {
  if (!info || info.count <= 1) return null;

  const leftDisabled = info.active <= 1;
  const rightDisabled = info.active >= info.count;

  return (
    <span className="branch-pill" title={`${info.active} of ${info.count} branches`}>
      <button
        className="branch-pill-arrow"
        disabled={leftDisabled}
        onClick={() => onSwitch(-1)}
        aria-label="Previous branch"
      >
        ‹
      </button>
      <span className="branch-pill-label">
        {info.active}/{info.count}
      </span>
      <button
        className="branch-pill-arrow"
        disabled={rightDisabled}
        onClick={() => onSwitch(1)}
        aria-label="Next branch"
      >
        ›
      </button>
    </span>
  );
});
