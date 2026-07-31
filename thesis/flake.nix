{
  inputs = {
    nixpkgs = {
      type = "indirect";
      id = "nixpkgs";
      ref = "624af665418d3c65d544145b4d34ad696439570e";
    };
  };

  outputs =
    { self, nixpkgs }:
    let
      inherit (nixpkgs) lib;
      forAllSystems = lib.genAttrs lib.systems.flakeExposed;
      forAllSystemsWithPkgs = f: forAllSystems (system: f system nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAllSystemsWithPkgs (
        system: pkgs: {
          default = pkgs.callPackage ./package.nix { };
        }
      );
    };
}
