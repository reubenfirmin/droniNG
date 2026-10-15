#!/usr/bin/env python3
"""Exercise Linux ABI auditing with compiled libraries and real ELF failures."""

import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "package"))
from linux_abi import collect_abi, dependency_paths
from deploy_linux import deploy


@unittest.skipUnless(shutil.which("gcc") and shutil.which("readelf") and shutil.which("ldd"),
                     "Requires gcc, readelf and ldd")
class LinuxRuntimeAbiTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="linux-abi-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.package = self.directory / "package with spaces"
        (self.package / "bin").mkdir(parents=True)
        (self.package / "lib/droning").mkdir(parents=True)
        self.library = self.package / "lib/droning/libfixture.so.1"
        self.compile_library(self.library, "libfixture.so.1", "FIXTURE_1.0")
        self.source = self.directory / "main.c"
        self.source.write_text("extern int fixture_api(void);\n"
                               "int main(void) { return fixture_api(); }\n")
        self.executable = self.package / "bin/droning-gcs.bin"
        self.compile_application(self.library)

    def compiler(self, *arguments):
        result = subprocess.run(["gcc", *map(str, arguments)], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout)

    def compile_library(self, output, soname, version):
        source = self.directory / "library.c"
        source.write_text("int fixture_api(void) { return 0; }\n")
        script = self.directory / "symbols.map"
        script.write_text(f"{version} {{ global: fixture_api; local: *; }};\n")
        self.compiler("-shared", "-fPIC", source, f"-Wl,-soname,{soname}",
                      f"-Wl,--version-script={script}", "-o", output)

    def compile_application(self, library):
        self.compiler(self.source, library, "-Wl,-rpath,$ORIGIN/../lib/droning",
                      "-o", self.executable)

    def test_actual_version_requirements_and_hashes(self):
        report = collect_abi(self.package)
        self.assertEqual(report["elf_file_count"], 2)
        files = {entry["path"]: entry for entry in report["elf_files"]}
        requirements = files["bin/droning-gcs.bin"]["required_symbol_versions"]
        self.assertEqual(requirements["libfixture.so.1"], ["FIXTURE_1.0"])
        provider = report["version_providers"]["libfixture.so.1"]
        self.assertTrue(provider["bundled"])
        self.assertEqual(provider["path"], "lib/droning/libfixture.so.1")
        self.assertEqual(provider["sha256"], files[provider["path"]]["sha256"])
        self.assertIn("GLIBC", report["highest_symbol_versions"])
        # The report must remain valid JSON for the deployment manifest.
        self.assertEqual(json.loads(json.dumps(report)), report)

    def test_missing_library_is_rejected(self):
        self.library.unlink()
        with self.assertRaisesRegex(RuntimeError, "not found"):
            collect_abi(self.package)

    def test_existing_library_with_missing_symbol_version_is_rejected(self):
        self.compile_library(self.library, "libfixture.so.1", "FIXTURE_2.0")
        with self.assertRaisesRegex(RuntimeError, "FIXTURE_1.0"):
            collect_abi(self.package)

    def test_foreign_architecture_is_rejected_before_loading(self):
        data = bytearray(self.library.read_bytes())
        identity = collect_abi(self.package)["elf_identity"]
        foreign = 183 if identity["machine"] != 183 else 62
        data[18:20] = foreign.to_bytes(2, identity["byte_order"])
        self.library.write_bytes(data)
        with self.assertRaisesRegex(RuntimeError, "ELF architecture differs"):
            collect_abi(self.package)

    def test_runtime_symlink_outside_package_is_rejected(self):
        external = self.directory / "external.so"
        self.library.rename(external)
        self.library.symlink_to(external)
        with self.assertRaisesRegex(RuntimeError, "outside package"):
            collect_abi(self.package)

    def test_host_qt_cannot_fill_a_package_gap(self):
        external = self.directory / "libQt6Fixture.so.6"
        self.compile_library(external, external.name, "Qt_6")
        self.compiler(self.source, external, f"-Wl,-rpath,{self.directory}",
                      "-o", self.executable)
        with self.assertRaisesRegex(RuntimeError, "Qt provider is outside package"):
            collect_abi(self.package, "6")

    def test_mixed_qt_major_is_rejected(self):
        wrong = self.package / "lib/droning/libQt5Fixture.so.5"
        self.compile_library(wrong, wrong.name, "Qt_5")
        self.compile_application(wrong)
        with self.assertRaisesRegex(RuntimeError, "Mixed Qt major"):
            collect_abi(self.package, "6")

    def test_dynamic_loader_path_is_recognized(self):
        # The loader itself is emitted without ldd's '=>' separator.
        paths = dependency_paths(" /lib64/ld-linux-x86-64.so.2 (0x0000000000010000)\n")
        self.assertEqual(paths["ld-linux-x86-64.so.2"],
                         Path("/lib64/ld-linux-x86-64.so.2").resolve())

    def test_deployment_preserves_requested_soname_from_versioned_sdk_library(self):
        sdk = self.directory / "sdk with spaces"
        libraries = sdk / "lib"
        plugins = sdk / "plugins"
        qml = sdk / "qml"
        for directory in (libraries, plugins / "sqldrivers", qml / "QtQml"):
            directory.mkdir(parents=True)
        source = libraries / "libQt6Core.so.6.12.0"
        self.compile_library(source, "libQt6Core.so.6", "Qt_6")
        (libraries / "libQt6Core.so.6").symlink_to(source.name)
        self.compile_library(plugins / "sqldrivers/libqsqlite.so", "libqsqlite.so",
                             "SQLITE_FIXTURE_1.0")
        self.compiler(self.source, source, f"-Wl,-rpath,{libraries}",
                      "-o", self.executable)
        self.library.unlink()
        queries = {"QT_VERSION": "6.12.0", "QT_INSTALL_PREFIX": sdk,
                   "QT_INSTALL_LIBS": libraries, "QT_INSTALL_PLUGINS": plugins,
                   "QT_INSTALL_QML": qml, "QT_INSTALL_TRANSLATIONS": sdk / "translations"}
        with mock.patch("deploy_linux.query", side_effect=lambda _, key: str(queries[key])):
            deploy(str(sdk / "bin/qmake"), self.package)
        deployed = self.package / "lib/droning/libQt6Core.so.6"
        self.assertEqual(deployed.read_bytes(), source.read_bytes())
        manifest = json.loads((self.package / "qt-runtime.json").read_text())
        self.assertIn(deployed.name, manifest["bundled_libraries_sha256"])
        provider = manifest["linux_abi"]["version_providers"][deployed.name]
        self.assertTrue(provider["bundled"])
        self.assertEqual(provider["path"], "lib/droning/libQt6Core.so.6")


if __name__ == "__main__":
    unittest.main()
