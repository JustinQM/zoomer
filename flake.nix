{
    description = "zoomer - wayland zoom tool";

    inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

    outputs = { self, nixpkgs }:
    let
        system = "x86_64-linux";
        pkgs = nixpkgs.legacyPackages.${system};
    in
    {
        packages.${system}.default = pkgs.stdenv.mkDerivation 
        {
            pname = "zoomer";
            version = "1.0.0";
            src = ./.;

            nativeBuildInputs = with pkgs; 
            [
                pkg-config
                wayland-scanner
            ];

            buildInputs = with pkgs; [
                wayland
                wayland-protocols
                wlr-protocols
                libGL
                mesa
            ];

            installPhase = ''
                mkdir -p $out/bin
                cp build/zoomer $out/bin/zoomer
            '';
        };

        devShells.${system}.default = pkgs.mkShell 
        {
            inputsFrom = [ self.packages.${system}.default ];
        };
    };
}
