const Components = {
    showScreen(id) {
        document.querySelectorAll('.screen').forEach(s => s.classList.remove('active'));
        const el = document.getElementById(id);
        if (el) el.classList.add('active');
    },

    showLoading(msg) {
        document.getElementById('loading-text').textContent = msg || 'Loading...';
        this.showScreen('screen-loading');
    },

    showSetup() {
        this.showScreen('screen-setup');
        const btn = document.getElementById('setup-btn');
        const input = document.getElementById('setup-password');
        const err = document.getElementById('setup-error');
        btn.onclick = async () => {
            const pw = input.value.trim();
            if (pw.length < 4) { err.textContent = 'Password must be at least 4 characters'; err.style.display = ''; return; }
            err.style.display = 'none';
            btn.disabled = true;
            const res = await Api.setup(pw);
            btn.disabled = false;
            if (res.token) { App.state.token = res.token; App.state.status = 'ready'; App.bootChat(); }
            else { err.textContent = res.error || 'Setup failed'; err.style.display = ''; }
        };
        input.onkeydown = (e) => { if (e.key === 'Enter') btn.click(); };
        input.focus();
    },

    showUnlock() {
        this.showScreen('screen-unlock');
        const btn = document.getElementById('unlock-btn');
        const input = document.getElementById('unlock-password');
        const err = document.getElementById('unlock-error');
        btn.onclick = async () => {
            const pw = input.value.trim();
            if (!pw) return;
            err.style.display = 'none';
            btn.disabled = true;
            const res = await Api.unlock(pw);
            btn.disabled = false;
            if (res.token) { App.state.token = res.token; App.state.status = 'ready'; App.bootChat(); }
            else { err.textContent = res.error || 'Wrong password'; err.style.display = ''; }
        };
        input.onkeydown = (e) => { if (e.key === 'Enter') btn.click(); };
        input.focus();
    },

    renderSidebar(sessions, currentId) {
        const list = document.getElementById('session-list');
        list.innerHTML = '';
        if (!sessions || sessions.length === 0) {
            const empty = document.createElement('div');
            empty.className = 'session-empty';
            empty.textContent = 'No sessions yet';
            list.appendChild(empty);
            return;
        }
        sessions.forEach(s => {
            const el = document.createElement('div');
            el.className = 'session-item' + (s.id === currentId ? ' active' : '');

            const title = document.createElement('span');
            title.className = 'session-title';
            title.textContent = s.title || s.id;
            title.onclick = () => App.switchSession(s.id);

            const actions = document.createElement('div');
            actions.className = 'session-actions';

            const renameBtn = document.createElement('button');
            renameBtn.className = 'rename-button';
            renameBtn.textContent = '✏️';
            renameBtn.title = 'Rename';
            let renameInput = null;

            renameBtn.onclick = (e) => {
                e.stopPropagation();
                if (renameInput) return;
                renameInput = document.createElement('input');
                renameInput.className = 'rename-input';
                renameInput.value = s.title || s.id;
                title.style.display = 'none';
                renameBtn.style.display = 'none';
                el.appendChild(renameInput);
                renameInput.focus();
                renameInput.select();

                const finish = async () => {
                    const val = renameInput.value.trim();
                    el.removeChild(renameInput);
                    title.style.display = '';
                    renameBtn.style.display = '';
                    renameInput = null;
                    if (val && val !== s.title) {
                        await Api.updateSession(App.state.token, s.id, { title: val });
                        s.title = val;
                        title.textContent = val;
                    }
                };

                renameInput.onkeydown = (ev) => {
                    if (ev.key === 'Enter') { ev.preventDefault(); finish(); }
                    if (ev.key === 'Escape') {
                        el.removeChild(renameInput);
                        title.style.display = '';
                        renameBtn.style.display = '';
                        renameInput = null;
                    }
                };
                renameInput.onblur = finish;
            };

            const del = document.createElement('button');
            del.className = 'session-del';
            del.textContent = '×';
            del.title = 'Delete';
            del.onclick = (e) => {
                e.stopPropagation();
                Components.showDeleteConfirm(s.id);
            };

            actions.appendChild(renameBtn);
            actions.appendChild(del);
            el.appendChild(title);
            el.appendChild(actions);
            list.appendChild(el);
        });

        document.getElementById('sidebar-toggle').onclick = () => {
            document.getElementById('sidebar').classList.toggle('open');
        };
        document.getElementById('new-chat-btn').onclick = () => App.createSession();

        const search = document.getElementById('session-search');
        search.oninput = () => {
            const q = search.value.toLowerCase();
            list.querySelectorAll('.session-item').forEach(el => {
                const t = el.querySelector('.session-title');
                if (t) el.style.display = t.textContent.toLowerCase().includes(q) ? '' : 'none';
            });
        };
    },

    showDeleteConfirm(sessionId) {
        const existing = document.querySelector('.confirm-overlay');
        if (existing) existing.remove();

        const overlay = document.createElement('div');
        overlay.className = 'confirm-overlay';
        overlay.innerHTML = `
            <div class="confirm-dialog">
                <p>Delete this session?</p>
                <div class="confirm-actions">
                    <button class="confirm-cancel" id="confirm-cancel">Cancel</button>
                    <button class="confirm-delete" id="confirm-delete">Delete</button>
                </div>
            </div>`;
        document.body.appendChild(overlay);

        overlay.querySelector('#confirm-cancel').onclick = () => overlay.remove();
        overlay.querySelector('#confirm-delete').onclick = () => {
            App.deleteSession(sessionId);
            overlay.remove();
        };
        overlay.onclick = (e) => { if (e.target === overlay) overlay.remove(); };
    },

    showApproval(data) {
        const dialog = document.getElementById('approval-dialog');
        if (!dialog) return;

        const DESTRUCTIVE = new Set(['bash', 'write_file']);
        const isDestructive = DESTRUCTIVE.has(data.tool_name);

        document.getElementById('approval-tool').textContent = data.tool_name || '';
        document.getElementById('approval-args').textContent = data.arguments || '';

        const warning = dialog.querySelector('.approval-warning');
        if (warning) warning.style.display = isDestructive ? '' : 'none';

        dialog.classList.add('open');
        document.getElementById('approval-allow').onclick = () => {
            dialog.classList.remove('open');
            WsClient.send(JSON.stringify({ type: 'approval_response', request_id: data.request_id, approved: true }));
        };
        document.getElementById('approval-deny').onclick = () => {
            dialog.classList.remove('open');
            WsClient.send(JSON.stringify({ type: 'approval_response', request_id: data.request_id, approved: false }));
        };
    },

    setConnectionStatus(status) {
        const dot = document.getElementById('conn-dot');
        if (!dot) return;
        dot.className = 'conn-' + status;
        const badge = document.getElementById('model-badge');
        if (badge) badge.textContent = status === 'connected' ? (App.state.currentModel || 'connected') : status;
    },

    toggleTheme() {
        const html = document.documentElement;
        const current = html.getAttribute('data-theme') || 'dark';
        const next = current === 'dark' ? 'light' : 'dark';
        html.setAttribute('data-theme', next);
        localStorage.setItem('echo-ai-theme', next);
    }
};
