{
  description = "3Beans, a low-level 3DS emulator (macOS)";

  # Same channel the CI build-mac job uses (see .github/workflows/autobuild.yml)
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-25.05-darwin";
  };

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "aarch64-darwin"
        "x86_64-darwin"
      ];
      eachSystem = f: nixpkgs.lib.genAttrs systems (system: f system);

      # Static, Cocoa-build of wxWidgets 3.3.2 cloned with --recursive by CI;
      # the official release tarball bundles the third-party sources, so
      # --disable-sys-libs keeps the build fully self-contained.
      mkWxWidgets =
        pkgs:
        pkgs.stdenv.mkDerivation (finalAttrs: {
          pname = "wxmac-static";
          version = "3.3.2";

          src = pkgs.fetchurl {
            url = "https://github.com/wxWidgets/wxWidgets/releases/download/v${finalAttrs.version}/wxWidgets-${finalAttrs.version}.tar.bz2";
            hash = "sha256-UKKMtmjeR7DgBs1uvtjPT3bBysYRb7PJeMREeCGRA/I=";
          };

          nativeBuildInputs = [ pkgs.pkg-config ];

          configureFlags = [
            "--disable-sys-libs"
            "--disable-shared"
            "--disable-tests"
            "--without-libcurl"
          ];

          enableParallelBuilding = true;

          meta = {
            description = "wxWidgets (Cocoa, static) built for 3Beans";
            homepage = "https://www.wxwidgets.org";
            license = pkgs.lib.licenses.wxWindows;
            platforms = pkgs.lib.platforms.darwin;
          };
        });

      mkThreebeans =
        pkgs:
        pkgs.stdenv.mkDerivation (finalAttrs: {
          pname = "3beans";
          version = "unstable";

          src = pkgs.lib.cleanSourceWith {
            src = self;
            filter = path: type:
              let
                relative = pkgs.lib.removePrefix "${self}/" (toString path);
                root = builtins.head (pkgs.lib.splitString "/" relative);
              in
              pkgs.lib.cleanSourceFilter path type
              && !(builtins.elem root [ "build" "reference" "3beans" "result" ]);
          };

          nativeBuildInputs = [ pkgs.pkg-config ];

          buildInputs = [
            pkgs.portaudio
            pkgs.libepoxy
            pkgs.libpng
            finalAttrs.wxWidgets
          ];

          # Extra passthrough input so devShells can reuse this derivation
          wxWidgets = mkWxWidgets pkgs;

          # The Makefile defaults to g++, which is not in the Nix stdenv
          makeFlags = [ "CXX=${pkgs.stdenv.cc.targetPrefix}c++" ];

          enableParallelBuilding = true;
          doCheck = true;
          nativeCheckInputs = [ pkgs.python3 ];
          checkPhase = ''
            runHook preCheck
            make test
            runHook postCheck
          '';

          installPhase = ''
            runHook preInstall
            install -Dm755 3beans $out/bin/3beans
            install -Dm644 third_party/lua/LICENSE $out/share/licenses/3beans/Lua-LICENSE.txt
            runHook postInstall
          '';

          meta = {
            description = "A low-level 3DS emulator";
            homepage = "https://github.com/Hydr8gon/3Beans";
            license = pkgs.lib.licenses.gpl3Plus;
            platforms = pkgs.lib.platforms.darwin;
            mainProgram = "3beans";
          };
        });
    in
    {
      packages = eachSystem (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
          threebeans = mkThreebeans pkgs;
        in
        {
          default = threebeans;
          "3beans" = threebeans;
          "wxmac-static" = threebeans.wxWidgets;
        }
      );

      devShells = eachSystem (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default = pkgs.mkShell {
            inputsFrom = [ (mkThreebeans pkgs) ];
          };
        }
      );
    };
}
