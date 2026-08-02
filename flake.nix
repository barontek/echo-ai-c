{
  description = "Echo AI - C development environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
      in
      {
        devShells.default = pkgs.mkShell {
          buildInputs = with pkgs; [
            cmake
            gnumake
            pkg-config
            libuv
            curl
            sqlite
            openssl
            cjson
            check
            valgrind
            gcovr
            nodejs_22
            caddy
          ];

          shellHook = ''
            echo "Echo AI dev environment ready"
            echo "  cmake:  $(cmake --version | head -1)"
            echo "  deps:   libuv curl sqlite openssl cjson check"
            echo "  node:   $(node --version 2>/dev/null || echo 'not found')"
          '';
        };
      });
}
