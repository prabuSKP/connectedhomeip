# Phase 2 Virtual Energy Simulator On Linux

This guide is for building and running the Phase 2 virtual energy app in this repository.

Target binary:

- `out/linux-x64-phase2-energy-simulator/chip-phase2-energy-simulator-app`

Target branch:

- `virtual-Eclectrical-device`

## 1. Get The Source

If you already have this repo checked out, skip this section.

```bash
cd ~
git clone --recurse-submodules https://github.com/project-chip/connectedhomeip.git
cd connectedhomeip
git checkout -b virtual-Eclectrical-device origin/virtual-Eclectrical-device
```

## 2. Install Required Host Tools

```bash
sudo apt-get update
sudo apt-get install -y \
  git gcc g++ pkg-config cmake curl unzip \
  libssl-dev libdbus-1-dev libglib2.0-dev libavahi-client-dev \
  ninja-build python3-pip \
  libgirepository1.0-dev libcairo2-dev libreadline-dev libevent-dev \
  default-jre \
  python3.11 python3.11-dev python3.11-venv
```

## 3. Ensure Python 3.11+

```bash
python3 --version
```

Expected: `3.11.x` or newer.

If your default `python3` is older, prepend a Python 3.11 shim before activation:

```bash
export PATH=/home/$USER/.local/py311-shim:$PATH
```

## 4. Check Out Required Submodules

```bash
git submodule update --init --depth 1 \
  third_party/pigweed/repo \
  third_party/openthread/repo \
  third_party/editline/repo

python3 scripts/checkout_submodules.py --shallow --platform linux
```

If you need all platform SDKs (larger checkout):

```bash
git submodule update -f --init --recursive
```

## 5. Bootstrap And Activate Build Environment

```bash
source scripts/bootstrap.sh -p linux
source scripts/activate.sh -p linux
```

Note:

- Use `source scripts/activate.sh`.
- `source/activate.sh` is not a valid path in this repo.

## 6. Build The Phase 2 Virtual App

```bash
./scripts/build/build_examples.py --target linux-x64-phase2-energy-simulator build
```

Verify the binary exists:

```bash
ls -l out/linux-x64-phase2-energy-simulator/chip-phase2-energy-simulator-app
file out/linux-x64-phase2-energy-simulator/chip-phase2-energy-simulator-app
```

## 7. Run One Clean End-To-End Build (Recommended)

This removes previous artifacts and verifies a fresh build from scratch.

```bash
rm -rf out/linux-x64-phase2-energy-simulator
./scripts/build/build_examples.py --target linux-x64-phase2-energy-simulator build
```

## 8. Run The Virtual App

```bash
./out/linux-x64-phase2-energy-simulator/chip-phase2-energy-simulator-app \
  --discriminator 3840 \
  --passcode 20202021 \
  --secured-device-port 5540 \
  --KVS /tmp/chip-phase2-kvs \
  --enable-key 000102030405060708090a0b0c0d0e0f
```

Useful runtime options:

- `--featureSet <value>` to set DEM FeatureMap.
- `--trace_decode 1` to make tracing easier to read.
- `--help` to print all supported options.

## 9. Re-Run And Re-Commission Tips

If you want to recommission as a fresh device, remove persisted state:

```bash
rm -f /tmp/chip-phase2-kvs
```

Then re-run the app command.

## 10. Optional: Build chip-tool Controller

```bash
./scripts/build/build_examples.py --target linux-x64-chip-tool build
./out/linux-x64-chip-tool/chip-tool --help
```

Use `chip-tool` to commission and test the running virtual app.
