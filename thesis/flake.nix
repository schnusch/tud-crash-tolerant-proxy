{
  inputs = {
    nixpkgs = {
      type = "indirect";
      id = "nixpkgs";
      ref = "56c02bc00adcf003215cc4bd996d6efaf4cff188";
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
