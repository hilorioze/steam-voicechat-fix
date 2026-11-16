{
  inputs = {
    flake-parts.url = "github:hercules-ci/flake-parts";

    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
  };

  outputs = inputs @ {flake-parts, ...}: let
    systems = ["i686-linux"];

    steam-voicechat-fix = {
      # keep-sorted start
      cmake,
      lib,
      stdenv,
      # keep-sorted end
      ...
    }:
      stdenv.mkDerivation {
        pname = "steam-voicechat-fix";

        version = "0";

        src = lib.fileset.toSource {
          root = ./.;

          fileset = lib.fileset.unions [
            ./CMakeLists.txt
            ./src
          ];
        };

        nativeBuildInputs = [cmake];

        installPhase = ''
          runHook preInstall

          install -D --mode=444 libsteam_voicechat_fix.so $out/lib/libsteam_voicechat_fix.so

          runHook postInstall
        '';

        meta = {
          description = "Automatic LD_PRELOAD fix for Steam's Linux voice-chat decoder crash in GoldSrc games";
          homepage = "https://github.com/hilorioze/steam-voicechat-fix";

          license = lib.licenses.mit;

          platforms = systems;
        };
      };
  in
    flake-parts.lib.mkFlake {inherit inputs;} {
      inherit systems;

      perSystem = {pkgs, ...}: let
        package = pkgs.callPackage steam-voicechat-fix {};
      in {
        packages = {
          # keep-sorted start
          default = package;
          steam-voicechat-fix = package;
          # keep-sorted end
        };
      };

      flake.overlays.default = _final: prev: inputs.self.packages.${prev.stdenv.hostPlatform.system} or {};
    };
}
