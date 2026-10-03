# linux-esp32s3

Linux 7.2.4 for the ESP32-S3, the kernel that runs on the boards built by
[Linux-on-esp32-S3](https://github.com/paulneja/Linux-on-esp32-S3).
Linux-on-esp32-S3 uses it from version 0.9 on. Linux runs on core 1 as a
NOMMU XIP kernel, and the ESP-IDF firmware on core 0 handles WiFi, BLE and
the flash.

## What is in the tree

The first commit is the official v7.2.4 tree from linux-stable, imported
without the earlier history. Its tree hash matches upstream, so you can check
that the base is unmodified. Everything after it is ESP32-S3 support:

- The platform code and the drivers for the interrupt matrix, GPIO, SPI,
  clocks, the IPC with core 0, the flash, the TRNG and the esp32-ng WiFi/BLE
  driver. Max Filippov ([jcmvbkbc](https://github.com/jcmvbkbc)) wrote these
  for his linux-xtensa tree, and the commits keep his authorship. esp32-ng
  started from Espressif's esp-hosted driver.
- The port of that work from 6.11 to 7.2, and fixes for problems seen on real
  boards: a race in the WiFi command queue, scans left running on stop, a
  console that stalled when the USB port went away, IPC completions and
  locking around flash commands.
- A driver for the hardware RSA accelerator, and `/dev/esp-ble`, the BLE pipe
  Linux-on-esp32-S3 uses to set up WiFi from a phone.
- fork() without an MMU (`CONFIG_XTENSA_NOMMU_FORK`). Parent and child share
  an address range, and the kernel swaps their private memory on every
  context switch between them. Aligned 64 KiB chunks are swapped by
  exchanging entries in the ESP32-S3 cache MMU, which is about 47 times
  faster than copying them, and the rest is copied.

To list the ESP32-S3 commits:

    git log --reverse upstream-v7.2.4..

## Building

The simplest way is the Linux-on-esp32-S3 build, which also produces the
toolchain, the root filesystem and the firmware. To build only the kernel you
need:

- the xtensa-esp32s3-linux-uclibcfdpic toolchain and the `esp32s3.so`
  dynconfig plugin, both made by that build;
- `new-files/board/espressif/esp32s3/devkit_c1_16m_linux.config` from
  Linux-on-esp32-S3;
- `regulatory.db` and `regulatory.db.p7s` from wireless-regdb in `firmware/`,
  because cfg80211 loads them before the root filesystem is mounted.

Then:

    export XTENSA_GNU_CONFIG=/path/to/esp32s3.so
    cp /path/to/devkit_c1_16m_linux.config .config
    scripts/config --enable XTENSA_NOMMU_FORK --set-str LOCALVERSION -forkbank
    make ARCH=xtensa CROSS_COMPILE=xtensa-esp32s3-linux-uclibcfdpic- olddefconfig
    make ARCH=xtensa CROSS_COMPILE=xtensa-esp32s3-linux-uclibcfdpic- xipImage

The image ends up in `arch/xtensa/boot/xipImage`.

## Branches and tags

`esp32s3-7.2` is the current branch. Tags named `v7.2.4-esp32s3.N` mark the
kernels shipped in Linux-on-esp32-S3 releases. A newer stable or LTS kernel
comes in as a new imported base, with the ESP32-S3 commits rebased on top.

## Issues

Kernel bugs, questions and pull requests are welcome here. Problems with the
images, the build, flashing or userspace belong in
[Linux-on-esp32-S3](https://github.com/paulneja/Linux-on-esp32-S3/issues).

## License and credits

Linux is under GPL-2.0, see `COPYING`. Most of the ESP32 support comes from
Max Filippov's work, the WiFi driver builds on Espressif's esp-hosted, and
everything else is upstream Linux and its developers.
