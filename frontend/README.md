# Echo AI Frontend

React + Vite + TypeScript frontend for Echo AI agent framework.

## Setup

```bash
npm install
```

## Scripts

| Command             | Description                                  |
| ------------------- | -------------------------------------------- |
| `npm run dev`       | Start dev server                             |
| `npm run build`     | Build for production                         |
| `npm run test:run`  | Run vitest tests                             |
| `npm run lint`      | ESLint                                       |
| `npm run typecheck` | TypeScript checks                            |
| `npm run check`     | All checks (lint + typecheck + test + build) |

## Communication

- WebSocket: `/ws/chat`
- SSE streaming: `/api/stream`
- REST: `/api/chat`
