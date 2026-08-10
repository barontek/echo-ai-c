function trimPartialTag(s: string): string {
  const lastLt = s.lastIndexOf('<');
  if (lastLt === -1) return s;
  const tail = s.slice(lastLt);
  if (tail === '<think>' || tail === '</think>') return s;
  if ('<think>'.startsWith(tail) || '</think>'.startsWith(tail)) {
    return s.slice(0, lastLt);
  }
  return s;
}

/** parseThinkBlocks - split raw text into visible + thinking segments.
 *
 * Pure function: returns a NEW array every call (the input string is
 * never mutated). Tolerates unclosed `<think>` tags by treating the rest
 * of the text as a thinking block. Never throws.
 *
 * @param content - The raw assistant text to parse.
 * @returns Array of { type: 'thinking' | 'content'; text: string }
 *   segments in document order; empty segments are omitted and an empty
 *   input yields an empty array.
 */
export function parseThinkBlocks(
  content: string
): Array<{ type: 'thinking' | 'content'; text: string }> {
  const blocks: Array<{ type: 'thinking' | 'content'; text: string }> = [];
  let remaining = content;
  while (remaining.length) {
    const thinkStart = remaining.indexOf('<think>');
    if (thinkStart === -1) {
      const trimmed = trimPartialTag(remaining);
      if (trimmed.trim()) blocks.push({ type: 'content', text: trimmed });
      break;
    }
    if (thinkStart > 0) {
      const trimmed = trimPartialTag(remaining.slice(0, thinkStart));
      if (trimmed.trim()) blocks.push({ type: 'content', text: trimmed });
    }
    const thinkEnd = remaining.indexOf('</think>', thinkStart);
    if (thinkEnd === -1) {
      const thinkContent = remaining.slice(thinkStart + 7);
      blocks.push({
        type: 'thinking',
        text: thinkContent.startsWith('\n') ? thinkContent.slice(1) : thinkContent,
      });
      break;
    }
    blocks.push({ type: 'thinking', text: remaining.slice(thinkStart + 7, thinkEnd) });
    remaining = remaining.slice(thinkEnd + 8);
  }
  return blocks;
}
