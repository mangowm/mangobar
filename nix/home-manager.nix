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

    settings = lib.mkOption {
      type = lib.types.nullOr lib.types.attrs;
      default = null;
      example = lib.literalExpression ''
        {
          modules-left = [ "workspaces" "layout" "window" ];
          modules-right = [ "cpu" "memory" "clock#time" ];
        }
      '';
      description = "JSON-serializable mangobar settings written to config.jsonc.";
    };

    configFile = lib.mkOption {
      type = lib.types.nullOr lib.types.path;
      default = null;
      example = lib.literalExpression "./config.jsonc";
      description = "An existing mangobar JSONC configuration file.";
    };
  };

  config = lib.mkIf cfg.enable {
    assertions = [
      {
        assertion = cfg.settings == null || cfg.configFile == null;
        message = "services.mangobar.settings and services.mangobar.configFile are mutually exclusive.";
      }
    ];

    home.packages = [ cfg.package ];

    xdg.configFile = lib.mkIf (cfg.settings != null) {
      "mangobar/config.jsonc".text = builtins.toJSON cfg.settings;
    };

    systemd.user.services.mangobar = {
      Unit = {
        Description = "mangobar Wayland status bar";
        After = [ cfg.systemdTarget ];
        PartOf = [ cfg.systemdTarget ];
        X-Restart-Triggers =
          [ cfg.package ]
          ++ lib.optional (cfg.settings != null)
            config.xdg.configFile."mangobar/config.jsonc".source
          ++ lib.optional (cfg.configFile != null) (toString cfg.configFile);
      };

      Service = {
        ExecStart = lib.getExe cfg.package;
        Environment = lib.optional (cfg.configFile != null)
          "MANGOBAR_CONFIG=${cfg.configFile}";
        Restart = "on-failure";
        RestartSec = 3;
      };

      Install.WantedBy = [ cfg.systemdTarget ];
    };
  };
}
