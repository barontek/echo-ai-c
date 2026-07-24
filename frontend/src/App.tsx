import { useState, useEffect, useCallback } from 'react';
import { api } from './api/client';
import { ChatProvider } from './context';
import {
  ApprovalDialog,
  Header,
  Sidebar,
  MessageList,
  ChatInput,
  UnlockScreen,
  SetupScreen,
} from './components';
import './App.css';

function App() {
  const [statusLoading, setStatusLoading] = useState(true);
  const [locked, setLocked] = useState(true);
  const [needsSetup, setNeedsSetup] = useState(false);

  const relock = useCallback(() => {
    setLocked(true);
    api.clearUnlockToken();
  }, []);

  useEffect(() => {
    api
      .getStatus()
      .then((s) => {
        setLocked(s.locked);
        setNeedsSetup(s.needs_setup);
      })
      .catch(() => setLocked(true))
      .finally(() => setStatusLoading(false));

    // Register token-expired callback — triggers if the server
    // rejects our unlock token (e.g. after server restart or logout).
    api.setOnTokenExpired(() => {
      setLocked(true);
    });
  }, []);

  async function handleLogout() {
    await api.logout();
    relock();
  }

  if (statusLoading) {
    return <div className="app-loading">Loading…</div>;
  }

  if (needsSetup) {
    return (
      <SetupScreen
        onComplete={() => {
          setNeedsSetup(false);
          setLocked(false);
        }}
      />
    );
  }

  if (locked) {
    return <UnlockScreen onUnlocked={() => setLocked(false)} />;
  }

  return (
    <ChatProvider>
      <ApprovalDialog />
      <div className="app">
        <Sidebar />
        <div className="main-content">
          <Header onLogout={handleLogout} />
          <MessageList />
          <ChatInput />
        </div>
      </div>
    </ChatProvider>
  );
}

export default App;
