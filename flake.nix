{
  nixConfig = {
    # keep-sorted start block=yes newline_separated=yes
    extra-substituters = [
      # https://cache.nixos.org has priority 40
      "https://nix-cache.hilorioze.com?priority=41"
    ];

    extra-trusted-public-keys = ["nix-cache.hilorioze.com-1:vKKWGjVDgXl/TXbUWuPWTnDhhDit6hqkTcuoGfter5Y="];
    # keep-sorted end
  };

  inputs = {
    flake-parts.url = "github:hercules-ci/flake-parts";

    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
  };

  outputs = inputs @ {
    # keep-sorted start
    flake-parts,
    nixpkgs,
    # keep-sorted end
    ...
  }: let
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

          license = lib.licenses.unfree;

          platforms = systems;
        };
      };
  in
    flake-parts.lib.mkFlake {inherit inputs;} {
      inherit systems;

      perSystem = {system, ...}: let
        pkgs = import nixpkgs {
          inherit system;

          config.allowUnfree = true;
        };

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
