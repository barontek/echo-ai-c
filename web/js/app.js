const App = {
    state: {
        status: 'loading',
        sessionId: null,
        sessions: [],
        messages: [],
        token: null,
        isStreaming: false,
        connectionStatus: 'disconnected',
        currentModel: 'gemma4-32k',
        currentProvider: 'ollama'
    },

    async init() {
        const theme = localStorage.getItem('echo-ai-theme') || 'dark';
        document.documentElement.setAttribute('data-theme', theme);

        Components.showLoading('Connecting...');

        WsClient.on('ready', (d) => {
            if (d.session_id) { this.state.sessionId = d.session_id; this.refreshSessions(); }
        });
        WsClient.on('history', (d) => {
            if (d.messages) {
                this.state.messages = d.messages;
                Chat.clear();
                for (const m of d.messages) Chat.renderMessage(m);
            }
        });
        WsClient.on('content', (d) => { Chat.appendChunk(d.content || ''); });
        WsClient.on('done', (d) => {
            const last = this.state.messages[this.state.messages.length - 1];
            if (last && last.role === 'assistant') {
                last.timestamp = d.timestamp || new Date().toLocaleTimeString();
                if (d.has_tools) last.has_tools = true;
                if (d.tool_calls) last.tool_calls = d.tool_calls;
                if (d.content) last.content = d.content;
            }
            Chat.finalizeMessage();
            this.refreshSessions();
        });
        WsClient.on('error', (d) => {
            const last = this.state.messages[this.state.messages.length - 1];
            if (last && last.role === 'user') last.error = d.message || 'Unknown error';
            Chat.appendChunk('[Error: ' + (d.message || 'unknown') + ']');
            Chat.finalizeMessage();
        });
        WsClient.on('approval_request', (d) => { Components.showApproval(d); });
        WsClient.on('approval_response', (d) => { Chat.addApprovalNotice(d); });
        WsClient.on('session_start', () => { this.refreshSessions(); });
        WsClient.on('title_updated', (d) => {
            const s = this.state.sessions.find(s => s.id === d.session_id);
            if (s) s.title = d.title;
            Components.renderSidebar(this.state.sessions, this.state.sessionId);
        });

        const res = await Api.status();
        if (res.needs_setup) { this.state.status = 'setup'; Components.showSetup(); }
        else if (res.locked) { this.state.status = 'locked'; Components.showUnlock(); }
        else { this.state.status = 'ready'; this.bootChat(); }
    },

    bootChat() {
        Components.showScreen('screen-main');
        Chat.init();
        Chat.setInputDisabled(true);
        this.refreshSessions().then(() => {
            if (this.state.sessions.length > 0) {
                const last = this.state.sessions[this.state.sessions.length - 1];
                this.switchSession(last.id);
            } else {
                this.createSession();
            }
        });
        setInterval(() => this.refreshSessions(), 10000);
    },

    async refreshSessions() {
        if (!this.state.token) return;
        const res = await Api.sessions(this.state.token);
        this.state.sessions = res.sessions || [];
        Components.renderSidebar(this.state.sessions, this.state.sessionId);
    },

    async switchSession(id) {
        if (this.state.sessionId === id && this.state.messages.length > 0) return;
        WsClient.disconnect();
        this.state.sessionId = id;
        this.state.messages = [];
        Chat.clear();
        if (id) {
            WsClient.connect(id);
            Chat.setInputDisabled(true);
        }
        Components.renderSidebar(this.state.sessions, id);
    },

    async createSession() {
        const res = await Api.createSession(this.state.token);
        if (res.id) {
            this.state.sessions.push(res);
            await this.switchSession(res.id);
        }
    },

    async deleteSession(id) {
        await Api.deleteSession(this.state.token, id);
        this.state.sessions = this.state.sessions.filter(s => s.id !== id);
        if (this.state.sessionId === id) {
            this.state.sessionId = null;
            this.state.messages = [];
            Chat.clear();
            WsClient.disconnect();
            if (this.state.sessions.length > 0) this.switchSession(this.state.sessions[0].id);
            else this.createSession();
        }
        Components.renderSidebar(this.state.sessions, this.state.sessionId);
    }
};

document.addEventListener('DOMContentLoaded', () => App.init());
