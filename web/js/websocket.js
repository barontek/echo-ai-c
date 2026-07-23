const WsClient = {
    ws: null,
    url: null,
    reconnectTimer: null,
    pingTimer: null,
    handlers: {},
    shouldReconnect: false,

    connect(sessionId) {
        const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
        this.url = `${proto}//${location.host}/ws/chat?session_id=${sessionId}`;
        this.shouldReconnect = true;
        this._connect();
    },

    _connect() {
        if (this.ws) this.disconnect();
        this.ws = new WebSocket(this.url);

        this.ws.onopen = () => {
            App.state.connectionStatus = 'connected';
            App.render();
            this.pingTimer = setInterval(() => {
                if (this.ws && this.ws.readyState === WebSocket.OPEN)
                    this.ws.send('{"type":"ping"}');
            }, 15000);
        };

        this.ws.onmessage = (e) => {
            let data;
            try { data = JSON.parse(e.data); } catch { return; }
            const handler = this.handlers[data.type];
            if (handler) handler(data);
        };

        this.ws.onclose = () => {
            App.state.connectionStatus = 'disconnected';
            App.render();
            clearInterval(this.pingTimer);
            if (this.shouldReconnect)
                this.reconnectTimer = setTimeout(() => this._connect(), 2000);
        };

        this.ws.onerror = () => { this.ws.close(); };
    },

    on(type, handler) { this.handlers[type] = handler; },

    send(data) {
        if (this.ws && this.ws.readyState === WebSocket.OPEN)
            this.ws.send(typeof data === 'string' ? data : JSON.stringify(data));
    },

    disconnect() {
        this.shouldReconnect = false;
        clearTimeout(this.reconnectTimer);
        clearInterval(this.pingTimer);
        if (this.ws) { this.ws.onclose = null; this.ws.close(); this.ws = null; }
        App.state.connectionStatus = 'disconnected';
    }
};
