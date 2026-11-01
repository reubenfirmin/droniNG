# Linux builder

Use Docker or rootless Podman on an x86-64 Linux host. The Ubuntu 24.04 image
contains Qt 6.12.0 and the build dependencies, so the host can be Ubuntu, Fedora
or Red Hat Enterprise Linux. The same image runs the graphical application.

```bash
bash package/linux-container.sh build
bash package/linux-container.sh run
```

The first command builds the image, prepares the existing toolchain targets,
then calls `make gcs` and `make package_ground_compress`. It produces:

```text
build/linux/package-linux_x86_64/droning_linux_x86_64.tar.xz
build/linux/package-linux_x86_64/SHA256SUMS
```

The archive includes the ground station, Qt runtime and the firmware installation
bundle selected by `make/firmware-installer.mk`. Existing packaging code records
the version and library requirements. This builder adds no test orchestration or
release publishing steps.

`run` connects to the current Wayland or X11 desktop and uses software OpenGL.
Settings persist in `build/linux-user`. It also accepts an extracted package:

```bash
bash package/linux-container.sh run /path/to/droning_linux_x86_64
```

The launcher exposes the display socket; controller USB/serial devices are not
passed through. Native execution of the extracted archive depends on the host's
library versions. Use the container on older systems such as RHEL 9.

Podman is selected when installed; set `CONTAINER_ENGINE=docker` to use Docker.
Set `JOBS=8` to change build parallelism. Build output, downloaded tools and
dependency downloads stay in the checkout for subsequent builds.

## GitHub artifacts

Once this workflow is on the default branch, open **Actions**, select **Linux
build**, and select **Run workflow**. Pushing a `v*` tag also starts a build.
Download the `droning-linux-x86_64` artifact, then verify and extract it:

```bash
sha256sum -c SHA256SUMS
tar -xf droning_linux_x86_64.tar.xz
```

No extra repository secrets are needed. The workflow uploads build artifacts;
it does not create or publish GitHub Releases.
