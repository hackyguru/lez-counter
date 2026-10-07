# LEZ Counter

One number on the Logos Execution Zone that everyone shares. A tiny program
is deployed once to the public LEZ v0.3 testnet, and every copy of this
Basecamp module, on anyone's machine, reads and bumps the same counter. It's
the "hello world" of LEZ contracts, like a counter contract on an Ethereum
testnet.

```
lez-counter/
├── counter-program/   the on-chain program (Rust → risc0 guest), build.sh, counter.bin
├── counter-core/      lezcounter_core — universal C++ module, links the LEZ wallet FFI
│   └── tests/         harness: deploy / use / increment / state against the testnet
├── counter-ui/        lezcounter — QML (big number, +1/+5, live feed, details)
└── install.sh         drop both into a local Basecamp
```

## Download

Grab `logos-lezcounter_core-module-lib.lgx` and `logos-lezcounter-module.lgx` from the
[latest release](https://github.com/hackyguru/lez-counter/releases/latest). Each holds
both macOS (Apple Silicon) and Linux (x86_64) builds. In Basecamp, open
**Modules → Install LGX Package** and install the core first, then the UI.
Releases are built by [`.github/workflows/release.yml`](.github/workflows/release.yml)
when a `v*` tag is pushed.

## Live deployment

| | |
|---|---|
| Program (the "contract address") | `5ShXhcA9R972B2BgbomP2Qi3JM1gxGw7Yf5NHwA1fvv4` |
| Counter account (where the number lives) | `8kUkUj457FX6bEQSdk48jbXhsUAdR35avo1J2Hp6j3iy` |
| Image ID | `4316d160e5883685c3015b45dd9f5873b3b7d2e07b61192aafe90bd3e5d64223` |
| Deployed | 2026-10-02: 3 segments + header, ~6½ min, 0.0019 LGO |

Verified with two separate wallets: the deployer added 5 (5), a fresh wallet
with no LGO pointed at the address and added 3 after an automatic faucet
top-up (8), and the deployer read 8. A public increment takes about one
block, 30–60 s.

## How it works

- **The program** (`counter-program/src/main.rs`, ~40 lines) takes a Borsh
  `u64` (1–100) and adds it to a little-endian `u64` in its own data on the
  one account it's given. It uses the v0.3 plan/apply model. A program may
  always write its own data on any account, so no signature is needed and
  anyone can increment.
- **Everyone finds the same account** because it's the public PDA of
  (program address, seed `lez-counter/shared/v1`). The program would accept
  any account; the shared seed is what makes it one shared number.
- **The module** has its own embedded wallet. An increment is a generic public
  transaction to the program address, with the module's account co-signing
  as fee payer. LGO comes automatically from the testnet's shared faucet
  account. After inclusion the module re-reads the counter, because a
  transaction can land in a block even when the program rejects it.
- **The live feed** compares each new reading with the last. Rises from your
  own increments show as "You added N"; everything else is "Someone added
  N".

## Build, deploy, install

```bash
# 1. Program (needs Docker running, cargo-risczero 3.0.5, and `rzup install rust`)
cd counter-program && ./build.sh                    # → counter.bin

# 2. Deploy it (only after a testnet reset, or for your own copy)
cd ../counter-core
LEZCOUNTER_DATA_DIR=~/lezcounter-deployer tests/run.sh deploy ../counter-program/counter.bin
#   → result: <program address>; put it in kDefaultProgram (src/counter_impl.cpp)
#     or paste it into the module's Details → "Use a different deployment".

# 3. Module
nix build 'path:.#lgx-portable' -o result-portable
# The UI pins its core to this repo on GitHub; build against your checkout instead:
cd ../counter-ui && nix build 'path:.#lgx-portable' -o result-portable \
    --override-input lezcounter_core path:../counter-core
cd .. && ./install.sh
```

Harness: `tests/run.sh state | use <address> | increment [N] | deploy <bin>`,
with `LEZCOUNTER_DATA_DIR` choosing which wallet (two directories = two
people).

## Verifying it independently

`./verify.sh` reads the counter straight from the testnet sequencer with
`curl`: no Basecamp, no module, no wallet (`./verify.sh watch` follows it).
If the module and the script agree, the number really is on the chain.

**Cross-machine check with a friend:**
1. Both run `./verify.sh` and get the same number.
2. You open LEZ Counter; they install it and open it.
3. They tap **+1**. Within about a minute you see "Someone added 1" in your
   Live feed, and `./verify.sh` shows +1 on both machines.
4. Swap roles.

## Sharing it

The built `.lgx` packages contain the program address, so anyone who installs
them talks to the same counter. Nothing to configure, and LGO is fetched for
them. Two ways to hand it out:

- Publish both modules to the catalog (modules.hackyguru.com). Its CI builds
  Linux and macOS variants, and people install from Basecamp.
- Send the two `.lgx` files. The local build here is **macOS arm64 only**;
  Linux and Windows users need the CI build.

## Things that bit (so you don't have to)

- **The program_deployment README is out of date for v0.3.** There's no
  `wallet deploy-program` any more. Deploying goes through `program_loader`
  (write-once 96 KiB segments plus a header whose account ID is the program's
  address), here via `wallet_ffi_program_loader_deploy`.
- **`cargo risczero build` needs the risc0 Rust toolchain on the host**
  (`rzup install rust`) even though it builds in Docker. It also needs LEZ's
  builder image, `RISC0_DOCKER_CONTAINER_TAG=r0.1.91.1`. The default
  `r0.1.88` is too old for LEZ's dependencies (`ruint` needs rustc 1.90).
  `build.sh` sets this.
- **Testnet resets wipe deployed programs.** Redeploy, then update
  `kDefaultProgram` or use the Details field. Part 5 (the v0.2 SPEL counter)
  is superseded by this one.
