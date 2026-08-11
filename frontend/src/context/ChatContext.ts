import { createContext, type Context } from 'react';
import type { ApprovalRequest, BranchInfo, ToolCall } from '../types';

/** ConnectionStatus - lifecycle of the chat websocket; 'reconnecting' is
 * entered after an unexpected drop, 'connected' after the handshake. */
export type ConnectionStatus = 'connected' | 'connecting' | 'disconnected' | 'reconnecting';

/* Fallback per-provider effort option lists, used only until /api/providers
 * resolves (the backend's effort_options map is authoritative). OpenAI gets
 * xhigh; openai_compatible, ollama and opencode_zen share the smaller set. */
/** EFFORT_OPTIONS_BY_PROVIDER - fallback per-provider effort option
 * lists, used only until /api/providers resolves (the backend's
 * effort_options map is authoritative). */
export const EFFORT_OPTIONS_BY_PROVIDER: Record<string, string[]> = {
  openai: ['low', 'medium', 'high', 'xhigh', 'max', 'none'],
  openai_compatible: ['low', 'medium', 'high', 'max', 'none'],
  ollama: ['low', 'medium', 'high', 'max', 'none'],
  opencode_zen: ['low', 'medium', 'high', 'max', 'none'],
};
/** EffortOption - one reasoning-effort level name; the authoritative list
 * comes from the backend's /api/providers effort_options map. */
export type EffortOption = string;

/** ChatContextValue - the full chat contract exposed by ChatProvider.
 *
 * Nullability: `activeSessionId`, `pendingApproval` and `pendingQuestion`
 * are null exactly when nothing is active/pending — the approval and
 * ask-user dialogs render null in those states; `pendingApproval` null
 * never means "failed to load". `sessions`/`models`/`providers` are
 * always arrays (possibly empty). `isConnected` is a derived boolean of
 * `connectionStatus === 'connected'`.
 *
 * Async actions: selectSession/selectProvider/deleteSession/renameSession
 * return promises that reject on backend failure (they surface their own
 * errors); the remaining actions are fire-and-forget via the websocket. */
export interface ChatContextValue {
  sessions: Array<{ id: string; title: string; created_at: string }>;
  activeSessionId: string | null;
  currentModel: string;
  currentProvider: string;
  models: string[];
  providers: string[];
  currentEffort: string;
  effortOptions: string[];
  selectEffort: (effort: string) => void;
  supportsEffort: boolean;
  messages: Array<{
    role: 'user' | 'assistant' | 'system' | 'tool';
    content: string;
    id?: string;
    parent_id?: string;
    fork_group_id?: string;
    timestamp?: string;
    thinking?: string;
    has_tools?: boolean;
    tool_calls?: ToolCall[];
    tool_call_id?: string;
    tool_name?: string;
    error?: string;
  }>;
  /* Fork-point counters for the active chain, keyed by message id. */
  branchInfo: BranchInfo[];
  connectionStatus: ConnectionStatus;
  isConnected: boolean;
  isStreaming: boolean;
  currentThinking: string;
  sidebarOpen: boolean;
  setSidebarOpen: (open: boolean) => void;
  sendMessage: (content: string) => void;
  stopGeneration: () => void;
  editMessage: (index: number, newText: string, msgId?: string) => void;
  regenerateMessage: (index: number, msgId?: string) => void;
  switchBranch: (messageId: string, direction: -1 | 1) => void;
  createSession: () => void;
  selectSession: (sessionId: string) => Promise<void>;
  deleteSession: (sessionId: string) => Promise<void>;
  renameSession: (sessionId: string, newTitle: string) => Promise<void>;
  selectModel: (model: string) => void;
  selectProvider: (provider: string, prefetchedModels?: string[]) => Promise<void>;
  reconnect: () => void;
  pendingApproval: ApprovalRequest | null;
  resolveApproval: (requestId: string, approved: boolean) => void;
  pendingQuestion: string | null;
  resolveAskUser: (answer: string) => void;
}

/** ChatContext - React context carrying ChatContextValue, or null before
 * ChatProvider mounts. Consumers must handle the null case (or use the
 * provider's hook). */
export const ChatContext: Context<ChatContextValue | null> = createContext<ChatContextValue | null>(
  null
);
