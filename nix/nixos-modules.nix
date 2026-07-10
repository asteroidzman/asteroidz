self: {
  config,
  lib,
  pkgs,
  ...
}: let
  cfg = config.programs.asteroidz;
in {
  options = {
    programs.asteroidz = {
      enable = lib.mkEnableOption "asteroidz, a wlroots compositor with a Vulkan renderer and dwm-style tags";
      addLoginEntry = lib.mkOption {
        type = lib.types.bool;
        default = true;
        description = "Whether to add a login entry to the display manager for asteroidz. Only has effect if a display manager is configured (e.g. SDDM, GDM via `services.displayManager`).";
      };
      package = lib.mkOption {
        type = lib.types.package;
        default = self.packages.${pkgs.stdenv.hostPlatform.system}.asteroidz;
        description = "The asteroidz package to use";
      };
    };
  };

  config = lib.mkIf cfg.enable {
    environment.systemPackages =
      [
        cfg.package
      ];

    xdg.portal = {
      enable = lib.mkDefault true;

      config = {
        # Must match XDG_CURRENT_DESKTOP (DesktopNames in asteroidz.desktop).
        asteroidz = {
          default = [
            "gtk"
          ];
          # except those
          "org.freedesktop.impl.portal.Secret" = ["gnome-keyring"];
          "org.freedesktop.impl.portal.ScreenCast" = ["wlr"];
          "org.freedesktop.impl.portal.ScreenShot" = ["wlr"];

          # wlr does not have this interface
          "org.freedesktop.impl.portal.Inhibit" = [];
        };
      };
      extraPortals = with pkgs; [
        xdg-desktop-portal-wlr
        xdg-desktop-portal-gtk
      ];

      wlr.enable = lib.mkDefault true;

      configPackages = [cfg.package];
    };

    security.polkit.enable = lib.mkDefault true;

    programs.xwayland.enable = lib.mkDefault true;

    services = {
      displayManager.sessionPackages = lib.mkIf cfg.addLoginEntry [ cfg.package ];

      graphical-desktop.enable = lib.mkDefault true;
    };
  };
}
