{
  description = "Echo AI - C development environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    # Toolchain versions are fixed by the committed flake.lock snapshot; do
    # not run `nix flake update` casually. CMake defaults to clang; gcc is
    # available for the odd GNU-only flag check. See AGENTS.md
    # "Environment and toolchain".
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};

        # TUI rendering library. The stock nixpkgs build links ffmpeg
        # (multimedia) and builds C++ bindings; neither is used by echo-ai,
        # which only links libnotcurses-core, so both are switched off here
        # (-DUSE_MULTIMEDIA=none -DUSE_CXX=off in notcurses terms). The nix
        # override maps to the former flag; the cmakeFlags append provides
        # the latter. Deps (ncurses/libunistring/libdeflate) are listed in
        # buildInputs so pkg-config can resolve notcurses-core.pc's
        # Requires chain inside the dev shell.
        notcurses-tui =
          (pkgs.notcurses.override { multimediaSupport = false; })
          .overrideAttrs (final: prev: {
            cmakeFlags = prev.cmakeFlags ++ [ "-DUSE_CXX=off" ];
            # notcurses installs a malloc'd alternate signal stack on its
            # input thread unless built with USE_ASAN (see unixsig.c). Under
            # ASan, that stack makes the thread's teardown abort in
            # __sanitizer::UnsetAlternateSignalStack (munmap fails), so every
            # TUI quit exits 1. Skipping the altstack is what notcurses
            # itself recommends for ASan builds, and it is inert otherwise.
            NIX_CFLAGS_COMPILE = (prev.NIX_CFLAGS_COMPILE or "") + " -DUSE_ASAN";
          });
      in
      {
        devShells.default = pkgs.mkShell {
          buildInputs = with pkgs; [
            cmake
            gnumake
            clang
            gcc
            pkg-config
            libuv
            curl
            sqlite
            openssl
            cjson
            notcurses-tui
            ncurses
            libunistring
            libdeflate
            check
            valgrind
            gcovr
            nodejs_22
            caddy
            # Optional (not a build dependency): web_fetch uses the
            # curl-impersonate-chrome binary at runtime when it is on PATH
            # to retry pages served a JS challenge to plain libcurl.
            curl-impersonate
            # Optional (not a build dependency): the browser tool spawns
            # whichever Chromium-family binary it discovers at runtime
            # (ECHO_BROWSER_BIN, $BROWSER, then PATH). A browser is NOT
            # pinned here anymore: users with a system browser (or
            # ECHO_BROWSER_BIN) get one on PATH already, and pinning brave
            # made every dev-shell entry download ~500MB from the cache.
          ];

          shellHook = ''
            echo "Echo AI dev environment ready"
            echo "  cmake:  $(cmake --version | head -1)"
            echo "  deps:   libuv curl sqlite openssl cjson check notcurses"
            echo "  node:   $(node --version 2>/dev/null || echo 'not found')"
          '';
        };
      });
}
