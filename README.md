# steam-voicechat-fix

Inspired by and based on [makindotcc](https://github.com/makindotcc)'s work on [hl-linux-voicechat-crash-fix](https://github.com/makindotcc/hl-linux-voicechat-crash-fix)

> [!WARNING]
> I cannot guarantee that this will not result in a VAC ban. Game processes are
> not patched, and only Steam's main-process `steamclient.so` is modified. I use
> this on my main Steam account, and AFAIK it should not trigger any traps. This
> is only my experience. Use it at your own risk!

An automatic i686 `LD_PRELOAD` fix for Steam's Linux voice-chat decoder crash
affecting CS 1.6 and other GoldSrc-based games, as was initially reported in
[ValveSoftware/halflife#3895](https://github.com/ValveSoftware/halflife/issues/3895)

## How it works

This fix is an i686 `LD_PRELOAD` library for the main Steam process. It waits
for `steamclient.so`, locates the PLT stub for its `memmove` relocation, and
replaces the `ebx`-relative indirect jump with a direct jump to the resolved
`memmove` implementation. This makes the `memmove` call independent of the
`ebx` value clobbered by the voice decoder

## Installation

### Use a binary release

Download `libsteam_voicechat_fix.so` from the [latest release](https://github.com/hilorioze/steam-voicechat-fix/releases/latest)
and save it somewhere outside the Steam installation, for example:

```sh
mkdir -p $HOME/.local/lib

curl --fail --location \
  --output $HOME/.local/lib/libsteam_voicechat_fix.so \
  https://github.com/hilorioze/steam-voicechat-fix/releases/latest/download/libsteam_voicechat_fix.so
```

If Steam is running, close it completely, then start it with `LD_PRELOAD` set
to the resulting library path:

```sh
LD_PRELOAD=$HOME/.local/lib/libsteam_voicechat_fix.so steam
```

### Nix

Build the library from the flake:

```sh
nix build --print-build-logs github:hilorioze/steam-voicechat-fix#packages.i686-linux.default
```

The resulting library should appear at `result/lib/libsteam_voicechat_fix.so`

### NixOS

Add the flake input and overlay, then configure Steam to preload the package:

```nix
{
  # optionally use the binary cache
  # nixConfig = {
  #   # keep-sorted start block=yes newline_separated=yes
  #   extra-substituters = [
  #     # https://cache.nixos.org has priority 40
  #     "https://nix-cache.hilorioze.com?priority=41"
  #   ];
  #
  #   extra-trusted-public-keys = [
  #     "nix-cache.hilorioze.com-1:vKKWGjVDgXl/TXbUWuPWTnDhhDit6hqkTcuoGfter5Y="
  #   ];
  #   # keep-sorted end
  # };

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

    steam-voicechat-fix = {
      url = "github:hilorioze/steam-voicechat-fix";

      inputs.nixpkgs.follows = "nixpkgs"; # remove when using the binary cache above
    };
  };

  outputs = inputs @ {nixpkgs, ...}: {
    nixosConfigurations.example = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";

      specialArgs = {
        inherit inputs;
      };

      modules = [
        (
          {inputs, ...}: {
            nixpkgs.overlays = [inputs.steam-voicechat-fix.overlays.default];
          }
        )

        (
          {pkgs, ...}: {
            programs.steam = {
              enable = true;

              package = pkgs.steam.override {
                extraEnv.LD_PRELOAD = "${pkgs.pkgsi686Linux.steam-voicechat-fix}/lib/libsteam_voicechat_fix.so";
              };
            };
          }
        )
      ];
    };
  };
}
```

The example above adds the overlay, which makes `steam-voicechat-fix` available
through `pkgs.pkgsi686Linux`, and the Steam package override uses it to
configure `LD_PRELOAD`

Alternatively, you can reference it directly instead of adding the overlay:

```nix
{
  programs.steam.package = pkgs.steam.override {
    extraEnv.LD_PRELOAD = "${inputs.steam-voicechat-fix.packages.i686-linux.default}/lib/libsteam_voicechat_fix.so";
  };
}
```

### Other distributions

1. Clone the repository:

   ```sh
   git clone https://github.com/hilorioze/steam-voicechat-fix.git
   cd steam-voicechat-fix
   ```

2. Install the prerequisites. On Debian or Ubuntu:

   ```sh
   sudo apt install --no-install-recommends cmake gcc-multilib make
   ```

   Other distributions need CMake, `make`, and a GCC multilib toolchain with
   i686 libc development files

3. Build the library:

   ```sh
   cmake -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build
   ```

   The resulting library should appear at `build/libsteam_voicechat_fix.so`

## Troubleshooting

Start Steam from a terminal and include all `steam-voicechat-fix:` messages
when reporting a problem
