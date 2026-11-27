# Source lineage

- **Mesa upstream** develops NVK, NIR, NAK, NIL and MME. The Windows Vulkan
  implementation here reuses that stack. [Mesa source](https://gitlab.freedesktop.org/mesa/mesa) · [NVK documentation](https://docs.mesa3d.org/drivers/nvk.html)
- **mesa-switch** carries the Switch-specific NVK and GM20B work. The build
  pins `0ff52079ca9e9e837ed27eb95a8d239f439a1864`, including the report
  encoder used by the driver. [Pinned Switch source](https://github.com/danfromtico/mesa-switch/tree/0ff52079ca9e9e837ed27eb95a8d239f439a1864)
- **Mesa Switch for Windows ARM64** adds the Windows NAK and ICD changes
  to the Mesa Switch source tree. The KMT transport and shared ABI live in
  [the NVK Windows backend](../src/nouveau/vulkan/nvkmd/windows/). The original Windows GPU driver
  history was extracted from `drivers/TegraGpu` at
  `8c835b6151f948c290b6f1be1f6aae577f1e3ee9`. The Lite integration came from
  WinX at `c5fba1a4735e6cfdb563a023d15ceae0af90af93`. GPU development and
  publication use the `main` branch of this standalone project.
  [Windows ARM64 source](https://github.com/ruje0504-2/mesa-switch-woa/tree/main)
- **WinX** and **Windows-for-Switch-Drivers** are the companion firmware and
  board-driver projects. They remain the source of the standard Lite boot
  chain, battery, power button and other platform drivers. The optional GPU
  ACPI patch here uses WinX commit `63d3c5aa85719b8fd009f4b166e741537eed80d7`
  as its base. [WinX](https://github.com/ruje0504-2/winx) · [Windows-for-Switch-Drivers](https://github.com/ruje0504-2/Windows-for-Switch-Drivers)

The Windows resource and display model draws on Microsoft's public graphics
driver samples. GM20B page tables, power sequences and engine initialization
also draw on Nouveau, Switchroot and NVIDIA sources. Individual source files
and the notice identify the relevant revisions and copyrights. [Notices](NOTICE)

The display audit also reviews Switchroot's NVIDIA DC code at
`3c40adf1081096bc61a7c1280434c163e7e79b48`, specifically `window.c` and
`dc.c`, for surface layout, update/activation and active-bank readback rules.
This is a GPL-2.0 hardware reference, not imported driver implementation.
[Pinned Switchroot source](https://gitlab.com/switchroot/kernel/l4t-kernel-nvidia/-/tree/3c40adf1081096bc61a7c1280434c163e7e79b48/drivers/video/tegra/dc)
