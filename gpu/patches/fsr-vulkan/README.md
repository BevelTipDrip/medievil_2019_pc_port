# Local changes to gpu/third_party/fsr-vulkan

The submodule points at upstream FireBurn/FSR-Vulkan (`c64f093`). `build.sh` applies the
patches here to its working tree when they are not applied yet:

- `0001-...`: `GOW3_FSR4_PROFILE` (GPU time per FSR 4 pass) and `GOW3_FSR4_STATS` (driver
  statistics of each pass) in the FSR 4 v07 provider.
- `0002-...`: `#include <mutex>` in the FFX Vulkan backend; libc++ (the Windows MSYS2 CLANG64
  build) does not include it transitively.
- `0003-...`: `ffxFsr4VkForgetExternalImage` in the FSR 4 v07 backend: its registry of imported
  images (eight, by view) only grew, so a host replacing its images filled it. MediEvil's FSR 4
  backend (vk_ue4_fsr4.cpp, plan-fsr.md step 7) forgets a view when its image is replaced.
- `0004-...`: `FfxFsr4VkExternalImageState::aspectMask` (0: colour, as before): the provider's
  barriers on imported images were always colour-aspect, invalid on MediEvil's D32S8 depth.
