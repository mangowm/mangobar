{
  description = "A Wayland status bar for mangowm";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
      homeManagerModule = args@{ pkgs, ... }:
        (import ./nix/home-manager.nix {
          package = self.packages.${pkgs.stdenv.hostPlatform.system}.mangobar;
        }) args;
    in
    {
      packages = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          mangobar = pkgs.stdenv.mkDerivation {
            pname = "mangobar";
            version = "0.1.1";
            src = self;

            nativeBuildInputs = with pkgs; [
              meson
              ninja
              pkg-config
              python3
              wayland-scanner
              wayland-protocols
            ];

            buildInputs = with pkgs; [
              alsa-lib
              cairo
              cjson
              fcft
              gdk-pixbuf
              libpulseaudio
              pango
              pixman
              systemd
              wayland
            ];

            meta = with pkgs.lib; {
              description = "A Wayland status bar for mangowm";
              homepage = "https://github.com/mangowm/mangobar";
              license = licenses.mit;
              platforms = platforms.linux;
              mainProgram = "mangobar";
            };
          };
        in
        {
          inherit mangobar;
          default = mangobar;
        });

      devShells = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default = pkgs.mkShell {
            inputsFrom = [ self.packages.${system}.mangobar ];
          };
        });

      homeManagerModules = {
        default = homeManagerModule;
        mangobar = homeManagerModule;
      };
    };
}
