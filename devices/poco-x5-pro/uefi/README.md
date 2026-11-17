# UEFI for redwood

A port of [Project Silicium / Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) to the
POCO X5 Pro 5G, modelled on `lisa` (the same `KodiakPkg` SoC package). You do not need to rebuild
it to run HakiOS: the built image is in `devices/redwood/prebuilt`, and `mkxnuimg.py` takes it.

What is here:

- `BASE`: the Mu-Silicium commit the port is made on;
- `mu-silicium.patch`: changes to existing files: `BootShim/BootShim.S` (copies our "XNUP"
  container behind the UEFI image to 0xF0000000) and `BootManagerLib` (starts the XNU loader from
  the container instead of a menu);
- `files/`: new files: `Platforms/Xiaomi/redwoodPkg` (memory map, driver list),
  `Silicon/.../BootManagerLib/XnuLoader.[ch]`, `Resources/Configs/redwood.toml`,
  `Resources/DTBs/redwood.dtb` (the device tree from the phone's vendor_boot). `Binaries/redwood`
  (the UEFI drivers of the phone's own firmware) is **not tracked**: they are the vendor's, so you
  extract them from your own phone's `xbl.img` with [UEFIReader](https://github.com/WOA-Project/UEFIReader)
  into `files/Binaries/redwood/`; `files/Binaries/redwood/README.md` is kept, it lists the patches
  that were applied to them.

## Build

Packages as in Mu-Silicium's `setup_env.sh` (Arch: `git base-devel lld nasm clang llvm
aarch64-linux-gnu-gcc python python-pip python-virtualenv`).

```sh
devices/redwood/uefi/apply.sh          # clones Mu-Silicium (with submodules, ~3 GB) and applies the port
python3 -m venv venv && venv/bin/pip install -r Mu-Silicium/pip-requirements.txt
cd Mu-Silicium
source ../venv/bin/activate
python3 build_uefi.py -d redwood -r RELEASE     # -> Build/redwoodPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd
```

The `mu_nasm` dependency comes through NuGet and wants Mono; without Mono, point it at the
system nasm:

```sh
D=Mu_Basecore/BaseTools/Bin/mu_nasm_extdep
mkdir -p $D/Linux-x86-64
echo 'version: 20016.1.1' > $D/extdep_state.yaml
ln -s /usr/bin/nasm /usr/bin/ndisasm $D/Linux-x86-64/
```

After a build, `mkxnuimg.py` (called by `./xl enter`) takes the UEFI from `Mu-Silicium/`, and
`tools/update-prebuilt.sh` refreshes `devices/redwood/prebuilt`. `devices/redwood/uefi/export.sh`
is the reverse: it saves the changes from `Mu-Silicium/` here.
