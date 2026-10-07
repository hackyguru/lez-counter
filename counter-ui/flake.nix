{
  description = "lezcounter — QML frontend for the lezcounter_core module";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    # The core module from this same repo. To build against a local checkout:
    #   nix build --override-input lezcounter_core path:../counter-core '.#lgx-portable'
    lezcounter_core.url = "github:hackyguru/lez-counter?dir=counter-core";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
