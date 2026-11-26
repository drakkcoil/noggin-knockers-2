#!/usr/bin/env python3
"""Build portable native release bundles from completed CMake outputs.

Examples:
  python3 native/package.py macos --binary /tmp/nk-mac/noggin2
  python3 native/package.py windows --binary /tmp/nk-win/noggin2.exe \
      --sdl2-runtime /tmp/SDL2-2.32.6/x86_64-w64-mingw32/bin/SDL2.dll
"""

from __future__ import annotations

import argparse
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent
APP_STEM = "NogginKnockers2"
APP_DISPLAY = "Noggin Knockers 2"


def fail(message: str) -> None:
    raise SystemExit(f"package: {message}")


def run(*command: str) -> str:
    try:
        return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT)
    except (OSError, subprocess.CalledProcessError) as exc:
        detail = exc.output if isinstance(exc, subprocess.CalledProcessError) else str(exc)
        fail(f"{' '.join(command)} failed: {detail}")
    return ""


def checked_file(path: Path, label: str) -> Path:
    path = path.expanduser().resolve()
    if not path.is_file():
        fail(f"{label} is missing: {path}")
    return path


def checked_assets(path: Path) -> Path:
    path = path.expanduser().resolve()
    for name in ("image_index.tsv", "palette_index.tsv", "rgba", "palettes", "music", "extracted"):
        if not (path / name).exists():
            fail(f"asset input is missing {name}: {path}")
    return path


def copy_assets(source: Path, destination: Path) -> None:
    """Keep the runtime assets; omit extraction diagnostics and duplicate PNGs."""
    destination.mkdir(parents=True)
    shutil.copy2(source / "image_index.tsv", destination / "image_index.tsv")
    shutil.copy2(source / "palette_index.tsv", destination / "palette_index.tsv")
    shutil.copytree(source / "rgba", destination / "rgba")
    shutil.copytree(source / "palettes", destination / "palettes")
    shutil.copytree(source / "music", destination / "music")
    wav_files = sorted((source / "extracted").glob("*/audio/*.wav"))
    if not wav_files:
        fail("no extracted WAV samples found")
    palette_files = sorted((source / "extracted").glob("*/records/*_scene.palette.bin"))
    if len(palette_files) != 3:
        fail("expected three extracted scene palettes")
    for asset in wav_files + palette_files:
        relative = asset.relative_to(source)
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(asset, target)


def sdl_license(runtime: Path, override: Path | None) -> Path:
    if override is not None:
        return checked_file(override, "SDL2 license")
    resolved = runtime.resolve()
    for ancestor in list(resolved.parents)[:4]:
        candidate = ancestor / "LICENSE.txt"
        if candidate.is_file():
            return candidate
    fail("SDL2 LICENSE.txt was not found; pass --sdl2-license")
    raise AssertionError


def copy_source(destination: Path) -> None:
    """Ship corresponding GPL source and the scripts needed to rebuild it."""
    native = destination / "native"
    native.mkdir(parents=True)
    shutil.copy2(ROOT / "CMakeLists.txt", native / "CMakeLists.txt")
    shutil.copy2(ROOT / "package.py", native / "package.py")
    for name in ("README.md", "REA_FINDINGS.md"):
        if (ROOT / name).is_file():
            shutil.copy2(ROOT / name, native / name)
    for subdirectory in ("core", "src", "tools"):
        source = ROOT / subdirectory
        if not source.is_dir():
            continue
        for path in sorted(source.rglob("*")):
            if path.is_dir() or "__pycache__" in path.parts:
                continue
            if path.suffix.lower() not in (".c", ".h", ".py", ".md", ".txt") and path.name != "COPYING":
                continue
            target = native / path.relative_to(ROOT)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)


def copy_notices(destination: Path, runtime: Path, license_override: Path | None) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / "packaging" / "README.txt", destination / "README.txt")
    notices = destination / "LICENSES"
    notices.mkdir()
    shutil.copy2(ROOT / "core" / "COPYING", notices / "GPL-2.0.txt")
    shutil.copy2(ROOT / "packaging" / "LICENSE-Bloodlust-MIT.txt", notices / "Bloodlust-MIT.txt")
    shutil.copy2(sdl_license(runtime, license_override), notices / "SDL2.txt")
    shutil.copy2(ROOT / "packaging" / "SOURCE-NOTICE.txt", notices / "SOURCE-NOTICE.txt")
    copy_source(destination / "Source")


def linked_sdl_name(binary: Path) -> str | None:
    for line in run("otool", "-L", str(binary)).splitlines()[1:]:
        name = line.strip().split(" (", 1)[0]
        if "libSDL2" in name and name.endswith(".dylib"):
            return name
    return None


def find_macos_sdl(binary: Path, linked_name: str | None, override: Path | None) -> Path:
    if override is not None:
        return checked_file(override, "SDL2 runtime")
    candidates = []
    if linked_name and linked_name.startswith("/"):
        candidates.append(Path(linked_name))
    candidates += [
        Path("/opt/homebrew/opt/sdl2/lib/libSDL2-2.0.0.dylib"),
        Path("/usr/local/opt/sdl2/lib/libSDL2-2.0.0.dylib"),
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    fail("could not locate the macOS SDL2 dylib; pass --sdl2-runtime")
    raise AssertionError


def package_macos(binary: Path, assets: Path, output: Path,
                  runtime_override: Path | None, license_override: Path | None) -> list[Path]:
    if sys.platform != "darwin":
        fail("macOS app packaging requires macOS install_name_tool and codesign")
    old_sdl = linked_sdl_name(binary)
    runtime = find_macos_sdl(binary, old_sdl, runtime_override)
    target = output / f"{APP_DISPLAY}.app"
    archive_target = output / f"{APP_STEM}-macOS-arm64.zip"
    # Sign outside iCloud-backed workspaces, where Finder metadata can be
    # reattached immediately and rejected by codesign.
    with tempfile.TemporaryDirectory(prefix="nk2-mac-package-") as scratch:
        app = Path(scratch) / target.name
        contents = app / "Contents"
        executable_dir = contents / "MacOS"
        framework_dir = contents / "Frameworks"
        resources = contents / "Resources"
        executable_dir.mkdir(parents=True)
        framework_dir.mkdir()
        resources.mkdir()
        executable = executable_dir / APP_STEM
        shutil.copy2(binary, executable)
        executable.chmod(executable.stat().st_mode | 0o111)
        dylib = framework_dir / "libSDL2-2.0.0.dylib"
        shutil.copy2(runtime, dylib)
        dylib.chmod(dylib.stat().st_mode | 0o200)
        if old_sdl:
            run("install_name_tool", "-change", old_sdl,
                "@executable_path/../Frameworks/libSDL2-2.0.0.dylib", str(executable))
        run("install_name_tool", "-id", "@rpath/libSDL2-2.0.0.dylib", str(dylib))
        with (contents / "Info.plist").open("wb") as file:
            plistlib.dump({
                "CFBundleDevelopmentRegion": "en",
                "CFBundleDisplayName": APP_DISPLAY,
                "CFBundleExecutable": APP_STEM,
                "CFBundleIdentifier": "org.nogginknockers2.native",
                "CFBundleInfoDictionaryVersion": "6.0",
                "CFBundleName": APP_DISPLAY,
                "CFBundlePackageType": "APPL",
                "CFBundleShortVersionString": "0.1.0",
                "CFBundleVersion": "1",
                "LSApplicationCategoryType": "public.app-category.games",
                "NSHighResolutionCapable": True,
            }, file)
        (contents / "PkgInfo").write_bytes(b"APPL????")
        copy_assets(assets, resources / "assets")
        copy_notices(resources, runtime, license_override)
        # iCloud/Finder attributes copied with source files invalidate a macOS
        # bundle signature. The package itself needs no extended attributes.
        run("xattr", "-cr", str(app))
        run("codesign", "--force", "--sign", "-", "--timestamp=none", str(dylib))
        run("codesign", "--force", "--sign", "-", "--timestamp=none", str(executable))
        run("codesign", "--force", "--sign", "-", "--timestamp=none", str(app))
        run("codesign", "--verify", "--deep", "--strict", str(app))
        # A ZIP keeps the signed bundle intact when the output directory is
        # iCloud-backed; File Provider attaches FinderInfo to bare .app folders.
        temporary_zip = Path(scratch) / archive_target.name
        with zipfile.ZipFile(temporary_zip, "w", compression=zipfile.ZIP_DEFLATED,
                             compresslevel=6, allowZip64=True) as archive:
            for path in sorted(app.rglob("*")):
                if path.is_file():
                    archive.write(path, path.relative_to(Path(scratch)))
        os.replace(temporary_zip, archive_target)
        if target.exists():
            shutil.rmtree(target)
        shutil.move(str(app), str(target))
    return [target, archive_target]


def package_windows(binary: Path, assets: Path, output: Path,
                    runtime_override: Path | None, license_override: Path | None) -> list[Path]:
    if runtime_override is None:
        fail("Windows packaging requires --sdl2-runtime pointing to SDL2.dll")
    runtime = checked_file(runtime_override, "SDL2 runtime")
    if runtime.name.lower() != "sdl2.dll":
        fail("Windows SDL2 runtime must be named SDL2.dll")
    target = output / f"{APP_STEM}-Windows-x64.zip"
    with tempfile.TemporaryDirectory(prefix="nk2-win-package-", dir=output) as scratch:
        folder = Path(scratch) / APP_STEM
        folder.mkdir()
        shutil.copy2(binary, folder / f"{APP_STEM}.exe")
        shutil.copy2(runtime, folder / "SDL2.dll")
        copy_assets(assets, folder / "assets")
        copy_notices(folder, runtime, license_override)
        temporary_zip = Path(scratch) / target.name
        with zipfile.ZipFile(temporary_zip, "w", compression=zipfile.ZIP_DEFLATED,
                             compresslevel=6, allowZip64=True) as archive:
            for path in sorted(folder.rglob("*")):
                if path.is_file():
                    archive.write(path, path.relative_to(Path(scratch)))
        os.replace(temporary_zip, target)
    return [target]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("platform", choices=("macos", "windows"))
    parser.add_argument("--binary", type=Path, required=True,
                        help="completed CMake noggin2 or noggin2.exe binary")
    parser.add_argument("--assets", type=Path, default=ROOT / "assets")
    parser.add_argument("--output", type=Path, default=ROOT / "dist")
    parser.add_argument("--sdl2-runtime", type=Path,
                        help="SDL2 dylib (macOS) or SDL2.dll (Windows)")
    parser.add_argument("--sdl2-license", type=Path,
                        help="override for SDL2 LICENSE.txt")
    args = parser.parse_args()
    binary = checked_file(args.binary, "native executable")
    assets = checked_assets(args.assets)
    output = args.output.expanduser().resolve()
    output.mkdir(parents=True, exist_ok=True)
    if args.platform == "macos":
        artifacts = package_macos(binary, assets, output,
                                  args.sdl2_runtime, args.sdl2_license)
    else:
        artifacts = package_windows(binary, assets, output,
                                    args.sdl2_runtime, args.sdl2_license)
    for artifact in artifacts:
        print(artifact)


if __name__ == "__main__":
    main()
