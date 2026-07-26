// Centralized types for the application

export interface Session {
  id: string;
  title: string;
  created_at: string;
}

export interface Message {
  role: 'user' | 'assistant' | 'system' | 'tool';
  content: string;
  timestamp?: string;
  thinking?: string;
  has_tools?: boolean;
  tool_calls?: ToolCall[];
  tool_call_id?: string;
  tool_name?: string;
}

export interface ToolCall {
  name: string;
  arguments: string | Record<string, unknown>;
  result?: {
    content: string;
    error: string | null;
  };
}

export interface Config {
  provider: string;
  model: string;
  temperature: number;
  max_iterations: number;
  session_enabled: boolean;
}

export interface ApprovalRequest {
  type: 'approval_request';
  request_id: string;
  tool_name: string;
  arguments: string;
}

export interface StreamEvent {
  type:
    | 'ready'
    | 'message'
    | 'content'
    | 'done'
    | 'error'
    | 'pong'
    | 'session_start'
    | 'approval_request'
    | 'title_updated'
    | 'history'
    | 'tool_start';
  content?: string;
  has_tools?: boolean;
  tool_calls?: ToolCall[];
  messages?: Message[];
  session_id?: string;
  title?: string;
  timestamp?: string;
  role?: string;
  request_id?: string;
  tool_name?: string;
  arguments?: string;
}

export interface ApiError {
  error: string;
  detail?: string;
}
