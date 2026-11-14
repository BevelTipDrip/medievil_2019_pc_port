// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 F2 (plan-fsr.md): AMD FSR 3.1 as an Ue4Upscaler backend (vk_ue4_upscaler.h), through the vendored
// FSR-Vulkan portable upscaler (gpu/third_party/fsr-vulkan: FSR 3.1.4 built from AMD's MIT source). Native
// anti-aliasing when the input and output sizes match (render scale 100), upscaling below it. The inputs are
// the front end's: the HDR colour before tonemapping (auto exposure), UE4's reversed infinite depth, U2's
// motion vectors (render pixels, previous minus current: FSR's convention at scale 1) and the game's jitter.
// The reactive mask (plan-fsr.md step 4, fsr_reactive above 0): UE4's responsive-AA pixels, from the motion pass.
// The menu's sharpening is FSR's own RCAS. GOW3_FSR=0 leaves it out.

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>

// Our Vulkan headers first (vk_common.h): ffx_vk_portable.h's <vulkan/vulkan.h> must find them included.
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_ue4_upscaler.h"

#include "ffx_vk_portable.h"

namespace Vulkan {

namespace {

FfxVkPortableImage Describe(const Ue4UpscaleImage& image, FfxVkPortableResourceState state) {
    FfxVkPortableImage out{};
    out.structSize = sizeof(out);
    out.image = image.image;
    out.format = static_cast<VkFormat>(image.format);
    out.extent = {image.width, image.height};
    out.mipCount = 1;
    out.arrayLayers = 1;
    out.usage = static_cast<VkImageUsageFlags>(image.usage);
    out.aspect = static_cast<VkImageAspectFlags>(image.aspect);
    // GENERIC_READ and UNORDERED_ACCESS are VK_IMAGE_LAYOUT_GENERAL in the FidelityFX backend
    // (ffx_vk.cpp getVKImageLayoutFromResourceState), the layout the front end leaves them in; the
    // backend puts every image back in its state after the dispatch.
    out.state = state;
    return out;
}

void PrintIssues(const char* what, u64 issues) {
    std::printf("UE4 upscaler: FSR %s rejected:", what);
    for (u32 bit = 0; bit < 64; ++bit) {
        if (issues & (1ull << bit)) {
            std::printf(" %s", ffxVkPortableValidationIssueName(1ull << bit));
        }
    }
    std::printf("\n");
    std::fflush(stdout);
}

class FsrBackend final : public Ue4UpscalerBackend {
public:
    explicit FsrBackend(const Instance& instance_) : instance{instance_} {}

    ~FsrBackend() override {
        if (context) {
            // Shutdown: nothing may still use the context's resources.
            (void)instance.GetDevice().waitIdle();
            ffxVkPortableUpscaleContextDestroy(context);
        }
    }

    const char* Name() const override {
        return "FSR";
    }

    bool Available() const override {
        return true; // MakeFsrBackend checked the device
    }

    bool Has(const Ue4UpscaleDesc& desc) const override {
        return context && desc == current;
    }

    bool Create(vk::CommandBuffer, const Ue4UpscaleDesc& desc) override {
        // The front end waited for the GPU to finish with the old context.
        ffxVkPortableUpscaleContextDestroy(context);
        context = nullptr;
        FfxVkPortableDeviceInfo device_info{};
        device_info.structSize = sizeof(device_info);
        device_info.instance = instance.GetInstance();
        device_info.physicalDevice = instance.GetPhysicalDevice();
        device_info.device = instance.GetDevice();
        device_info.getDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
        device_info.queue = instance.GetGraphicsQueue();
        device_info.queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex();
        device_info.shaderFloat16Enabled = instance.IsShaderFloat16Enabled();
        device_info.subgroupSizeControlEnabled = instance.IsSubgroupSizeControlEnabled();
        device_info.synchronization2Enabled = VK_TRUE;
        device_info.shaderStorageImageWriteWithoutFormatEnabled = VK_TRUE;

        FfxVkPortableUpscaleCreateInfo create_info{};
        create_info.structSize = sizeof(create_info);
        create_info.flags = FFX_VK_PORTABLE_CONTEXT_HDR_COLOR_INPUT | FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE |
                            FFX_VK_PORTABLE_CONTEXT_DEPTH_INVERTED | FFX_VK_PORTABLE_CONTEXT_DEPTH_INFINITE;
        create_info.maxRenderSize = {desc.input_width, desc.input_height};
        create_info.maxOutputSize = {desc.output_width, desc.output_height};
        if (const u64 issues = ffxVkPortableValidateUpscaleCreateInfo(&create_info)) {
            PrintIssues("context", issues);
            return false;
        }
        if (ffxVkPortableUpscaleContextCreate(&device_info, &create_info, &context) != FFX_VK_PORTABLE_OK) {
            context = nullptr;
            return false;
        }
        current = desc;
        this->create_info = create_info;
        FfxVkPortableMemoryUsage usage{};
        usage.structSize = sizeof(usage);
        if (ffxVkPortableUpscaleContextGetMemoryUsage(context, &usage) == FFX_VK_PORTABLE_OK) {
            std::printf("UE4 upscaler: FSR 3.1 context, %.1f MB of GPU memory\n",
                        double(usage.totalUsageInBytes) / (1024.0 * 1024.0));
        }
        return true;
    }

    bool Evaluate(vk::CommandBuffer cmdbuf, const Ue4UpscaleFrame& frame) override {
        FfxVkPortableUpscaleDispatchInfo info{};
        info.structSize = sizeof(info);
        info.commandBuffer = cmdbuf;
        info.color = Describe(frame.color, FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        info.depth = Describe(frame.depth, FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        info.motionVectors = Describe(frame.motion, FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        info.output = Describe(frame.output, FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);
        info.exposure.structSize = sizeof(info.exposure); // auto exposure
        // Null (structSize only): FSR's own 1x1 blank mask, as before step 4.
        info.reactiveMask = frame.reactive.image ? Describe(frame.reactive, FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ)
                                                 : FfxVkPortableImage{};
        info.reactiveMask.structSize = sizeof(info.reactiveMask);
        info.transparencyAndCompositionMask.structSize = sizeof(info.transparencyAndCompositionMask);
        info.jitterOffset = {frame.jitter_x, frame.jitter_y};
        info.motionVectorScale = {1.0f, 1.0f};
        info.renderSize = {frame.color.width, frame.color.height};
        info.outputSize = {frame.output.width, frame.output.height};
        info.frameTimeMilliseconds = frame.frame_ms;
        info.preExposure = 1.0f; // View PreExposure is 1 in this game
        info.cameraNear = frame.near_plane;
        info.cameraFar = std::numeric_limits<float>::max(); // infinite (the context flag)
        info.cameraVerticalFovRadians = frame.fov_y;
        info.viewSpaceToMeters = 0.01f; // UE units are centimetres
        // The menu's sharpening, as FSR's own RCAS (the presenter's stays off while FSR runs).
        info.sharpness = frame.sharpness < 0.0f ? 0.0f : frame.sharpness;
        info.enableSharpening = frame.sharpness < 0.0f ? VK_FALSE : VK_TRUE;
        info.reset = frame.reset ? VK_TRUE : VK_FALSE;
        info.frameId = frame_id++;
        if (const u64 issues = ffxVkPortableValidateUpscaleDispatchInfo(&create_info, &info)) {
            if (!reported) {
                reported = true;
                PrintIssues("dispatch", issues);
            }
            return false;
        }
        return ffxVkPortableUpscaleContextRecordDispatch(context, &info) == FFX_VK_PORTABLE_OK;
    }

private:
    const Instance& instance;
    FfxVkPortableUpscaleContext* context = nullptr;
    FfxVkPortableUpscaleCreateInfo create_info{};
    Ue4UpscaleDesc current{};
    u64 frame_id = 0;
    bool reported = false;
};

} // namespace

std::unique_ptr<Ue4UpscalerBackend> MakeFsrBackend(const Instance& instance) {
    if (const char* value = std::getenv("GOW3_FSR"); value && value[0] == '0') {
        return nullptr;
    }
    // FSR 3's accumulate shaders write storage images without a format.
    if (!instance.IsStorageImageWriteWithoutFormatEnabled()) {
        std::printf("UE4 upscaler: FSR needs shaderStorageImageWriteWithoutFormat; not available\n");
        return nullptr;
    }
    return std::make_unique<FsrBackend>(instance);
}

} // namespace Vulkan
