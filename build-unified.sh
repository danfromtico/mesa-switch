#!/usr/bin/env bash
# Copyright © 2026 Mesa Switch contributors
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/builddir-unified}"
HOST_BUILD_DIR="${HOST_BUILD_DIR:-${ROOT_DIR}/builddir-native-x64-host}"
DESTDIR="${DESTDIR:-${ROOT_DIR}/mesa-unified-install}"
DIST_DIR="${DIST_DIR:-${ROOT_DIR}/dist}"
MESON_BIN="${MESON:-meson}"
NINJA_BIN="${NINJA:-ninja}"
PYTHON_BIN="${PYTHON:-python}"
BUILD_TYPE="${MESA_BUILD_TYPE:-release}"
OPTIMIZATION="${MESA_OPTIMIZATION:-2}"
ALLOW_DIRTY="${ALLOW_DIRTY:-0}"
SDK_BASENAME="${SDK_BASENAME:-mesa-26.2.4-switch-unified-horizon-sdk}"
export MESA_SWITCH_RUST_TARGET="${MESA_SWITCH_RUST_TARGET:-aarch64-unknown-linux-gnu}"

if [[ -n "${MSYSTEM:-}" ]]; then
    default_cross_file="${ROOT_DIR}/switch_cross_file_msys2.txt"
    default_native_file="${ROOT_DIR}/switch_native_tools_msys2.txt"
    default_host_cross_file="${ROOT_DIR}/windows_x64_host_cross_file_msys2.txt"
else
    echo "This unified NVK build currently requires the checked MSYS2 toolchain." >&2
    echo "Run it through C:/msys64/usr/bin/bash.exe or an MSYS2 shell." >&2
    exit 2
fi

CROSS_FILE="${CROSS_FILE:-${default_cross_file}}"
NATIVE_FILE="${NATIVE_FILE:-${default_native_file}}"
HOST_CROSS_FILE="${HOST_CROSS_FILE:-${default_host_cross_file}}"
SDK_DIR="${DESTDIR}/opt/devkitpro/portlibs/switch"
PACKAGE_PATH="${DIST_DIR}/${SDK_BASENAME}.zip"

command -v "${MESON_BIN}" >/dev/null
command -v "${NINJA_BIN}" >/dev/null
command -v "${PYTHON_BIN}" >/dev/null
command -v sed >/dev/null
command -v git >/dev/null

if [[ -n "${MESA_SWITCH_RUSTC:-}" ]]; then
    if [[ ! -x "${MESA_SWITCH_RUSTC}" ]]; then
        echo "MESA_SWITCH_RUSTC is not executable: ${MESA_SWITCH_RUSTC}" >&2
        exit 2
    fi
elif [[ ! -x "${ROOT_DIR}/build/deps/cargo/bin/rustc.exe" &&
        ! -x /root/.cargo/bin/rustc ]]; then
    echo "A Rust toolchain with the aarch64-unknown-linux-gnu target is required." >&2
    echo "Set MESA_SWITCH_RUSTC (and RUSTUP_HOME/CARGO_HOME when using a rustup proxy)," >&2
    echo "or provide the cached toolchain at build/deps/{cargo,rustup}." >&2
    exit 2
fi

cd "${ROOT_DIR}"
if [[ "${ALLOW_DIRTY}" != "1" ]] && [[ -n "$(git status --porcelain)" ]]; then
    echo "Refusing to create a release SDK from a dirty checkout." >&2
    echo "Commit the complete Horizon implementation, or set ALLOW_DIRTY=1 for a development snapshot." >&2
    exit 2
fi

export NINJA="${NINJA_BIN}"
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git show -s --format=%ct HEAD)}"
export ZERO_AR_DATE=1
export LC_ALL=C
export TZ=UTC
# The Meson executable is a native Windows program.  MSYS may translate the
# Horizon install prefix into C:/msys64/opt/..., which is not an absolute path
# in the target's POSIX namespace.  Preserve just the --prefix argument while
# continuing to translate source and build directory arguments.
export MSYS2_ARG_CONV_EXCL="${MSYS2_ARG_CONV_EXCL:+${MSYS2_ARG_CONV_EXCL};}--prefix="

# Keep native Windows compiler and Rust temporary files in the writable source
# workspace.  Sandboxed or portable MSYS installations may expose /tmp while
# denying writes to the corresponding C:/msys64/tmp directory.
MESA_TMPDIR="${MESA_TMPDIR:-${ROOT_DIR}/build/unified-tmp}"
if [[ ! -d "${MESA_TMPDIR}" ]]; then
    mkdir -p "${MESA_TMPDIR}"
fi
if command -v cygpath >/dev/null 2>&1; then
    MESA_TMPDIR_NATIVE="$(cygpath -m "${MESA_TMPDIR}")"
else
    MESA_TMPDIR_NATIVE="${MESA_TMPDIR}"
fi
export TMP="${MESA_TMPDIR_NATIVE}"
export TEMP="${MESA_TMPDIR_NATIVE}"
export TMPDIR="${MESA_TMPDIR}"

if [[ ! -x "${HOST_BUILD_DIR}/src/compiler/clc/mesa_clc.exe" ||
      ! -x "${HOST_BUILD_DIR}/src/compiler/spirv/vtn_bindgen2.exe" ]]; then
    host_setup=()
    if [[ -f "${HOST_BUILD_DIR}/meson-private/coredata.dat" ]]; then
        host_setup+=(--wipe)
    fi
    "${MESON_BIN}" setup "${HOST_BUILD_DIR}" "${host_setup[@]}" \
        --cross-file "${HOST_CROSS_FILE}" \
        --buildtype=release \
        -Dvulkan-drivers= \
        -Dgallium-drivers= \
        -Dshader-cache=disabled \
        -Dplatforms= \
        -Dglx=disabled \
        -Degl=disabled \
        -Dopengl=false \
        -Dgles1=disabled \
        -Dgles2=disabled \
        -Dtools=[] \
        -Dllvm=enabled \
        -Dmesa-clc=enabled \
        -Dprecomp-compiler=enabled \
        -Dinstall-mesa-clc=true
    "${NINJA_BIN}" -C "${HOST_BUILD_DIR}" \
        src/compiler/clc/mesa_clc.exe \
        src/compiler/spirv/vtn_bindgen2.exe
fi

setup_mode=()
if [[ -f "${BUILD_DIR}/meson-private/coredata.dat" ]]; then
    setup_mode+=(--wipe)
fi

"${MESON_BIN}" setup "${BUILD_DIR}" "${setup_mode[@]}" \
    --cross-file "${CROSS_FILE}" \
    --native-file "${NATIVE_FILE}" \
    --default-library=static \
    --prefix=/opt/devkitpro/portlibs/switch \
    --libdir=lib \
    --buildtype="${BUILD_TYPE}" \
    -Doptimization="${OPTIMIZATION}" \
    -Db_lto=false \
    -Db_ndebug=true \
    -Dvulkan-drivers=nouveau \
    -Dgallium-drivers=nouveau,zink \
    -Dgallium-rusticl=false \
    -Dplatforms=switch \
    -Degl-native-platform=switch \
    -Dglx=disabled \
    -Degl=enabled \
    -Dopengl=true \
    -Dgles1=enabled \
    -Dgles2=enabled \
    -Dvideo-codecs= \
    -Dshader-cache=enabled \
    -Dxmlconfig=enabled \
    -Dexpat=enabled \
    -Dtools=[] \
    -Dllvm=disabled \
    -Dshared-glapi=disabled \
    -Dshared-llvm=disabled \
    -Dmesa-clc=system \
    -Dprecomp-compiler=system \
    -Dcpp_rtti=false \
    -Dbuild-tests=false \
    -Dnvk-build-id="$(git rev-parse HEAD)"

"${NINJA_BIN}" -C "${BUILD_DIR}"

case "${DESTDIR}" in
    "${ROOT_DIR}"/mesa-unified-install*) ;;
    *)
        echo "Refusing to clean unexpected DESTDIR outside the repository's unified stage: ${DESTDIR}" >&2
        exit 2
        ;;
esac
rm -rf -- "${DESTDIR}"
"${MESON_BIN}" install -C "${BUILD_DIR}" --destdir "${DESTDIR}"

for pc_file in "${SDK_DIR}"/lib/pkgconfig/*.pc; do
    sed -i 's|^prefix=.*|prefix=${pcfiledir}/../..|' "${pc_file}"
done

required_files=(
    lib/libEGL.a
    lib/libGL.a
    lib/libGLESv1_CM.a
    lib/libGLESv2.a
    lib/libglapi.a
    lib/libvulkan.a
    lib/pkgconfig/egl.pc
    lib/pkgconfig/gl.pc
    lib/pkgconfig/glesv1_cm.pc
    lib/pkgconfig/glesv2.pc
    lib/pkgconfig/glapi.pc
    lib/pkgconfig/vulkan.pc
    lib/cmake/OpenGL/OpenGLConfig.cmake
    lib/cmake/Vulkan/VulkanConfig.cmake
    include/EGL/egl.h
    include/EGL/eglext.h
    include/GL/gl.h
    include/GL/glcorearb.h
    include/GLES2/gl2.h
    include/vulkan/vulkan.h
    include/vulkan/vulkan_vi.h
    share/drirc.d/00-zink-defaults.conf
)
for relative in "${required_files[@]}"; do
    if [[ ! -f "${SDK_DIR}/${relative}" ]]; then
        echo "Unified SDK is missing ${relative}" >&2
        exit 1
    fi
done

if [[ ! -d "${DIST_DIR}" ]]; then
    mkdir -p "${DIST_DIR}"
fi
"${PYTHON_BIN}" "${ROOT_DIR}/tools/package-switch-sdk.py" \
    --stage "${DESTDIR}" \
    --output "${PACKAGE_PATH}" \
    --epoch "${SOURCE_DATE_EPOCH}"

echo "Unified Mesa Switch SDK staged at:"
echo "  ${SDK_DIR}"
echo "Unified SDK package:"
echo "  ${PACKAGE_PATH}"
