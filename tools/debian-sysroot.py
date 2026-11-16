#!/usr/bin/env python3
"""Populate a Debian arm64 sysroot for Linux ARM64 cross builds.

The Linux ARM64 target uses Debian trixie/arm64, so the libraries the cross
build links against are that suite's packages. This tool resolves a dependency
closure from the archive index, downloads the .debs into a cache, and unpacks
their payloads into a sysroot. Nothing runs package maintainer scripts: a
sysroot only needs headers, static archives and shared objects.

    tools/debian-sysroot.py                            # populate build/sysroot-arm64
    tools/debian-sysroot.py --verify                   # compare the tree with the lock
    tools/debian-sysroot.py --update-lock --dry-run    # refresh the lock, no downloads
    tools/debian-sysroot.py --update-lock              # move to current versions

The resolved package list is recorded in tools/sysroots/arm64.lock. Later runs
resolve against that lock, so a Debian point release cannot silently change what
the build links; --update-lock is the deliberate way to move. Only Debian arm64
is supported: the Linux ARM64 build targets a 64-bit aarch64 userland.

The sysroot is idempotent: packages already unpacked at the same version are
skipped, so re-running only picks up changes. The tree and the .deb cache are
derived build artifacts under build/, which is ignored; the lock is the tracked
input.

Unpacking a zstd-compressed payload needs bsdtar (libarchive-tools on Linux;
macOS ships /usr/bin/bsdtar).
"""

import argparse
import fnmatch
import io
import lzma
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.error
import urllib.request

MIRROR = "https://deb.debian.org/debian"

# Debian architecture name for the Linux ARM64 target (aarch64 userland).
ARCH = "arm64"

# SDL2 and the libraries its headers and build probes reference. The runtime
# builds SDL2 from source, but its CMake configure needs the target's X11,
# Wayland, EGL, GL/GLES, audio and input headers/libraries. libsdl2-dev's own
# Depends pull those in, and the closure is resolved from the archive rather
# than hardcoded.
DEFAULT_PACKAGES = [
    "libsdl2-dev",
    # SDL_syswm.h includes the windowing headers of every backend the target's
    # SDL was built with, so compiling against it needs these even though the
    # emulator itself only uses the window/KMSDRM paths.
    "libx11-dev",
    "libwayland-dev",
]

# Not build inputs; their payloads only bloat the sysroot.
PRUNE_PATHS = ["usr/share/doc", "usr/share/man", "usr/share/locale", "usr/share/lintian"]
PRUNE_KEEP_PATHS = ["usr/share/doc/pkg-config", "usr/share/doc/libc6-dev"]


def log(message):
    print(message, flush=True)


def fetch(url, dest):
    """Download url to dest (skipping it when already cached)."""
    if os.path.exists(dest) and os.path.getsize(dest) > 0:
        return dest
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    partial = dest + ".part"

    for attempt in range(3):
        try:
            with urllib.request.urlopen(url, timeout=60) as response:
                with open(partial, "wb") as out:
                    shutil.copyfileobj(response, out)
            os.replace(partial, dest)
            return dest
        except (urllib.error.URLError, TimeoutError) as error:
            if attempt == 2:
                raise SystemExit("%s: %s" % (url, error))
            log("  retrying %s (%s)" % (url, error))
    return dest


def load_index(suite, component, arch):
    """Return {package: {field: value}} for one component of a suite."""
    url = "%s/dists/%s/%s/binary-%s/Packages.xz" % (MIRROR, suite, component, arch)
    log("index %s" % url)
    with urllib.request.urlopen(url, timeout=120) as response:
        raw = lzma.decompress(response.read()).decode("utf-8", "replace")

    packages = {}
    for stanza in raw.split("\n\n"):
        entry = {}
        for line in stanza.splitlines():
            if line[:1] in (" ", "\t") or ":" not in line:
                continue
            key, _, value = line.partition(":")
            entry[key.strip()] = value.strip()
        if "Package" in entry and "Filename" in entry:
            packages[entry["Package"]] = entry
    return packages


def dependency_groups(entry):
    """Yield the alternatives of each dependency, in preference order."""
    for field in ("Pre-Depends", "Depends"):
        value = entry.get(field)
        if not value:
            continue
        for group in value.split(","):
            alternatives = []
            for item in group.split("|"):
                name = item.split("(")[0].strip()
                # "libc6:arm64" and "libc6-dev:any" name the same package.
                name = name.split(":")[0].strip()
                if name:
                    alternatives.append(name)
            if alternatives:
                yield alternatives


# Packages that only exist to build software: compilers, script runtimes,
# packaging helpers. A -dev package depends on them, but their payloads are of
# no use when compiling against its headers, and unpacking a python or gcc into
# the sysroot only invites picking up the wrong headers.
TOOLING = (
    "python3*", "perl*", "ruby*", "gcc*", "g++*", "cpp*", "make",
    "dpkg*", "debconf*", "debhelper*", "dh-*", "cmake*", "ninja-*", "meson*",
    "autoconf*", "automake*", "autotools-*", "libtool", "pkg-config", "pkgconf*",
    "sed", "tar", "m4", "patch", "quilt", "gettext*", "intltool", "po-debconf",
    "xml-core", "sgml-base", "docbook*", "xsltproc*", "libxml2-utils",
    "openjdk-*", "default-jre*", "lsb-base", "sensible-utils", "ca-certificates",
)


def is_tooling(name):
    return any(fnmatch.fnmatch(name, pattern) for pattern in TOOLING)


def resolve(seeds, index, full_deps=False):
    """Dependency closure of seeds, first-satisfiable alternative per group.

    Without full_deps the closure stops at packages whose only purpose is to
    build software (see TOOLING); headers and libraries are followed, because
    compiling against SDL's headers needs the X11 and Wayland ones they name.
    """
    wanted = []
    seen = set()
    missing = set()
    queue = list(seeds)

    while queue:
        name = queue.pop(0)
        if name in seen:
            continue
        seen.add(name)
        entry = index.get(name)
        if not entry:
            # Virtual packages ("awk", "debconf-2.0") have no files of their
            # own; a build input never depends on one for headers.
            missing.add(name)
            continue
        wanted.append(entry)
        for alternatives in dependency_groups(entry):
            for candidate in alternatives:
                if candidate in seen:
                    break
                if full_deps or not is_tooling(candidate):
                    if candidate in index:
                        queue.append(candidate)
                        break
    return wanted, sorted(missing - seen)


def ar_members(data):
    """Yield (name, payload) from an ar archive (.deb, no symbol table)."""
    if data[:8] != b"!<arch>\n":
        raise SystemExit("not an ar archive")
    offset = 8
    while offset + 60 <= len(data):
        header = data[offset : offset + 60]
        name = header[:16].decode("ascii", "replace").strip()
        try:
            size = int(header[48:58].decode("ascii").strip())
        except ValueError:
            break
        offset += 60
        yield name.rstrip("/"), data[offset : offset + size]
        offset += size + (size & 1)


def unpack(deb, dest):
    """Extract the data.tar.* payload of a .deb into dest."""
    with open(deb, "rb") as handle:
        archive = handle.read()

    for name, payload in ar_members(archive):
        if not name.startswith("data.tar"):
            continue
        if name.endswith(".zst"):
            # libarchive handles zstd when Python's tarfile cannot.
            try:
                result = subprocess.run(
                    ["bsdtar", "-xf", "-", "-C", dest], input=payload
                )
            except FileNotFoundError:
                raise SystemExit(
                    "%s: bsdtar is required to unpack zstd payloads "
                    "(install libarchive-tools)" % deb
                )
            if result.returncode != 0:
                raise SystemExit("%s: bsdtar failed" % deb)
            return
        with tarfile.open(fileobj=io.BytesIO(payload), mode="r:*") as tar:
            try:
                tar.extractall(dest, filter="fully_trusted")
            except TypeError:
                tar.extractall(dest)
        return
    raise SystemExit("%s: no data.tar payload" % deb)


def lock_path(root, arch):
    return os.path.join(root, "tools", "sysroots", arch + ".lock")


def read_lock(path):
    """Return the recorded entries, or None when there is no lock."""
    if not os.path.exists(path):
        return None

    entries = []
    with open(path) as handle:
        for line in handle:
            fields = line.split()
            if not fields or line.startswith("#"):
                continue
            if fields[0] == "suite":
                continue
            # package version filename
            if len(fields) == 3:
                entries.append({"Package": fields[0], "Version": fields[1], "Filename": fields[2]})
    return entries or None


def write_lock(path, suite, arch, entries):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as handle:
        handle.write("# %s: resolved by tools/debian-sysroot.py --update-lock\n" % arch)
        handle.write("# Regenerate deliberately; the build resolves against this list.\n")
        handle.write("suite %s\n" % suite)
        for entry in sorted(entries, key=lambda item: item["Package"]):
            handle.write("%s %s %s\n" % (entry["Package"], entry["Version"], entry["Filename"]))


def stamp_path(dest, entry):
    return os.path.join(dest, ".debs", "%s_%s.stamp" % (entry["Package"], entry["Version"]))


def prune(dest):
    for relative in PRUNE_PATHS:
        path = os.path.join(dest, relative)
        if os.path.isdir(path):
            for keep in PRUNE_KEEP_PATHS:
                if keep.startswith(relative + "/"):
                    shutil.rmtree(
                        os.path.join(path, keep.split("/", len(relative.split("/")))[-1]),
                        ignore_errors=True,
                    )
            shutil.rmtree(path, ignore_errors=True)


def multiarch_dir(dest, arch):
    """The triplet directory Debian installs libraries into (aarch64-linux-gnu)."""
    libraries = os.path.join(dest, "usr", "lib")
    if os.path.isdir(libraries):
        for name in sorted(os.listdir(libraries)):
            if name.endswith("-linux-gnu") or "-linux-gnu" in name:
                return name
    return arch


def pkgconfig_paths(dest, arch):
    return [
        os.path.join(dest, "usr/lib/%s/pkgconfig" % multiarch_dir(dest, arch)),
        os.path.join(dest, "usr/lib/pkgconfig"),
        os.path.join(dest, "usr/share/pkgconfig"),
    ]


def populate(args, root, arch):
    dest = os.path.abspath(args.dest or os.path.join(root, "build", "sysroot-" + arch))
    cache = os.path.abspath(args.cache or os.path.join(root, "build", "debs-" + arch))
    seeds = [name.strip() for name in args.packages.split(",") if name.strip()]
    record = lock_path(root, arch)
    locked = None if args.update_lock else read_lock(record)

    if locked:
        wanted = locked
        log("%s: %d package(s) from %s" % (arch, len(wanted), os.path.relpath(record, root)))
    else:
        index = {}
        for component in args.components.split(","):
            index.update(load_index(args.suite, component.strip(), arch))

        unknown = [name for name in seeds if name not in index]
        if unknown:
            raise SystemExit("not in the index: %s" % ", ".join(unknown))

        wanted, missing = resolve(seeds, index, args.full_deps)
        log("%s: %d package(s) needed, %d virtual name(s) skipped" % (arch, len(wanted), len(missing)))
        if args.dry_run:
            for entry in sorted(wanted, key=lambda item: item["Package"]):
                log("  %-32s %s" % (entry["Package"], entry["Version"]))
            if args.update_lock:
                write_lock(record, args.suite, arch, wanted)
                log("%s: wrote %s" % (arch, os.path.relpath(record, root)))
            return 0

    if args.verify:
        absent = [entry for entry in wanted
                  if not os.path.exists(stamp_path(dest, entry))]
        if absent:
            log("%s: %d package(s) missing from %s" % (arch, len(absent), dest))
            for entry in absent[:10]:
                log("  %s %s" % (entry["Package"], entry["Version"]))
            return 1
        log("%s: sysroot matches the lock" % arch)
        return 0

    os.makedirs(dest, exist_ok=True)
    os.makedirs(os.path.join(dest, ".debs"), exist_ok=True)
    os.makedirs(cache, exist_ok=True)

    downloaded = 0
    unpacked = 0
    for entry in sorted(wanted, key=lambda item: item["Package"]):
        deb = os.path.join(cache, os.path.basename(entry["Filename"]))
        stamp = stamp_path(dest, entry)

        if os.path.exists(stamp):
            continue
        fetch("%s/%s" % (MIRROR, entry["Filename"]), deb)
        downloaded += 1
        unpack(deb, dest)
        unpacked += 1
        os.makedirs(os.path.dirname(stamp), exist_ok=True)
        with open(stamp, "w") as handle:
            handle.write(entry["Filename"] + "\n")

    if not args.keep_doc:
        prune(dest)

    if args.update_lock or not locked:
        write_lock(record, args.suite, arch, wanted)
        log("%s: wrote %s" % (arch, os.path.relpath(record, root)))

    log("%s: %s (%d unpacked this run, %d new download(s))"
        % (arch, dest, unpacked, downloaded))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--suite", default="trixie", help="Debian suite (default: trixie)")
    parser.add_argument("--components", default="main", help="comma-separated archive components")
    parser.add_argument(
        "--dest", default=None, help="sysroot directory (default: build/sysroot-<arch>)"
    )
    parser.add_argument(
        "--cache", default=None, help=".deb cache directory (default: build/debs-<arch>)"
    )
    parser.add_argument(
        "--packages",
        default=",".join(DEFAULT_PACKAGES),
        help="comma-separated seed packages (default: %s)" % ",".join(DEFAULT_PACKAGES),
    )
    parser.add_argument("--keep-doc", action="store_true", help="keep man pages and locale data")
    parser.add_argument(
        "--full-deps",
        action="store_true",
        help="also unpack compilers and script runtimes (a full dpkg-equivalent "
        "closure; not needed to compile against the libraries)",
    )
    parser.add_argument("--dry-run", action="store_true", help="resolve and list, download nothing")
    parser.add_argument("--update-lock", action="store_true",
                        help="re-resolve and rewrite tools/sysroots/<arch>.lock")
    parser.add_argument("--verify", action="store_true",
                        help="check the installed tree against the lock, download nothing")
    parser.add_argument(
        "--print-pkgconfig", action="store_true", help="print the pkg-config environment and exit"
    )
    args = parser.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    arch = ARCH
    dest = os.path.abspath(args.dest or os.path.join(root, "build", "sysroot-" + arch))

    if args.print_pkgconfig:
        print("PKG_CONFIG_LIBDIR=%s" % ":".join(pkgconfig_paths(dest, arch)))
        return 0

    result = populate(args, root, ARCH)
    if result != 0:
        return result

    if args.dry_run or args.verify:
        return 0

    print()
    print("Linux ARM64 sysroot: %s" % dest)
    print("Point the cross toolchain's sysroot at that directory.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
