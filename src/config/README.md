# src/config — the `.conf` parser

What this owns: parsing `config.conf` (key/value with typed getters:
provider settings, model, safety policy, ports) and exposing the loaded
`Conf` through `conf_load`.

Why it exists: configuration is the one input format the server must
tolerate being malformed, so parse errors degrade entry-by-entry with
documented behavior instead of aborting.
