#!/usr/bin/env bash
# One build environment and desktop launcher for Docker and rootless Podman.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd -P)"
engine="${CONTAINER_ENGINE:-}"
if [[ -z "$engine" ]]; then
    if command -v podman >/dev/null; then engine=podman; else engine=docker; fi
fi
command -v "$engine" >/dev/null || { echo 'Install Podman or Docker first.' >&2; exit 1; }
image=localhost/droning-linux:qt6.12.0
runtime=(--rm --user "$(id -u):$(id -g)" --security-opt label=disable)
if [[ "$engine" == *podman ]]; then runtime+=(--userns keep-id); fi
mode="${1:-build}"
if [[ $# -gt 0 ]]; then shift; fi

case "$mode" in
    build)
        "$engine" build --tag "$image" --file "$root/package/linux.Dockerfile" "$root/package"
        exec "$engine" run "${runtime[@]}" --volume "$root:/work" \
            --env "JOBS=${JOBS:-4}" "$image"
        ;;
    run)
        application="$root/build/linux/package-linux_x86_64/droning_linux_x86_64"
        if [[ $# -gt 0 && "$1" != -* ]]; then application="$(realpath "$1")"; shift; fi
        [[ -x "$application/droning-gcs" ]] || {
            echo 'Package not found. Run: bash package/linux-container.sh build' >&2; exit 1;
        }
        state="$root/build/linux-user"
        mkdir -p "$state/runtime"
        chmod 700 "$state/runtime"
        gui=(--volume "$application:/app:ro" --volume "$state:/state"
             --env XDG_CONFIG_HOME=/state/config --env XDG_DATA_HOME=/state/data
             --env XDG_CACHE_HOME=/state/cache --env XDG_RUNTIME_DIR=/state/runtime
             --env LIBGL_ALWAYS_SOFTWARE=1 --env QSG_RHI_BACKEND=opengl)
        socket="${WAYLAND_DISPLAY:-}"
        if [[ "$socket" != /* ]]; then socket="${XDG_RUNTIME_DIR:-}/$socket"; fi
        if [[ -n "${WAYLAND_DISPLAY:-}" && -S "$socket" ]]; then
            gui+=(--volume "$socket:/state/runtime/wayland-0"
                  --env WAYLAND_DISPLAY=wayland-0 --env QT_QPA_PLATFORM=wayland)
        elif [[ -n "${DISPLAY:-}" ]]; then
            gui+=(--volume /tmp/.X11-unix:/tmp/.X11-unix:ro --env "DISPLAY=$DISPLAY"
                  --env QT_QPA_PLATFORM=xcb --env QT_X11_NO_MITSHM=1)
            authority="${XAUTHORITY:-$HOME/.Xauthority}"
            if [[ -f "$authority" ]]; then
                gui+=(--volume "$authority:/tmp/droning.xauth:ro" --env XAUTHORITY=/tmp/droning.xauth)
            fi
        else
            echo 'Run from a graphical Linux session (Wayland or X11).' >&2; exit 1
        fi
        exec "$engine" run "${runtime[@]}" "${gui[@]}" "$image" /app/droning-gcs "$@"
        ;;
    *) echo 'Usage: bash package/linux-container.sh build | run [application-directory] [application-options]' >&2; exit 2 ;;
esac
