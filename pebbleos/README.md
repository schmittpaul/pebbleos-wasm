# The radial launcher

The firmware in `firmware/pebbleos-dev/` is PebbleOS with its system
launcher replaced: instead of a vertical list, the app list sits at hour
positions on a clock face, over the watch hands.

**You do not need any of this to run the demo.** The built firmware is
committed; see the repository README. This is only for changing the
launcher.

## What is here

| File | What |
|------|------|
| `radial_menu_layer.c` | The launcher itself, for reading without a checkout |
| `radial-launcher.patch` | The same change as a patch against upstream PebbleOS |

The patch applies to [coredevices/PebbleOS](https://github.com/coredevices/PebbleOS)
at commit `e8b36268a7b885fb14b4cd7bc9e57246ed1a342b`.

## How it works

It is behind `CONFIG_LAUNCHER_RADIAL`, so the stock list launcher stays
buildable and the two can be compared. `radial_menu_layer.c` implements the
same `launcher_menu_layer_*` API the stock one does, so `launcher.c` is
untouched and shared. The launcher sources are globbed by CMake, so
`menu_layer.c` is guarded to compile only when the option is off.

The app list comes from `app_menu_data_source_get_count()` and
`_get_node_at_index()`; selecting one calls:

```c
app_manager_put_launch_app_event(&(AppLaunchEventConfig){ .id = node->install_id, ... });
```

which is exactly what the stock launcher does.

Three concentric zones that do not overlap: the hands at the centre, the
apps on the ring, the selected app's name in a band below. The launcher
draws the time itself from `rtc_get_time()` — it runs *in place of* the
watchface, so the watchface's own hands are not available to draw under it.

## Building it

```sh
git clone https://github.com/coredevices/PebbleOS.git ~/dev/PebbleOS
cd ~/dev/PebbleOS
git checkout e8b36268a7b885fb14b4cd7bc9e57246ed1a342b
git submodule update --init --recursive --depth 1
git am < /path/to/this/repo/pebbleos/radial-launcher.patch
```

Toolchain and dependencies (macOS; Linux differs only in the package
manager):

```sh
brew install python freetype gettext llvm cmake lcov ninja node librsvg
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
```

The ARM toolchain and QEMU come from the PebbleOS SDK bundle, installed at
`~/pebbleos-sdk-<version>` — see
[PebbleOS-SDK](https://github.com/coredevices/PebbleOS-SDK). Use its
toolchain rather than Homebrew's `arm-none-eabi-gcc`, which is too new.

```sh
. ~/pebbleos-sdk-0.1.8/env.sh
. .venv/bin/activate
pbl configure --board=qemu_emery -DCONFIG_LAUNCHER_RADIAL=y
pbl build
cmake --build build --target qemu_image_micro qemu_image_spi
```

A full build is about 25 seconds.

Two traps, each of which costs a configure cycle:

- **Activate the venv; do not just put it on `PATH`.** CMake's `FindPython3`
  picks the Homebrew interpreter otherwise and `kconfiglib` is not there.
  `. .venv/bin/activate` sets `VIRTUAL_ENV`, which is what CMake keys on.
- **`pbl build` does not produce the flash images.** The
  `qemu_image_micro` and `qemu_image_spi` targets do.

## Putting a new build back into the emulator

The images land at `build/qemu_{micro,spi}_flash.bin` — exactly what this
emulator loads. The committed copies are gzipped, because the SPI image is
33 MB raw and 400 KB compressed; `index.html` falls back to `<name>.gz` and
gunzips in the browser.

```sh
cd /path/to/this/repo
cp ~/dev/PebbleOS/build/qemu_micro_flash.bin firmware/pebbleos-dev/
cp ~/dev/PebbleOS/build/qemu_spi_flash.bin   firmware/pebbleos-dev/
gzip -9 -c firmware/pebbleos-dev/qemu_micro_flash.bin > firmware/pebbleos-dev/qemu_micro_flash.bin.gz
gzip -9 -c firmware/pebbleos-dev/qemu_spi_flash.bin   > firmware/pebbleos-dev/qemu_spi_flash.bin.gz
node scripts/boot-firmware.mjs --fw pebbleos-dev --eink --press select --out /tmp/fw.png
```

Only the `.gz` are committed; the raw `.bin` are gitignored build output.

Saved watch storage in the browser is keyed on the firmware name, so a
rebuilt firmware will not boot into an image the previous one wrote.
