# Homebrew toolchain for echo-ai macOS CI and local dev.
# Versions are pinned by Brewfile.lock.json (see AGENTS.md "Environment and
# toolchain"). Regenerate with: brew bundle --force --file=Brewfile
#
# notcurses: echo-ai links only libnotcurses-core (no multimedia, no C++
# bindings). Homebrew's stock formula builds with ffmpeg multimedia support;
# that code is never linked by this project — the ffmpeg install is inert
# weight, recorded by the `brew list --versions` CI step.
brew "check"
brew "cjson"
brew "cmake"
brew "curl"
brew "libuv"
brew "notcurses"
brew "openssl@3"
brew "pkgconf"
brew "sqlite"
