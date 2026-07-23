let state = {
    status: 'loading',
    sessionId: null,
    token: null,
    unlocked: false
};

async function checkStatus() {
    try {
        const r = await fetch('/api/status');
        const data = await r.json();
        document.getElementById('status-text').textContent =
            data.needs_setup ? 'First-run setup required' :
            data.locked ? 'Locked — enter password' :
            'Connected — ready';
        if (!data.locked && !data.needs_setup) {
            state.unlocked = true;
            document.getElementById('status').style.display = 'none';
            document.getElementById('chat-container').style.display = 'flex';
        }
    } catch (e) {
        document.getElementById('status-text').textContent = 'Cannot connect to server';
    }
}

checkStatus();
