{
  inputs = {
    nixpkgs = {
      type = "indirect";
      id = "nixpkgs";
      ref = "8c50a710ddca43d7a530fb805ad55bde8d0141c5";
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
