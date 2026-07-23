const Api = {
    async _fetch(path, opts) {
        const r = await fetch(path, {
            headers: { 'Content-Type': 'application/json', ...opts?.headers },
            ...opts
        });
        const ct = r.headers.get('content-type') || '';
        if (ct.includes('application/json')) return r.json();
        const text = await r.text();
        try { return JSON.parse(text); } catch { return text; }
    },

    status() { return this._fetch('/api/status'); },

    unlock(password) {
        return this._fetch('/api/unlock', {
            method: 'POST', body: JSON.stringify({ password })
        });
    },

    setup(password) {
        return this._fetch('/api/setup', {
            method: 'POST', body: JSON.stringify({ password })
        });
    },

    logout(token) {
        return this._fetch('/api/logout', {
            method: 'POST', headers: { 'X-Unlock-Token': token }
        });
    },

    sessions(token) {
        return this._fetch('/api/sessions', {
            headers: { 'X-Unlock-Token': token }
        });
    },

    createSession(token, title) {
        return this._fetch('/api/sessions', {
            method: 'POST',
            headers: { 'X-Unlock-Token': token },
            body: JSON.stringify({ title: title || 'New Chat' })
        });
    },

    getSession(token, id) {
        return this._fetch(`/api/sessions/${id}`, {
            headers: { 'X-Unlock-Token': token }
        });
    },

    deleteSession(token, id) {
        return this._fetch(`/api/sessions/${id}`, {
            method: 'DELETE', headers: { 'X-Unlock-Token': token }
        });
    },

    updateSession(token, id, data) {
        return this._fetch(`/api/sessions/${id}`, {
            method: 'PUT',
            headers: { 'X-Unlock-Token': token },
            body: JSON.stringify(data)
        });
    },

    config(token) {
        return this._fetch('/api/config', {
            headers: token ? { 'X-Unlock-Token': token } : {}
        });
    },

    health() { return this._fetch('/api/health'); }
};
