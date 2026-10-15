#!/usr/bin/env python3
"""Bundle non-system Mach-O dependencies that Qt deployment leaves external."""

from __future__ import annotations

import os
import pathlib
import re
import shutil
import stat
import subprocess
import sys
from collections import deque


def run(*args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        args, check=check, text=True, capture_output=True
    )


def is_macho(path: pathlib.Path) -> bool:
    if path.is_symlink() or not path.is_file():
        return False
    return "Mach-O" in run("file", "-b", str(path)).stdout


def dependencies(path: pathlib.Path) -> list[str]:
    lines = run("otool", "-L", str(path)).stdout.splitlines()[1:]
    return [line.strip().split(" ", 1)[0] for line in lines if line.strip()]


def writable(path: pathlib.Path) -> None:
    path.chmod(path.stat().st_mode | stat.S_IWUSR)


def framework_name(value: str) -> str | None:
    match = re.search(r"([^/]+\.framework)(?:/|$)", value)
    return match.group(1) if match else None


def bundled_reference(
    path: pathlib.Path, frameworks: pathlib.Path
) -> str | None:
    try:
        relative = path.relative_to(frameworks)
    except ValueError:
        return None
    return f"@rpath/{relative}"


def locate_dependency(
    dependency: str, search_roots: list[pathlib.Path]
) -> pathlib.Path | None:
    relative = dependency.removeprefix("@rpath/")
    for root in search_roots:
        exact = root / relative
        if exact.exists():
            return exact.resolve()
        basename = root / pathlib.Path(relative).name
        if basename.exists():
            return basename.resolve()
    return None


def copy_dependency(
    source: pathlib.Path, dependency: str, frameworks: pathlib.Path
) -> tuple[pathlib.Path, bool]:
    framework = framework_name(dependency) or framework_name(str(source))
    if framework:
        source_text = str(source)
        source_root = pathlib.Path(
            source_text[: source_text.index(framework) + len(framework)]
        )
        destination = frameworks / framework
        created = not destination.exists()
        if created:
            shutil.copytree(source_root, destination, symlinks=True)
        relative = (
            dependency.removeprefix("@rpath/")
            if dependency.startswith("@rpath/")
            else dependency[dependency.index(framework):]
        )
        binary = frameworks / relative
        if binary.exists():
            writable(binary)
            run("install_name_tool", "-id", f"@rpath/{relative}", str(binary),
                check=False)
        return binary, created

    destination = frameworks / pathlib.Path(
        dependency.removeprefix("@rpath/")
    ).name
    created = not destination.exists()
    if created:
        shutil.copy2(source, destination)
    writable(destination)
    run("install_name_tool", "-id", f"@rpath/{destination.name}",
        str(destination), check=False)
    return destination, created


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: bundle_dependencies.py <LanAudio.app> <Qt lib dir>",
              file=sys.stderr)
        return 2

    app = pathlib.Path(sys.argv[1]).resolve()
    qt_lib = pathlib.Path(sys.argv[2]).resolve()
    frameworks = app / "Contents" / "Frameworks"
    frameworks.mkdir(parents=True, exist_ok=True)
    search_roots = [
        qt_lib,
        pathlib.Path("/opt/homebrew/lib"),
        pathlib.Path("/usr/local/lib"),
    ]
    openssl_root = os.environ.get("OPENSSL_ROOT_DIR", "")
    openssl_prefix = (
        pathlib.Path(openssl_root).resolve() if openssl_root else None
    )
    if openssl_prefix is not None:
        search_roots.append(openssl_prefix / "lib")

    queue = deque(path for path in app.rglob("*") if is_macho(path))
    processed: set[pathlib.Path] = set()
    while queue:
        binary = queue.popleft()
        if binary in processed:
            continue
        processed.add(binary)
        writable(binary)
        identity = bundled_reference(binary, frameworks)
        if identity is not None:
            run("install_name_tool", "-id", identity, str(binary), check=False)
        if binary.parent.name == "MacOS":
            # The CLI is copied next to the GUI and is not processed by
            # macdeployqt, so it needs the same Frameworks rpath as LanAudio.
            run(
                "install_name_tool",
                "-add_rpath",
                "@executable_path/../Frameworks",
                str(binary),
                check=False,
            )
        for dependency in dependencies(binary):
            if dependency.startswith(("/System/", "/usr/lib/")):
                continue
            if dependency.startswith(("@loader_path/", "@executable_path/")):
                continue
            if str(app) in dependency:
                replacement = bundled_reference(
                    pathlib.Path(dependency), frameworks
                )
                if replacement is not None and replacement != dependency:
                    run(
                        "install_name_tool",
                        "-change",
                        dependency,
                        replacement,
                        str(binary),
                        check=False,
                    )
                continue

            source: pathlib.Path | None = None
            replacement = dependency
            if dependency.startswith("@rpath/"):
                bundled = frameworks / dependency.removeprefix("@rpath/")
                if bundled.exists():
                    continue
                source = locate_dependency(dependency, search_roots)
            elif dependency.startswith(("/opt/homebrew/", "/usr/local/")) or (
                openssl_prefix is not None
                and dependency.startswith(str(openssl_prefix))
            ):
                source = pathlib.Path(dependency).resolve()
                framework = framework_name(dependency)
                replacement = (
                    f"@rpath/{dependency[dependency.index(framework):]}"
                    if framework
                    else f"@rpath/{pathlib.Path(dependency).name}"
                )
            elif dependency.startswith("/"):
                raise RuntimeError(
                    f"unsupported external dependency in {binary}: "
                    f"{dependency}"
                )

            if source is None or not source.exists():
                raise RuntimeError(
                    f"cannot resolve dependency in {binary}: {dependency}"
                )
            destination, created = copy_dependency(
                source, dependency, frameworks
            )
            if replacement != dependency:
                run("install_name_tool", "-change", dependency, replacement,
                    str(binary), check=False)
            if created:
                root = (
                    frameworks / framework_name(dependency)
                    if framework_name(dependency)
                    else destination
                )
                candidates = root.rglob("*") if root.is_dir() else [root]
                queue.extend(path for path in candidates if is_macho(path))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"dependency bundling failed: {error}", file=sys.stderr)
        raise SystemExit(1)
