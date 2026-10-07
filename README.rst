Mesa for Nintendo Switch
========================

A Nintendo Switch (Horizon OS) port of `Mesa <https://mesa3d.org>`_, providing
native EGL, OpenGL, OpenGL ES and Vulkan on the Tegra X1 ``GM20B`` GPU through
Mesa's Nouveau drivers.

Based on Mesa 26.2.4.

This repository lives at https://github.com/danfromtico/mesa-switch.


What you get
------------

* **Vulkan** — NVK, built loaderless.  The application links ``libvulkan.a``
  directly; there is no Vulkan loader and no ICD to install.
* **OpenGL and OpenGL ES 1/2/3** — Gallium NVC0 by default, or Zink layered on
  top of NVK.
* **EGL** — Switch frontend supporting NWindow surfaces and pbuffers.

Everything links statically into an NRO/NSO.  Explicit Vulkan and either
OpenGL backend can coexist in one process, because both sit on the same
Horizon backend.


How this differs from upstream Mesa
-----------------------------------

``src/nouveau/horizon`` is a Horizon GPU backend written directly against
libnx.  It owns the device, address space, memory, channel submission,
syncpoint synchronization, cache maintenance and error handling.  Gallium/NVC0
and NVK are both adapters over that one shared backend.

Consequences worth knowing before you build:

* No external ``switch-libdrm_nouveau`` library and no patched libnx ABI are
  required.
* Horizon is a unified-memory platform, so CPU-visible allocations carry an
  explicit cache policy and GPU work is ordered with native syncpoint fences.
* Unknown GPU completion fails closed — resources stay owned or quarantined
  rather than being recycled without proof of completion.
* Horizon has no POSIX file descriptors and no DMA-BUF, so NVK does not expose
  ``VK_KHR_external_memory_fd``, ``VK_EXT_external_memory_dma_buf`` or
  ``VK_EXT_map_memory_placed``.
* EGL configurations are advertised as non-conformant until Khronos
  conformance testing is complete.


Requirements
------------

devkitA64 and libnx, plus libelf, expat, zlib, zstd, Meson, Ninja, and a Rust
toolchain that includes the ``aarch64-unknown-linux-gnu`` standard library.


Building
--------

Three entry points, depending on what you need:

.. code-block:: sh

  ./build-switch.sh     # NVK Vulkan only, via Docker (Docker.rust)
  ./build-opengl.sh     # EGL / OpenGL / OpenGL ES only
  ./build-unified.sh    # both APIs staged into one release SDK

``build-switch.sh`` runs the whole cross build inside a container, so Docker is
the only host requirement.  It emits static archives under ``builddir-switch``.

``build-opengl.sh`` cross-compiles against ``switch_cross_file.txt`` and stages
into ``mesa-install``.

``build-unified.sh`` currently requires the checked MSYS2 toolchain and will
refuse to run elsewhere.  It installs static GL, GLES, EGL and Vulkan
libraries, Khronos headers, pkg-config metadata and CMake package files under a
single Switch portlibs prefix, then writes a deterministic SDK ZIP to
``dist``.  It rejects a dirty checkout unless you set ``ALLOW_DIRTY=1``.

The installed CMake packages export ``OpenGL::GL``, ``OpenGL::EGL``,
``OpenGL::GLES1``, ``OpenGL::GLES2``, ``Vulkan::Headers`` and
``Vulkan::Vulkan``.


Selecting a GL backend at runtime
---------------------------------

NVC0 is the default.  Choose Zink before ``eglInitialize`` with either
``MESA_SWITCH_GL_DRIVER=zink`` or ``MESA_LOADER_DRIVER_OVERRIDE=zink``; pass
``MESA_SWITCH_GL_DRIVER=nvc0`` to force the native Gallium driver.  One EGL
display keeps one backend for its lifetime.


Documentation
-------------

`docs/switch-opengl.rst <docs/switch-opengl.rst>`_ is the reference for this
port: architecture, the full set of runtime environment variables, Zink and
presentation behaviour, and platform limitations.

For Mesa itself, see the upstream documentation at https://docs.mesa3d.org.


Upstream
--------

Upstream Mesa lives at https://gitlab.freedesktop.org/mesa/mesa and is the
place for bugs and patches that are not specific to the Switch port.  Issues
with the Horizon backend, the Switch WSI, or the build scripts here belong in
this repository instead.


License
-------

MIT, matching upstream Mesa.  Individual components carry their own licenses;
see ``licenses/`` and the headers of the files themselves.
