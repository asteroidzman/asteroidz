self: {
  lib,
  config,
  pkgs,
  ...
}: let
  cfg = config.wayland.windowManager.asteroidz;

  variables = lib.concatStringsSep " " cfg.systemd.variables;
  extraCommands = lib.concatStringsSep "\n" cfg.systemd.extraCommands;

  # Imports key env vars into the systemd/D-Bus user environment and starts the
  # session target. asteroidz runs this via `spawn-at-startup` (see below).
  sessionSetup = pkgs.writeShellScript "asteroidz-session-setup" ''
    ${pkgs.dbus}/bin/dbus-update-activation-environment --systemd ${variables}
    ${extraCommands}
  '';

  # config.kdl body, with the session-setup spawn appended when systemd is on.
  configText =
    cfg.settings
    + lib.optionalString cfg.systemd.enable "\nspawn-at-startup ${sessionSetup}\n";

  configFile =
    if cfg.validateConfig
    then
      pkgs.runCommand "asteroidz-config.kdl" {} ''
        cp ${pkgs.writeText "config.kdl" configText} "$out"
        ${lib.getExe cfg.package} -c "$out" -p
      ''
    else pkgs.writeText "config.kdl" configText;
in {
  options.wayland.windowManager.asteroidz = with lib; {
    enable = mkEnableOption "asteroidz, a wlroots compositor with a Vulkan renderer and dwm-style tags";

    package = mkOption {
      type = types.package;
      default = self.packages.${pkgs.stdenv.hostPlatform.system}.asteroidz;
      defaultText = literalExpression "asteroidz.packages.\${system}.asteroidz";
      description = "The asteroidz package to use.";
    };

    settings = mkOption {
      type = types.lines;
      default = "";
      description = ''
        Contents of {file}`~/.config/asteroidz/config.kdl`, written verbatim.

        asteroidz is configured in KDL, not a flat key=value file; see the
        shipped `config.kdl` for the full syntax (`misc`, `pill`, `layout`,
        `animations`, `environment`, `binds`, `mouse-binds`, `tag`,
        `window-rule`, `spawn-at-startup`, `source`, ...).

        When empty, no file is written and asteroidz falls back to
        {file}`/etc/asteroidz/config.kdl`.
      '';
      example = literalExpression ''
        '''
          misc {
            focus-on-activate 1
            sdr { reference-luminance 280 }
          }
          binds {
            Super+Return { spawn "kitty"; }
            Super+q { killclient; }
          }
          tag 1 { layout tile }
        '''
      '';
    };

    validateConfig = mkOption {
      type = types.bool;
      default = true;
      description = ''
        Check {option}`settings` at build time with
        {command}`asteroidz -c FILE -p`.

        Disable this if your config uses `source "..."` includes pointing at
        files that are not present during the Nix build — the include cannot be
        resolved in the build sandbox and validation would fail.
      '';
    };

    systemd = {
      enable = mkOption {
        type = types.bool;
        default = pkgs.stdenv.isLinux;
        description = ''
          Start {file}`asteroidz-session.target` on startup, bound to
          {file}`graphical-session.target`. A `spawn-at-startup` entry is added
          to the generated config that imports key environment variables into
          the systemd and D-Bus user environment and starts the target.

          Requires {option}`settings` to be set (the spawn entry is written into
          the generated config.kdl).
        '';
      };
      variables = mkOption {
        type = types.listOf types.str;
        default = [
          "DISPLAY"
          "WAYLAND_DISPLAY"
          "XDG_CURRENT_DESKTOP"
          "XDG_SESSION_TYPE"
          "NIXOS_OZONE_WL"
          "XCURSOR_THEME"
          "XCURSOR_SIZE"
        ];
        example = ["--all"];
        description = "Environment variables imported into the systemd and D-Bus user environment.";
      };
      extraCommands = mkOption {
        type = types.listOf types.str;
        default = [
          "systemctl --user reset-failed"
          "systemctl --user start asteroidz-session.target"
        ];
        description = "Commands run after the D-Bus environment is updated.";
      };
      xdgAutostart = mkEnableOption ''
        autostart of applications using
        {manpage}`systemd-xdg-autostart-generator(8)`
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    home.packages = [cfg.package];

    xdg.configFile."asteroidz/config.kdl" =
      lib.mkIf (cfg.settings != "") {source = configFile;};

    systemd.user.targets.asteroidz-session = lib.mkIf cfg.systemd.enable {
      Unit = {
        Description = "asteroidz compositor session";
        Documentation = ["man:systemd.special(7)"];
        BindsTo = ["graphical-session.target"];
        Wants =
          ["graphical-session-pre.target"]
          ++ lib.optional cfg.systemd.xdgAutostart "xdg-desktop-autostart.target";
        After = ["graphical-session-pre.target"];
        Before = lib.optional cfg.systemd.xdgAutostart "xdg-desktop-autostart.target";
      };
    };
  };
}
