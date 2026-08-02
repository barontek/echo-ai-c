# OpenAI OAuth Plan

## Goal

Make `openai` a dedicated OAuth-only provider that uses a user's ChatGPT
Plus/Pro entitlement through the Codex backend. It must not accept an OpenAI
API key or use the public `api.openai.com/v1` API.

Static Bearer tokens remain supported by the separate `openai_compatible`
provider for local and third-party OpenAI-compatible servers.

## User Experience

OpenAI login is triggered explicitly from the provider selector. Echo must not
open a browser automatically at startup.

1. The user selects `openai` in the sidebar.
2. The frontend requests `GET /api/auth/openai/status`.
3. If already signed in, Echo selects the provider and loads its model list.
4. Otherwise, the frontend opens a placeholder popup and requests
   `POST /api/auth/openai/start`.
5. The backend creates an OAuth transaction and returns only an authorization
   URL and opaque login ID.
6. The popup navigates to OpenAI, where the user signs in.
7. OpenAI redirects to Echo's loopback callback listener.
8. The backend validates and exchanges the callback code, then stores the
   credentials encrypted.
9. The frontend polls the status endpoint until it reports `signed_in` or an
   error, then selects OpenAI and fetches its models.

Authorization codes, access tokens, and refresh tokens must never pass through
the frontend or browser storage.

## Authentication Flow

### Browser Login

Use OAuth authorization code flow with PKCE:

- Issuer: `https://auth.openai.com`
- Authorization endpoint: `/oauth/authorize`
- Token endpoint: `/oauth/token`
- Redirect URI: `http://localhost:1455/auth/callback`
- Callback listener: IPv4 loopback only (`127.0.0.1`)
- PKCE method: `S256`
- Scopes: `openid profile email offline_access`
- Additional parameters:
  - `id_token_add_organizations=true`
  - `codex_cli_simplified_flow=true`
  - `originator=echo-ai`

For each login attempt, generate independent cryptographically random values
for the PKCE verifier, OAuth state, and opaque login ID. OAuth state is
single-use and must be compared before exchanging the authorization code.
Pending attempts expire after five minutes.

The client ID and callback port must follow the public Codex/OpenCode flow and
remain compatible with OpenAI's registered redirect allow-list.

### Headless Login

Add device-code login for CLI and remote/headless systems:

1. Request a user code from `/api/accounts/deviceauth/usercode`.
2. Display `https://auth.openai.com/codex/device` and the one-time code.
3. Poll `/api/accounts/deviceauth/token` at the server-provided interval.
4. Exchange the returned authorization code at `/oauth/token`.
5. Stop on success, cancellation, a terminal response, or timeout.

The device flow should share token parsing, persistence, and refresh logic with
browser login rather than introduce a second credential implementation.

## Credential State

The OAuth manager owns:

- Access token
- Refresh token
- Access-token expiry
- ChatGPT account ID
- ChatGPT plan type
- Pending PKCE verifier and state
- Last login error suitable for UI display

The access token and refresh token are sensitive. Clear temporary token,
authorization-code, and PKCE buffers before freeing them where practical.

### Persistence

Provider OAuth data is stored in the existing SQLite session database in a
dedicated `provider_oauth` table. The JSON credential payload is encrypted with
the current `SessionManager` encryption key before it is bound to SQLite.

Requirements:

- Never write OAuth credentials to `config.conf`.
- Never store credentials in browser storage.
- Persist refresh-token rotation atomically.
- Do not replace valid stored credentials if encryption or SQL persistence
  fails.
- Loading malformed or undecryptable data must fail without committing partial
  in-memory state.
- Password migration must keep provider credentials decryptable or migrate the
  table in the same transaction as session data.

OAuth routes require the local Echo database to be unlocked, because the
session encryption key is needed to read or update credentials.

## Token Refresh

Before every OpenAI request, obtain a valid access token from the OAuth manager.

- Refresh five minutes before expiration.
- Permit only one refresh operation at a time.
- Other concurrent requests wait for that refresh instead of sending their own.
- If OpenAI rotates the refresh token, replace and persist it atomically.
- Retry one request after a `401` only when a token refresh succeeds.
- Do not loop indefinitely on authorization failures.
- Treat permanent refresh failures as signed out and require a new login.
- Keep transient network failures distinguishable from permanent credential
  failures.

Network requests must not hold a general state mutex in a way that blocks
status/logout indefinitely. Use a refresh condition or explicit refresh state
for single-flight coordination.

## Dedicated OpenAI Provider

`src/llm/openai.c` is a dedicated Codex Responses provider, not a wrapper around
`openai_compatible`.

- Endpoint: `https://chatgpt.com/backend-api/codex/responses`
- Authentication: `Authorization: Bearer <OAuth access token>`
- Account routing: `ChatGPT-Account-Id: <account ID>` when available
- Originator: `echo-ai`
- Protocol: OpenAI Responses API request and streaming event shapes

The adapter must support Echo's existing provider contract:

- Buffered responses
- Streaming text deltas
- Tool definitions
- Function-call argument deltas
- Multiple function calls distinguished by item/output index or call ID
- Function-call outputs in subsequent requests
- Structured output, including the requested JSON schema
- Request timeouts supplied by the agent
- Explicit non-2xx handling with redacted errors
- One refresh-and-retry attempt after `401`

Do not silently ignore unsupported parameters. If the Codex backend does not
support an Echo feature, return a contextual error or implement a documented
translation.

## Models

The ChatGPT Codex backend does not use the public `/v1/models` endpoint.
Initially expose a reviewed local allow-list only while OAuth is signed in.
Keep this list in one backend location and update it deliberately as OpenAI's
entitlements change.

The frontend must receive an empty list while signed out. It must start login
before committing `openai` as the active provider.

## Backend API

All routes require a valid Echo unlock token:

### `GET /api/auth/openai/status`

Returns public state only:

```json
{
  "state": "signed_out | pending | signed_in",
  "account_id": "optional",
  "plan_type": "optional",
  "error": "optional"
}
```

### `POST /api/auth/openai/start`

Starts one browser login attempt:

```json
{
  "authorization_url": "https://auth.openai.com/...",
  "login_id": "opaque value"
}
```

The status request should accept or validate the login ID so one browser cannot
observe or confuse another pending login attempt.

### `POST /api/auth/openai/logout`

Cancel any pending login, revoke credentials when supported, delete encrypted
credentials, and clear sensitive in-memory state.

## Security Requirements

- Bind the callback listener only to loopback.
- Validate the exact callback method and path.
- Validate OAuth state before processing errors or codes.
- Reject duplicate query fields, malformed percent encoding, oversized
  requests, embedded NUL bytes, and callback request smuggling.
- Never log full callback URLs, authorization codes, access tokens, refresh
  tokens, ID tokens, PKCE verifiers, or encrypted credential blobs.
- Redact token endpoint response bodies before logging.
- Check every socket, pthread, OpenSSL, libcurl, cJSON, SQLite, and allocation
  return value.
- Use complete writes for callback responses; `send()` may write fewer bytes
  than requested.
- Ensure shutdown cannot race with callback, refresh, logout, session teardown,
  or OAuth manager destruction.
- Keep OAuth state single-use and clear it after success, failure, timeout, or
  cancellation.
- Do not infer entitlement solely from unverified JWT claims. OpenAI remains the
  authority and must enforce access server-side.

## Testing Plan

### OAuth Unit Tests

- PKCE verifier length and S256 challenge
- Authorization URL encoding and required parameters
- Callback success, denial, state mismatch, duplicate fields, missing fields,
  malformed encoding, wrong path/method, and oversized requests
- Token JSON validation and missing/invalid field handling
- JWT metadata extraction from expected and malformed payloads
- Expiry boundary and refresh skew
- Refresh-token rotation
- Permanent and transient refresh errors
- Single-flight concurrent refresh
- Logout during pending login and refresh
- Allocation-failure cleanup for every multi-allocation commit path

### Persistence Tests

- Encrypted-at-rest credential payload
- Save/load/delete round trip
- Wrong-password/decryption failure
- Encryption, SQLite prepare/bind/step, and allocation fault injection
- Existing credentials remain intact after a failed replacement
- Password-change migration preserves OAuth credentials

### Provider Tests

- Codex endpoint and required headers
- API-key configuration is ignored for `openai`
- Responses request conversion for text, tools, and tool outputs
- Buffered response parsing
- Fragmented SSE events
- Multiple interleaved function calls
- Structured-output request translation
- Non-2xx and `401` refresh/retry behavior
- Configured request timeout propagation

### Route And Frontend Tests

- Unlock enforcement for start/status/logout
- Public status never contains credentials
- Start conflict, callback completion, timeout, and logout
- Provider selection when already signed in
- Popup login and polling completion
- Popup blocked, user closes popup, backend unreachable, timeout, and unmount
- No provider switch or empty-model WebSocket connection before login succeeds

### Fuzz Targets

- OAuth callback request/query parser
- Token endpoint JSON parser
- JWT payload parser
- Codex Responses SSE parser

Run all Check tests under ASan/UBSan and run frontend typecheck, tests, lint, and
production build.

## Beta Branch Status

The `beta` branch currently contains an initial implementation:

- OAuth-only OpenAI configuration
- Browser PKCE flow with a loopback callback listener
- Encrypted provider credential table
- Token refresh and metadata extraction
- Dedicated Codex Responses provider
- Auth start/status/logout routes
- Sidebar login trigger and status polling
- Initial local model allow-list

This is a work-in-progress snapshot, not merge-ready.

Known remaining work:

- Add device-code login.
- Complete the security and concurrency audit.
- Validate callback parsing and partial socket I/O.
- Make refresh fully single-flight without holding state locks during network I/O.
- Add `401` refresh/retry and proper timeout propagation.
- Correct multi-tool streaming index/call-ID handling.
- Implement structured output instead of ignoring the schema.
- Ensure OAuth persistence participates in password migration.
- Add real OAuth, provider, route, persistence, frontend, and fuzz tests rather
  than link-only stubs.
- Update four legacy `routes_general` tests that still expect public OpenAI API
  keys and `/v1/models` behavior.
- Verify frontend typecheck/tests/build.
- Run the full Linux test suite inside `nix develop` with ASan/UBSan.

## Linux Continuation

```bash
git fetch origin
git switch beta
nix develop
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure

cd frontend
npm ci
npm run typecheck
npm run test:run
npm run lint
npm run build
```

Do not merge the beta branch until the remaining implementation items and
sanitizer-clean verification are complete.
