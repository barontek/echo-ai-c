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
