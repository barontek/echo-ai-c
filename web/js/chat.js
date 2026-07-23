function parseThinkBlocks(content) {
    const parts = [];
    let remaining = content || '';
    const openTag = '<think>';
    const closeTag = '</think>';

    while (remaining.length > 0) {
        const openIdx = remaining.indexOf(openTag);
        if (openIdx === -1) {
            parts.push({ type: 'content', text: remaining });
            break;
        }
        if (openIdx > 0) parts.push({ type: 'content', text: remaining.slice(0, openIdx) });
        remaining = remaining.slice(openIdx + openTag.length);
        const closeIdx = remaining.indexOf(closeTag);
        if (closeIdx === -1) {
            parts.push({ type: 'thinking', text: remaining });
            break;
        }
        parts.push({ type: 'thinking', text: remaining.slice(0, closeIdx) });
        remaining = remaining.slice(closeIdx + closeTag.length);
    }
    return parts;
}

function escapeHtml(text) {
    const div = document.createElement('div');
    div.textContent = text;
    return div.innerHTML;
}

const Chat = {
    currentMsgEl: null,
    buffer: '',
    container: null,
    input: null,
    sendBtn: null,
    stopBtn: null,
    emptyEl: null,

    init() {
        this.container = document.getElementById('messages');
        this.input = document.getElementById('message-input');
        this.sendBtn = document.getElementById('send-btn');
        this.stopBtn = document.getElementById('stop-btn');

        this.sendBtn.onclick = () => this.send();
        this.stopBtn.onclick = () => this.stopGeneration();
        this.input.onkeydown = (e) => {
            if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); this.send(); }
            this.autoResize();
        };
        this.input.oninput = () => this.autoResize();

        this.renderEmptyState();
    },

    renderEmptyState() {
        if (this.emptyEl) return;
        this.emptyEl = document.createElement('div');
        this.emptyEl.className = 'empty-state';
        this.emptyEl.innerHTML = '<div class="empty-logo">✦</div><div class="empty-title">Echo AI</div><div class="empty-subtitle">Ask me anything. I\'m running locally.</div>';
        this.container.appendChild(this.emptyEl);
    },

    hideEmptyState() {
        if (this.emptyEl) { this.emptyEl.remove(); this.emptyEl = null; }
    },

    autoResize() {
        this.input.style.height = 'auto';
        this.input.style.height = Math.min(this.input.scrollHeight, 120) + 'px';
    },

    send() {
        const text = this.input.value.trim();
        if (!text || App.state.isStreaming) return;
        this.hideEmptyState();
        const msg = { role: 'user', content: text, id: 'msg_' + Date.now(), timestamp: new Date().toLocaleTimeString() };
        App.state.messages.push(msg);
        this.renderMessage(msg);
        this.scrollBottom();
        this.input.value = '';
        this.input.style.height = 'auto';
        App.state.isStreaming = true;
        this.setInputDisabled(true);
        WsClient.send(JSON.stringify({ type: 'message', message: text }));
    },

    stopGeneration() {
        WsClient.send(JSON.stringify({ type: 'stop' }));
        this.finalizeMessage();
    },

    renderMessage(msg) {
        const el = document.createElement('div');
        el.className = 'message ' + (msg.role === 'user' ? 'user' : 'assistant');
        el.dataset.id = msg.id || '';

        const bubble = document.createElement('div');
        bubble.className = 'message-bubble';

        if (msg.role === 'user') {
            const content = document.createElement('div');
            content.className = 'msg-content';
            content.textContent = msg.content || '';
            bubble.appendChild(content);
        } else {
            const parts = parseThinkBlocks(msg.content || '');
            for (const part of parts) {
                if (part.type === 'thinking') {
                    const details = document.createElement('details');
                    details.className = 'thinking-collapsible';
                    details.open = false;
                    const summary = document.createElement('summary');
                    summary.textContent = 'Thinking';
                    details.appendChild(summary);
                    const pre = document.createElement('pre');
                    pre.textContent = part.text;
                    details.appendChild(pre);
                    bubble.appendChild(details);
                } else {
                    const content = document.createElement('div');
                    content.className = 'msg-content';
                    content.innerHTML = this.renderMarkdown(part.text);
                    bubble.appendChild(content);
                }
            }
            if (msg.tool_calls && msg.tool_calls.length > 0) {
                const toolSection = document.createElement('div');
                toolSection.className = 'tool-calls';
                for (const tc of msg.tool_calls) {
                    const details = document.createElement('details');
                    details.className = 'tool-call';
                    const summary = document.createElement('summary');
                    summary.textContent = tc.name + '()';
                    details.appendChild(summary);
                    const args = document.createElement('pre');
                    args.className = 'tool-args';
                    args.textContent = JSON.stringify(tc.arguments || {}, null, 2);
                    details.appendChild(args);
                    if (tc.result) {
                        const result = document.createElement('pre');
                        result.className = 'tool-args';
                        result.style.borderTop = 'none';
                        result.style.background = 'var(--bg-primary)';
                        result.textContent = tc.result.content || tc.result.error || '';
                        details.appendChild(result);
                    }
                    toolSection.appendChild(details);
                }
                bubble.appendChild(toolSection);
            }
            if (msg.error) {
                const errEl = document.createElement('div');
                errEl.className = 'message-error';
                errEl.textContent = msg.error;
                bubble.appendChild(errEl);
            }
            const footer = document.createElement('div');
            footer.className = 'message-footer';
            if (msg.timestamp) {
                const time = document.createElement('span');
                time.className = 'msg-time';
                time.textContent = msg.timestamp;
                footer.appendChild(time);
            }
            const copyBtn = document.createElement('button');
            copyBtn.className = 'icon-btn';
            copyBtn.textContent = '📋';
            copyBtn.title = 'Copy';
            copyBtn.onclick = () => {
                navigator.clipboard.writeText(msg.content || '');
                copyBtn.textContent = '✓';
                setTimeout(() => { copyBtn.textContent = '📋'; }, 2000);
            };
            footer.appendChild(copyBtn);
            bubble.appendChild(footer);
        }

        el.appendChild(bubble);
        this.container.appendChild(el);

        if (msg.role === 'user' && App.state.messages.length > 0) {
            const idx = App.state.messages.length - 1;
            const editBtn = document.createElement('button');
            editBtn.className = 'icon-btn';
            editBtn.textContent = '✏️';
            editBtn.title = 'Edit';
            editBtn.onclick = () => this.startEdit(idx);
            const footer = el.querySelector('.message-footer');
            if (footer) footer.prepend(editBtn);
        }

        return el;
    },

    renderMarkdown(text) {
        let html = escapeHtml(text);
        html = html.replace(/^### (.+)$/gm, '<h3>$1</h3>');
        html = html.replace(/^## (.+)$/gm, '<h2>$1</h2>');
        html = html.replace(/^# (.+)$/gm, '<h1>$1</h1>');
        html = html.replace(/\*\*(.+?)\*\*/g, '<strong>$1</strong>');
        html = html.replace(/`([^`]+)`/g, '<code>$1</code>');
        html = html.replace(/```(\w*)\n?([\s\S]*?)```/g, '<pre><code>$2</code></pre>');
        html = html.replace(/^- (.+)$/gm, '<li>$1</li>');
        html = html.replace(/(<li>.*<\/li>)/gs, '<ul>$1</ul>');
        html = html.replace(/\n{2,}/g, '</p><p>');
        html = '<p>' + html + '</p>';
        html = html.replace(/<p><\/p>/g, '');
        html = html.replace(/<ul>\s*<\/ul>/g, '');
        html = html.replace(/<\/ul>\s*<ul>/g, '');
        return html;
    },

    startEdit(idx) {
        const msg = App.state.messages[idx];
        if (!msg || msg.role !== 'user') return;
        const el = this.container.querySelector(`.message:nth-child(${idx + 1})`);
        if (!el) return;
        const bubble = el.querySelector('.message-bubble');
        const oldContent = bubble.querySelector('.msg-content');
        if (oldContent) oldContent.style.display = 'none';
        const editOverlay = document.createElement('div');
        editOverlay.className = 'edit-overlay';
        const textarea = document.createElement('textarea');
        textarea.value = msg.content;
        textarea.rows = 3;
        textarea.style.width = '100%';
        textarea.onkeydown = (e) => {
            if (e.key === 'Enter' && e.ctrlKey) { e.preventDefault(); this.saveEdit(idx, textarea.value, editOverlay); }
            if (e.key === 'Escape') { editOverlay.remove(); if (oldContent) oldContent.style.display = ''; }
        };
        const actions = document.createElement('div');
        actions.className = 'edit-actions';
        const save = document.createElement('button');
        save.textContent = 'Save (Ctrl+Enter)';
        save.onclick = () => this.saveEdit(idx, textarea.value, editOverlay);
        const cancel = document.createElement('button');
        cancel.textContent = 'Cancel (Esc)';
        cancel.onclick = () => { editOverlay.remove(); if (oldContent) oldContent.style.display = ''; };
        actions.appendChild(save);
        actions.appendChild(cancel);
        editOverlay.appendChild(textarea);
        editOverlay.appendChild(actions);
        bubble.appendChild(editOverlay);
        textarea.focus();
    },

    saveEdit(idx, newText, overlay) {
        const msg = App.state.messages[idx];
        if (!msg || !newText.trim()) return;
        msg.content = newText.trim();
        overlay.remove();
        const el = this.container.querySelector(`.message:nth-child(${idx + 1})`);
        if (el) {
            const content = el.querySelector('.msg-content');
            if (content) { content.textContent = msg.content; content.style.display = ''; }
        }
        WsClient.send(JSON.stringify({ type: 'edit', index: idx, content: msg.content }));
    },

    appendChunk(text) {
        if (!this.currentMsgEl) {
            this.hideEmptyState();
            const msg = { role: 'assistant', content: '', id: 'msg_' + Date.now() };
            App.state.messages.push(msg);
            this.currentMsgEl = this.renderMessage(msg);
            this.currentMsgEl.classList.add('streaming');
        }
        this.buffer += text;
        const parts = parseThinkBlocks(this.buffer);
        const contentParts = parts.filter(p => p.type === 'content').map(p => p.text);
        const thinkingParts = parts.filter(p => p.type === 'thinking').map(p => p.text);

        const bubble = this.currentMsgEl.querySelector('.message-bubble');
        if (bubble) {
            const contentDiv = bubble.querySelector('.msg-content');
            if (contentDiv) {
                contentDiv.innerHTML = this.renderMarkdown(contentParts.join(''));
            }
            let thinkingDetails = bubble.querySelector('.thinking-collapsible');
            if (thinkingParts.length > 0 && thinkingParts[0]) {
                if (!thinkingDetails) {
                    thinkingDetails = document.createElement('details');
                    thinkingDetails.className = 'thinking-collapsible';
                    thinkingDetails.open = true;
                    const summary = document.createElement('summary');
                    summary.textContent = 'Thinking';
                    thinkingDetails.appendChild(summary);
                    const pre = document.createElement('pre');
                    pre.textContent = '';
                    thinkingDetails.appendChild(pre);
                    bubble.prepend(thinkingDetails);
                }
                const pre = thinkingDetails.querySelector('pre');
                if (pre) pre.textContent = thinkingParts.join('');
            }
        }
        this.scrollBottom();
    },

    finalizeMessage() {
        if (this.currentMsgEl) {
            this.currentMsgEl.classList.remove('streaming');
            this.currentMsgEl = null;
        }
        this.buffer = '';
        App.state.isStreaming = false;
        this.setInputDisabled(false);
        this.input.focus();
    },

    addApprovalNotice(data) {
        const el = document.createElement('div');
        el.className = 'message assistant';
        const bubble = document.createElement('div');
        bubble.className = 'message-bubble';
        const tag = document.createElement('div');
        tag.style.cssText = 'font-size:12px;color:var(--text-muted);text-align:center;padding:8px';
        const status = data.approved ? '✅ approved' : '❌ denied';
        tag.textContent = `Tool call: ${data.tool_name} ${status}`;
        bubble.appendChild(tag);
        el.appendChild(bubble);
        this.container.appendChild(el);
        this.scrollBottom();
    },

    clear() {
        this.container.innerHTML = '';
        this.buffer = '';
        this.currentMsgEl = null;
        this.emptyEl = null;
        this.renderEmptyState();
    },

    setInputDisabled(disabled) {
        this.input.disabled = disabled;
        this.sendBtn.style.display = disabled ? 'none' : '';
        this.stopBtn.style.display = disabled ? '' : 'none';
    },

    scrollBottom() {
        this.container.scrollTop = this.container.scrollHeight;
    }
};
