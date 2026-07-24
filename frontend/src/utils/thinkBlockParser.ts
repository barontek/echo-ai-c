export function parseThinkBlocks(
  content: string
): Array<{ type: 'thinking' | 'content'; text: string }> {
  const blocks: Array<{ type: 'thinking' | 'content'; text: string }> = [];
  let remaining = content;
  while (remaining.length) {
    const thinkStart = remaining.indexOf('<think>');
    if (thinkStart === -1) {
      if (remaining.trim()) blocks.push({ type: 'content', text: remaining });
      break;
    }
    if (thinkStart > 0) {
      blocks.push({ type: 'content', text: remaining.slice(0, thinkStart) });
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
