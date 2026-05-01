# Unveiler

A GTK4/libadwaita archive manager for Linux, built on [7-Zip](https://7-zip.org) 26.01.
Unveiler brings the familiar Windows 7-Zip experience to Linux, making it easy for former Windows users to feel right at home.

![GNOME](https://img.shields.io/badge/GNOME-GTK4%20%2B%20libadwaita-blue) ![GitHub top language](https://img.shields.io/github/languages/top/Gabriel2Silva/Unveiler)

## Features

- Browse and extract archives with a native GNOME interface
- Supports 7z, ZIP, RAR, TAR, GZ, BZ2, XZ, Zstd, CAB, ISO, WIM, RPM, DEB, CPIO, ARJ, LZH, DMG, and more
- Drag-and-drop extraction!
- Search within archives
- Password-protected archive support
- Column sorting (name, size, date)

## Building

### Dependencies

- GCC/G++
- GTK 4
- libadwaita
- pkg-config

On Ubuntu/Debian:
```bash
sudo apt install build-essential libgtk-4-dev libadwaita-1-dev pkg-config python3
```

### Compile

```bash
# 1. Build the 7z.so codec plugin
cd 7zip-main/CPP/7zip/Bundles/Format7zF
make -f makefile.gcc -j$(nproc)
cd -

# Then build:
make
```

### Run (without installing)

```bash
cp 7zip-main/CPP/7zip/Bundles/Format7zF/_o/7z.so _o/
_o/unveiler
```

### Install

```bash
sudo make install
```

Installs to `/usr/local` by default. Use `PREFIX=/opt/unveiler` for a custom location.

## Architecture

Unveiler loads 7-Zip's `7z.so` plugin at runtime through a C bridge layer:

```
┌──────────────┐     ┌──────────────┐     ┌──────────┐
│  GTK4 App    │───▶│  C Bridge    │───▶│  7z.so   │
│  (C / GLib)  │     │  (C++ / COM) │     │  (7-Zip) │
└──────────────┘     └──────────────┘     └──────────┘
```

- `src/` — GTK4 application (pure C)
- `bridge/` — C API wrapping 7-Zip's COM-style `IInArchive` interface
- `7zip-main/` — upstream 7-Zip source (builds `7z.so`)

## License

Unveiler bridge and application code: GPLv3

7-Zip source code: see `7zip-main/DOC/License.txt` (LGPL + unRAR restriction)
