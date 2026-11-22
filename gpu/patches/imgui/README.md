# Local changes to gpu/third_party/imgui

The submodule points at upstream ocornut/imgui (`f1cc2ae15`, 1.92.9b). `build.sh` applies the patches here to its
working tree when they are not applied yet:

- `0001-...`: `ImGui_ImplVulkan_CreatePipelineForFormat` in the Vulkan backend: the main pipeline's setup for
  another colour format and fragment shader (the caller owns the result). The port's overlay draws into an
  HDR10 swapchain (A2B10G10R10) with `imgui_hdr.frag`, which encodes its colours as PQ
  (`gpu/shim/gow3_overlay.cpp` `HdrPipeline`; research/patches/present/hdr-output.md).
