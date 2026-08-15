{ package }:
{ config, lib, ... }:

let
  cfg = config.services.mangobar;
in
{
  options.services.mangobar = {
    enable = lib.mkEnableOption "mangobar Wayland status bar";

    package = lib.mkOption {
      type = lib.types.package;
      default = package;
      defaultText = lib.literalExpression "inputs.mangobar.packages.\${pkgs.system}.default";
      description = "The mangobar package to run.";
    };

    systemdTarget = lib.mkOption {
      type = lib.types.str;
      default = "graphical-session.target";
      description = "The user systemd target that manages mangobar.";
    };
  };

  config = lib.mkIf cfg.enable {
    home.packages = [ cfg.package ];

    systemd.user.services.mangobar = {
      Unit = {
        Description = "mangobar Wayland status bar";
        After = [ cfg.systemdTarget ];
        PartOf = [ cfg.systemdTarget ];
      };

      Service = {
        ExecStart = lib.getExe cfg.package;
        Restart = "on-failure";
        RestartSec = 3;
      };

      Install.WantedBy = [ cfg.systemdTarget ];
    };
  };
}
