"""Pinned platform downloads. Measured bytes and sources: docs/release/builder-platforms.md."""
import platform

DEBIAN_INSTALLER = {
    "version": "2026-10-20",
    "url": "https://apt.devkitpro.org/install-devkitpro-pacman",
    "sha256": "6a62d5b06c82f8e1e029ac60d3bf6c46d357de4ca5ccffbf9fc92fe410bb97f3",
    "size": 800,
}
MAC_INSTALLER = {
    "version": "6.0.2",
    "url": "https://github.com/devkitPro/pacman/releases/download/v6.0.2/devkitpro-pacman-installer.pkg",
    "sha256": "da74156211cd3d8664ba7fb95544fbc0a2fbef1dd87b1a6bfafb30d2d446cd30",
    "size": 65370714,
}
MAC_CMAKE = {
    "version": "3.31.8",
    "url": "https://github.com/Kitware/CMake/releases/download/v3.31.8/cmake-3.31.8-macos-universal.tar.gz",
    "sha256": "d1449f969c54d5c00886d5b643340d493dfb3c81cb39ee29b35453395c11ebf7",
    "size": 78373419,
}
BANNERTOOL_SOURCE = {
    "version": "1.2.3",
    "url": "https://github.com/carstene1ns/3ds-bannertool/releases/download/1.2.3/bannertool-1.2.3.tar.gz",
    "sha256": "3ae61ff21c7762e75ba37a57f248cc0d6a9d4bff92207c57f7420a86dfbd08e6",
    "size": 210923,
}
MAKEROM = {
    ("linux", "x86_64"): ("ubuntu_x86_64", "dd596854718c195c6e3229286be485b122921715555af8ae5cf8e9a465d9f970", 240725),
    ("darwin", "arm64"): ("macos_arm64", "813fd517ec23d54cacee2c9a718a3b5fda7678a541b37d19dab9f8b1de1297a2", 232548),
    ("darwin", "x86_64"): ("macos_x86_64", "0d382da757833e4dc4fa99721f3cf8d9e1967cefb2a979f75d39bc4ee035f65b", 226712),
}


def machine(value=None):
    value = (value or platform.machine()).lower()
    return {"amd64": "x86_64", "aarch64": "arm64"}.get(value, value)


def makerom_pin(system, arch=None):
    key = (system, machine(arch))
    if key not in MAKEROM:
        raise ValueError("No pinned makerom binary for %s %s. Supported: Linux x86_64, macOS arm64/x86_64."
                         % key)
    suffix, sha, size = MAKEROM[key]
    return {"version": "v0.18.4", "sha256": sha, "size": size,
            "url": "https://github.com/3DSGuy/Project_CTR/releases/download/makerom-v0.18.4/"
                   "makerom-v0.18.4-%s.zip" % suffix}
