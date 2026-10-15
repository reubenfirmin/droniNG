#!/usr/bin/env python3
"""Bundle the selected Linux Qt runtime into a built GCS tree.

Copies only into the supplied application directory. Uses ldd to follow the
actual ELF dependency closure and rejects mixed Qt majors or missing libraries.
System graphics, libc, audio and TLS dependencies stay owned by the host distro.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile

from linux_abi import collect_abi, dependency_paths
from source_distribution import include_sources


PLUGIN_GROUPS = (
    "imageformats", "platforms", "xcbglintegrations", "egldeviceintegrations",
    "wayland-decoration-client", "wayland-graphics-integration-client",
    "wayland-shell-integration", "multimedia", "mediaservice", "audio",
    "tls", "networkinformation",
)
QML_MODULES = (
    "QML", "Qt", "QtQml", "QtQml.2", "QtQuick", "QtQuick.2",
    "QtCharts", "QtMultimedia", "Qt5Compat",
)


def query(qmake, key):
    return subprocess.check_output([qmake, "-query", key], text=True).strip()


def is_elf(path):
    if not path.is_file():
        return False
    with path.open("rb") as stream:
        return stream.read(4) == b"\x7fELF"


def deploy(qmake, application, qt_source_archives=None, sdk_dependency_cache=None,
           application_source_root=None):
    if (qt_source_archives is None) != (sdk_dependency_cache is None):
        raise RuntimeError("Supply both Qt source archives and the SDK dependency cache")
    if application_source_root is not None and qt_source_archives is None:
        raise RuntimeError("Application source inclusion requires the SDK source material options")
    if ((application / "source-materials").exists()
            or (application / "THIRD_PARTY_NOTICES.md").exists()):
        raise RuntimeError("Use a fresh package stage to preserve existing source materials")
    version = query(qmake, "QT_VERSION")
    major = version.split(".")[0]
    if major != "6":
        raise RuntimeError(f"Qt 6 is required for deployment: {version}")
    library_source = Path(query(qmake, "QT_INSTALL_LIBS")).resolve()
    plugin_source = Path(query(qmake, "QT_INSTALL_PLUGINS"))
    qml_source = Path(query(qmake, "QT_INSTALL_QML"))
    translation_source = Path(query(qmake, "QT_INSTALL_TRANSLATIONS"))
    prefix = Path(query(qmake, "QT_INSTALL_PREFIX")).resolve()
    library_destination = application / "lib/droning"
    executable = application / "bin/droning-gcs.bin"
    manifest_path = application / "qt-runtime.json"
    if not executable.is_file() or not library_destination.is_dir():
        raise RuntimeError(f"Build the GCS first: {application}")
    previous = {}
    if manifest_path.exists():
        previous = json.loads(manifest_path.read_text())
        if previous["qt_version"] != version:
            raise RuntimeError("Use a separate build tree for a different Qt SDK")

    # A distro Qt prefix shares /usr/lib with unrelated system libraries. Only
    # a private SDK's additional runtime libraries (e.g. ICU/FFmpeg) are bundled.
    private_sdk = prefix not in (Path("/usr"), Path("/usr/local"))
    environment = os.environ.copy()
    environment["LD_LIBRARY_PATH"] = os.pathsep.join(map(str, (
        library_destination, library_destination / "plugins/droniNG", library_source,
    )))

    def dependencies(path):
        result = subprocess.run(["ldd", str(path)], env=environment, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if result.returncode:
            raise RuntimeError(f"ldd failed for {path}: {result.stderr.strip()}")
        found = []
        for name, filename in dependency_paths(result.stdout + result.stderr).items():
            qt = re.match(r"libQt([56])", name)
            if qt and qt.group(1) != major:
                raise RuntimeError(f"Qt {major} package would load {name} from {path}")
            found.append((name, filename))
        return found

    # Check the application before making runtime copies: a wrong QMAKE must
    # fail rather than turn a Qt 5 tree into a partially mixed Qt 6 deployment.
    app_dependencies = dependencies(executable)
    if not any(name.startswith(f"libQt{major}Core.") for name, _ in app_dependencies):
        raise RuntimeError(f"Application does not use the selected Qt {version}")

    manifest_path.unlink(missing_ok=True)
    for name in previous.get("bundled_libraries_sha256", {}):
        if Path(name).name != name:
            raise RuntimeError("Invalid library name in previous deployment manifest")
        (library_destination / name).unlink(missing_ok=True)
    plugins = library_destination / "qtplugins"
    if plugins.exists():
        shutil.rmtree(plugins)
    plugins.mkdir(exist_ok=True)
    for group in PLUGIN_GROUPS:
        source = plugin_source / group
        if source.is_dir():
            shutil.copytree(source, plugins / group, dirs_exist_ok=True)
    sqlite = plugin_source / "sqldrivers/libqsqlite.so"
    if not sqlite.is_file():
        raise RuntimeError(f"SQLite plugin is missing from {plugin_source}")
    (plugins / "sqldrivers").mkdir(exist_ok=True)
    shutil.copy2(sqlite, plugins / "sqldrivers/libqsqlite.so")
    # Deploy the GCS module families and their child modules. Optional SDK
    # families such as QtQuick3D, and distro KDE modules, are outside this build.
    if not qml_source.is_dir():
        raise RuntimeError(f"QML modules are missing from {qml_source}")
    qml_destination = library_destination / "qml"
    if qml_destination.exists():
        shutil.rmtree(qml_destination)
    qml_destination.mkdir()
    for module in qml_source.iterdir():
        if module.is_dir() and module.name in QML_MODULES:
            shutil.copytree(module, qml_destination / module.name,
                            ignore=shutil.ignore_patterns("*.o", "*.a", "*.cmake", "*.prl", "objects-*", "CMakeFiles", "test"))
    if translation_source.is_dir():
        shutil.copytree(translation_source, library_destination / "translations", dirs_exist_ok=True)

    pending = [path for root in (application / "bin", library_destination)
               for path in root.rglob("*") if not path.name.endswith(".debug") and is_elf(path)]
    visited = set()
    bundled = {}
    external = set()
    while pending:
        path = pending.pop()
        if path.resolve() in visited:
            continue
        visited.add(path.resolve())
        for name, dependency in dependencies(path):
            resolved = dependency.resolve()
            is_qt = name.startswith(f"libQt{major}")
            is_sdk_runtime = private_sdk and (
                resolved.is_relative_to(library_source)
                or (resolved.is_relative_to(library_destination)
                    and (library_source / name).is_file()))
            if is_qt or is_sdk_runtime:
                if not (resolved.is_relative_to(library_source)
                        or resolved.is_relative_to(library_destination)):
                    raise RuntimeError(f"Qt dependency comes from another SDK: {dependency}")
                if Path(name).name != name:
                    raise RuntimeError(f"Runtime dependency is not a relocatable library name: {name}")
                # ldd may resolve a symlink to its fully versioned filename.
                # The deployed filename must satisfy the requested SONAME.
                destination = library_destination / name
                if dependency != destination:
                    if destination.exists() and not destination.samefile(dependency):
                        destination.unlink()
                    shutil.copy2(dependency, destination)
                bundled[destination.name] = hashlib.sha256(destination.read_bytes()).hexdigest()
                pending.append(destination)
            elif not resolved.is_relative_to(application):
                external.add(name)

    (application / "bin/qt.conf").write_text(
        "[Paths]\nPlugins=../lib/droning/qtplugins\n"
        "QmlImports=../lib/droning/qml\nQml2Imports=../lib/droning/qml\n"
        "Translations=../lib/droning/translations\n", encoding="utf-8")
    manifest = {"qt_version": version, "qmake": str(Path(qmake).resolve()),
                "bundled_libraries_sha256": dict(sorted(bundled.items())),
                "system_dependencies": sorted(external),
                "linux_abi": collect_abi(application, major)}
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    if qt_source_archives is not None:
        include_sources(application, prefix, qt_source_archives, sdk_dependency_cache,
                        application_source_root=application_source_root)
    print(f"Bundled Qt {version}: {len(bundled)} runtime libraries; manifest {manifest_path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qmake", required=True)
    parser.add_argument("--application", required=True, type=Path)
    parser.add_argument("--qt-source-archives", type=Path,
                        help="Include verified Qt source archives, notices and SDK inventories")
    parser.add_argument("--sdk-dependency-cache", type=Path,
                        help="Include the pinned FFmpeg/ICU materials with Qt sources")
    parser.add_argument("--application-source-root", type=Path,
                        help="Preserve current tracked sources and new application source files")
    args = parser.parse_args()
    try:
        deploy(args.qmake, args.application.resolve(),
               args.qt_source_archives, args.sdk_dependency_cache, args.application_source_root)
    except (OSError, RuntimeError, subprocess.SubprocessError, ValueError,
            KeyError, tarfile.TarError) as error:
        print(f"Qt deployment failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
