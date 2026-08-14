{pkgs, ...}: {
  packages = [pkgs.pkgsi686Linux.glibc.dev];

  scripts.clangd-i686-gcc.exec = ''exec nix develop path:$DEVENV_ROOT#packages.i686-linux.default -c gcc "$@"'';

  languages = {
    # keep-sorted start block=yes newline_separated=yes
    c.enable = true;

    nix = {
      enable = true;

      lsp.package = pkgs.nil;
    };
    # keep-sorted end
  };

  treefmt = {
    enable = true;

    config.programs = {
      # keep-sorted start block=yes newline_separated=yes
      alejandra = {
        enable = true;

        priority = 100;
      };

      clang-format.enable = true;

      deadnix.enable = true;

      keep-sorted.enable = true;

      statix.enable = true;
      # keep-sorted end
    };
  };

  git-hooks.hooks = {
    # keep-sorted start block=yes newline_separated=yes
    actionlint.enable = true;

    check-merge-conflicts = {
      enable = true;

      fail_fast = true; # abort immediately so treefmt never runs on conflicted files
    };

    # use `.editorconfig` as the single source of truth for generic file normalization
    eclint = {
      enable = true;

      types = ["text"];

      # `eclint` only processes the first positional path, so let it discover tracked files itself
      pass_filenames = false;

      settings.fix = true;
    };

    flake-checker.enable = true;

    treefmt = {
      enable = true;

      after = ["check-merge-conflicts"];
    };
    # keep-sorted end
  };

  devcontainer = {
    enable = true;

    settings = {
      # cache `/nix` between rebuilds
      mounts = ["source=devcontainer-nix,target=/nix,type=volume"];

      onCreateCommand = "sudo sh -c 'echo \"accept-flake-config = true\" >> /etc/nix/nix.conf'";

      customizations.vscode.extensions = [
        # keep-sorted start
        "EditorConfig.EditorConfig"
        "jnoortheen.nix-ide"
        "mkhl.direnv"
        "ms-vscode.cmake-tools"
        "ms-vscode.cpptools"
        # keep-sorted end
      ];
    };
  };

  # ensure generated files (like `.devcontainer/devcontainer.json`) exist before `treefmt` runs to prevent race conditions
  tasks."devenv:treefmt:run".after = ["devenv:files"];
}
