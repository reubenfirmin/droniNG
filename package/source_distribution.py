#!/usr/bin/env python3
"""Include verified Qt SDK source materials in a staged Linux package."""

import argparse
import json
from pathlib import Path, PurePosixPath
import sys
import tarfile

from qt_source_materials import digest, prepare as prepare_qt
from sdk_dependency_materials import prepare as prepare_dependencies
from application_source_materials import prepare as prepare_application, verify_materials


def checked_file(application, name):
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts or path == PurePosixPath("."):
        raise RuntimeError(f"Unsafe source distribution path: {name}")
    result = (application / path).resolve(strict=True)
    if not result.is_relative_to(application) or not result.is_file():
        raise RuntimeError(f"Source distribution file escapes the package: {name}")
    return result


def verify_sources(application):
    application = application.resolve(strict=True)
    manifest = json.loads((application / "qt-runtime.json").read_text())
    report = manifest.get("source_materials")
    if not report:
        raise RuntimeError("Package has no included source-material manifest")
    if report["qt_version"] != manifest["qt_version"]:
        raise RuntimeError("Source materials do not match the package's Qt version")
    hashes = report["files_sha256"]
    actual = {str(path.relative_to(application))
              for path in (application / "source-materials").rglob("*") if path.is_file()}
    actual.add("THIRD_PARTY_NOTICES.md")
    if hashes.keys() != actual:
        raise RuntimeError("Source material file set differs from the package manifest")
    for name, wanted in hashes.items():
        if digest(checked_file(application, name)) != wanted:
            raise RuntimeError(f"Source material checksum differs: {name}")
    qt = json.loads((application / "source-materials/qt/qt-source-materials.json").read_text())
    if (sorted(qt["modules"]) != report["qt_modules"]
            or qt.get("build_tools", []) != report.get("qt_build_tools", [])):
        raise RuntimeError("Qt module/build-tool inventory differs from package manifest")
    dependencies = json.loads((application / "source-materials/sdk-dependencies/"
                                "sdk-dependency-materials.json").read_text())
    runtime = application / "lib/droning"
    qt_paths = set()
    for path in runtime.rglob("*"):
        if not path.is_file():
            continue
        relative = path.relative_to(runtime)
        if (relative.parts[0] in ("qtplugins", "qml", "translations")
                or (len(relative.parts) == 1 and path.name.startswith("libQt6"))):
            qt_paths.add(str(path.relative_to(application)))
    if (qt_paths != {record["path"] for record in qt["deployed_files"]}
            or len(qt["deployed_files"]) != report["deployed_qt_file_count"]):
        raise RuntimeError("Deployed Qt file set differs from its source materials")
    catalog = json.loads((application / "source-materials/sdk-dependencies/"
                           "sdk-dependency-sources.json").read_text())
    dependency_paths = {f"lib/droning/{name}" for component in catalog["components"].values()
                        for name in component["sdk_library_sha256"]}
    if (dependency_paths != {record["path"] for record in dependencies["deployed_libraries"]}
            or len(dependencies["deployed_libraries"]) != report["deployed_sdk_dependency_library_count"]):
        raise RuntimeError("SDK dependency file set differs from its source catalogue")
    for record in qt["deployed_files"] + dependencies["deployed_libraries"]:
        if digest(checked_file(application, record["path"])) != record["sha256"]:
            raise RuntimeError(f"Deployed file differs from its source materials: {record['path']}")
    if report.get("application_sources"):
        application_sources = verify_materials(application / "source-materials/application", application)
        if application_sources != report["application_sources"]:
            raise RuntimeError("Application source inventory differs from package manifest")
    print(f"Verified {len(hashes)} included source/notice/inventory files and "
          f"{len(qt['deployed_files']) + len(dependencies['deployed_libraries'])} deployed SDK files.")
    return report


def include_sources(application, sdk, qt_archives, dependency_cache,
                    qt_catalog=None, dependency_catalog=None, application_source_root=None):
    application = application.resolve(strict=True)
    manifest_path = application / "qt-runtime.json"
    manifest = json.loads(manifest_path.read_text())
    destination = application / "source-materials"
    overview = application / "THIRD_PARTY_NOTICES.md"
    if destination.exists() or overview.exists():
        raise RuntimeError("Use a fresh package stage to preserve existing source materials")
    catalogs = Path(__file__).resolve().parent
    if qt_catalog is None:
        qt_catalog = json.loads((catalogs / "qt_runtime_sources.json").read_text())
    if dependency_catalog is None:
        dependency_catalog = json.loads((catalogs / "sdk_dependency_sources.json").read_text())
    if any(manifest["qt_version"] != catalog["qt_version"]
           for catalog in (qt_catalog, dependency_catalog)):
        raise RuntimeError("Both source catalogues must match the package's Qt version")

    # Each preparer checks pinned archives and deployed SDK bytes before writing
    # its output. A failed stage never replaces the previous final package.
    qt = prepare_qt(application, sdk, qt_archives, destination / "qt", qt_catalog)
    dependencies = prepare_dependencies(application, sdk, dependency_cache,
                                        destination / "sdk-dependencies", dependency_catalog)
    application_sources = None
    if application_source_root is not None:
        application_sources = prepare_application(application, application_source_root,
                                                  destination / "application")
    remaining = list(dependencies["remaining_source_gates"])
    remaining.append("Qt build inputs: complete remaining tool/dependency and build-recipe verification.")
    if application_sources:
        remaining.extend(application_sources["remaining_source_gates"])
    index = {"qt_version": manifest["qt_version"],
             "source_distribution_complete": False,
             "qt_modules": sorted(qt["modules"]),
             "qt_build_tools": qt["build_tools"],
             "sdk_dependencies": dependencies["components"],
             "deployed_qt_file_count": qt["deployed_file_count"],
             "deployed_sdk_dependency_library_count": len(dependencies["deployed_libraries"]),
             "remaining_source_gates": remaining}
    if application_sources:
        index["application_sources"] = application_sources
    (destination / "index.json").write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8")
    overview.write_text(
        "# Third-party sources and notices\n\n"
        f"This package uses Qt {manifest['qt_version']}. The included source archives, "
        "original notices and inventories are under [source-materials](source-materials).\n\n"
        "- Qt module licence texts: [source-materials/qt/licenses](source-materials/qt/licenses).\n"
        "- Qt source archives: [source-materials/qt/archives](source-materials/qt/archives).\n"
        "- SDK dependency notices: [source-materials/sdk-dependencies/notices]"
        "(source-materials/sdk-dependencies/notices).\n"
        "- SDK dependency sources and selected Qt provisioning files: "
        "[source-materials/sdk-dependencies/materials](source-materials/sdk-dependencies/materials).\n\n"
        + ("- Current application sources and retained original notices: "
           "[source-materials/application/droning-source.tar.xz]"
           "(source-materials/application/droning-source.tar.xz). "
           "The adjacent inventory records exact files and runtime hashes; "
           "association with runtime hashes does not prove a rebuild.\n"
           "- Pinned GLC, Breakpad and linux-syscall-support sources and notices: "
           "[source-materials/application/dependencies]"
           "(source-materials/application/dependencies).\n\n"
           if application_sources else "") +
        "Individual Qt modules and their third-party components have different terms; "
        "retain their original notices and inventory licence expressions. "
        "QtCharts includes the GPL-3.0-only alternative.\n\n"
        "The source audit is incomplete. The verified SDK materials do not yet cover "
        "every application library, asset or build input. "
        "[source-materials/index.json](source-materials/index.json) records the remaining gates.\n",
        encoding="utf-8")
    hashes = {str(path.relative_to(application)): digest(path)
              for path in sorted(destination.rglob("*")) if path.is_file()}
    hashes[overview.name] = digest(overview)
    result = dict(index, files_sha256=dict(sorted(hashes.items())))
    manifest["source_materials"] = result
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Included {len(hashes)} verified source/notice/inventory files in the package. "
          "The complete application source audit remains open.")
    return result


def main():
    parser = argparse.ArgumentParser(description="Verify a packaged SDK source distribution")
    parser.add_argument("--application", type=Path, required=True)
    args = parser.parse_args()
    try:
        verify_sources(args.application)
    except (OSError, RuntimeError, ValueError, KeyError, tarfile.TarError) as error:
        print(f"Source distribution verification failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
