# deploy — production deployment assets

What this owns: the Caddy reverse-proxy configuration used to serve the
built frontend and terminate TLS in front of the echo-ai HTTP server.

Why it exists: the dev flow runs echo-ai directly on :8080; production
deployments need a TLS-terminating proxy that also serves the static
`frontend/dist` build.
