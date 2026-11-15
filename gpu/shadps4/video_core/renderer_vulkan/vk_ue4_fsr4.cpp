// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 F7a (plan-fsr.md step 7): AMD FSR 4 as an Ue4Upscaler backend (vk_ue4_upscaler.h), through the
// vendored FSR-Vulkan FSR 4 provider: the v07 INT8 model (integer dot products, no matrix units), so it
// runs on NVIDIA and older AMD GPUs too. Its SPIR-V passes and weights are not in the repository:
// tools/fetch_fsr4_assets.sh downloads them (MIT, built by Q2RTX from AMD's FSR 4 source) into
// fsr4_shaders/ (GOW3_FSR4_DIR overrides; run.py falls back to the data directory's). The model is chosen
// nearest to the output / render ratio (native AA, quality, balanced, performance, ultra performance;
// GOW3_FSR4_DRS=1: the dynamic-resolution model at any ratio), the 1080 or 2160 tier from the output.
//
// Unlike FSR 3.1 the provider leaves frame lifetimes to the host: at most three unretired frames, each
// retired once the GPU passed the scheduler tick its commands run under (Ue4UpscaleFrame::tick); all on
// the recording thread with the scheduler's semaphore only (no Wait, Finish or submit there). It looks up
// images by view in a registry of eight: the front end's views are new each frame, so each is registered
// for its dispatch and forgotten right after (gpu/patches/fsr-vulkan 0003); the depth image's barriers get
// its own aspect (0004). GOW3_FSR4=0 leaves it out; GOW3_FSR4_PROFILE=1 logs GPU time per pass (0001).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Our Vulkan headers first (vk_common.h): the provider's <vulkan/vulkan.h> must find them included.
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"
#include "video_core/renderer_vulkan/vk_ue4_upscaler.h"

#include "ffx_vk_fsr4_v07.h"
#include "ffx_vk_fsr4_v07_assets.h"
#include "ffx_vk_fsr4_v07_schedule.h"

namespace Vulkan {

namespace {

std::string AssetDir() {
    const char* dir = std::getenv("GOW3_FSR4_DIR");
    return dir && dir[0] ? dir : "fsr4_shaders";
}

bool ReadFile(const std::string& path, std::vector<u8>& data) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }
    data.resize(size_t(file.tellg()));
    file.seekg(0);
    return bool(file.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size())));
}

/// The model trained nearest to this output / render ratio (AMD's presets: native AA 1.0, quality 1.5, balanced
/// 1.7, performance 2.0, ultra performance 3.0). The provider's own choice takes the largest preset ratio at or
/// below the ratio, so render scale 67% (1.49) got the native model (T-F10); here 67% and 75% get quality.
/// `drs`: the dynamic-resolution model, at any ratio.
FfxFsr4ModelPreset NearestModel(u32 render_width, u32 output_width, bool drs) {
    if (drs) {
        return FFX_FSR4_MODEL_PRESET_DRS;
    }
    static constexpr std::array<std::pair<float, FfxFsr4ModelPreset>, 5> presets{{
        {1.0f, FFX_FSR4_MODEL_PRESET_NATIVE_AA},
        {1.5f, FFX_FSR4_MODEL_PRESET_QUALITY},
        {1.7f, FFX_FSR4_MODEL_PRESET_BALANCED},
        {2.0f, FFX_FSR4_MODEL_PRESET_PERFORMANCE},
        {3.0f, FFX_FSR4_MODEL_PRESET_ULTRA_PERFORMANCE},
    }};
    const float ratio = render_width ? float(output_width) / float(render_width) : 1.0f;
    const auto nearest = std::ranges::min_element(
        presets, {}, [ratio](const auto& preset) { return std::abs(preset.first - ratio); });
    return nearest->second;
}

/// The aspect of the provider's barriers on an imported image: its whole aspect (a depth-stencil image
/// cannot be transitioned by one aspect without separateDepthStencilLayouts).
VkImageAspectFlags BarrierAspect(vk::Format format) {
    switch (format) {
    case vk::Format::eD32SfloatS8Uint:
    case vk::Format::eD24UnormS8Uint:
    case vk::Format::eD16UnormS8Uint:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    case vk::Format::eD32Sfloat:
    case vk::Format::eD16Unorm:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    default:
        return 0; // colour
    }
}

class Fsr4Backend final : public Ue4UpscalerBackend {
public:
    Fsr4Backend(const Instance& instance_, Semaphore& semaphore_)
        : instance{instance_}, semaphore{semaphore_}, drs{[] {
              const char* value = std::getenv("GOW3_FSR4_DRS");
              return value && value[0] == '1';
          }()} {}

    ~Fsr4Backend() override {
        if (backend_ok) {
            // Shutdown: nothing may still use the context's resources.
            (void)instance.GetDevice().waitIdle();
        }
        Destroy();
    }

    const char* Name() const override {
        return "FSR 4";
    }

    bool Available() const override {
        return true; // MakeFsr4Backend checked the device and the assets
    }

    bool Has(const Ue4UpscaleDesc& desc) const override {
        return context_ok && desc == current;
    }

    bool Create(vk::CommandBuffer, const Ue4UpscaleDesc& desc) override {
        // The front end drained the recording thread and waited for the submitted work; frames of
        // the old context still unretired are waited for here (the GPU thread may wait).
        for (const auto& [id, tick] : in_flight) {
            semaphore.Wait(tick);
        }
        Destroy();
        const FfxFsr4ModelPreset preset = NearestModel(desc.input_width, desc.output_width, drs);
        if (!CreateContext(preset, desc.output_width, desc.output_height)) {
            Destroy();
            return false;
        }
        current = desc;
        return true;
    }

    bool Evaluate(vk::CommandBuffer cmdbuf, const Ue4UpscaleFrame& frame) override {
        Retire();
        const u64 frame_id = next_frame_id++;
        VkResult begin = ffxFsr4VkBeginFrame(&backend, frame_id);
        while (begin == VK_NOT_READY && !in_flight.empty()) {
            // Three frames of the provider still run on the GPU: rare (the frame queue is shorter).
            semaphore.Wait(in_flight.front().second);
            ++waits;
            Retire();
            begin = ffxFsr4VkBeginFrame(&backend, frame_id);
        }
        if (begin != VK_SUCCESS) {
            Report("no free provider frame", begin);
            return false;
        }
        // A frame begun must be retired even when nothing is recorded, or every later BeginFrame fails.
        in_flight.emplace_back(frame_id, frame.tick);

        constexpr VkAccessFlags read = VK_ACCESS_SHADER_READ_BIT;
        const std::array<std::pair<const Ue4UpscaleImage*, VkAccessFlags>, 4> images{{
            {&frame.color, read},
            {&frame.depth, read},
            {&frame.motion, read},
            {&frame.output, read | VK_ACCESS_SHADER_WRITE_BIT},
        }};
        u32 registered = 0;
        const auto forget = [&] {
            for (u32 i = 0; i < registered; ++i) {
                ffxFsr4VkForgetExternalImage(&backend, images[i].first->view);
            }
        };
        for (const auto& [image, access] : images) {
            const FfxFsr4VkExternalImageState state{
                .structSize = sizeof(FfxFsr4VkExternalImageState),
                .image = image->image,
                .view = image->view,
                .layout = VK_IMAGE_LAYOUT_GENERAL,
                .stageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                .accessMask = access,
                .restoreLayout = VK_IMAGE_LAYOUT_GENERAL,
                .restoreStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                .restoreAccessMask = access,
                .aspectMask = BarrierAspect(image->format),
            };
            if (const VkResult result = ffxFsr4VkSetExternalImageState(&backend, &state); result != VK_SUCCESS) {
                forget();
                Report("image registration failed", result);
                return false;
            }
            ++registered;
        }
        const auto resource = [](const Ue4UpscaleImage& image, u32 format, u32 state) {
            FfxApiResource r{};
            r.resource = reinterpret_cast<void*>(VkImageView(image.view));
            r.description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
            r.description.format = format;
            r.description.width = image.width;
            r.description.height = image.height;
            r.description.depth = 1;
            r.description.mipCount = 1;
            r.state = state;
            return r;
        };
        ffxDispatchDescUpscale d{};
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = reinterpret_cast<void*>(VkCommandBuffer(cmdbuf));
        d.color = resource(frame.color, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.depth = resource(frame.depth, FFX_SURFACE_FORMAT_R32_FLOAT, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        // RGBA16F, sampled (the pre-pass's t1): the provider reads .xy.
        d.motionVectors =
            resource(frame.motion, FFX_SURFACE_FORMAT_R16G16_FLOAT, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.output =
            resource(frame.output, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        d.jitterOffset = {frame.jitter_x, frame.jitter_y};
        // Render pixels, previous minus current; the provider divides by the render size.
        d.motionVectorScale = {1.0f, 1.0f};
        d.renderSize = {frame.color.width, frame.color.height};
        d.upscaleSize = {frame.output.width, frame.output.height};
        // The menu's FSR sharpening (fsr_sharpness), FSR 4's own RCAS as for FSR 3.1.
        d.enableSharpening = frame.sharpness > 0.0f;
        d.sharpness = std::max(frame.sharpness, 0.0f);
        d.enableAutoExposure = true;
        d.frameTimeDelta = frame.frame_ms;
        d.preExposure = 1.0f; // View PreExposure is 1 in this game
        d.reset = frame.reset;
        // Not read by this provider (its model takes no camera); filled as for FSR 3.1.
        d.cameraNear = frame.near_plane;
        d.cameraFar = 1e30f;
        d.cameraFovAngleVertical = frame.fov_y;
        d.viewSpaceToMetersFactor = 0.01f;
        const auto result = ffxFsr4V07Dispatch(&context, &d.header);
        // Each image's record was copied into the dispatch: the slots are free for the next frame's views.
        forget();
        if (result != FFX_API_RETURN_OK) {
            Report("dispatch failed", int(result));
            return false;
        }
        if (waits != reported_waits && (waits & (waits - 1)) == 0) {
            reported_waits = waits;
            std::printf("UE4 upscaler: FSR 4 waited for the GPU %llu time(s) (3 provider frames in flight)\n",
                        static_cast<unsigned long long>(waits));
        }
        return true;
    }

private:
    bool CreateContext(FfxFsr4ModelPreset preset, u32 ow, u32 oh) {
        FfxFsr4V07AssetSet assets{};
        if (!ffxFsr4V07BuildAssetSet(preset, ow, oh, &assets)) {
            std::printf("UE4 upscaler: FSR 4: output %ux%u is not supported by the v07 model\n", ow, oh);
            return false;
        }
        const std::string dir = AssetDir() + "/";
        // tools/fsr4_optimize.sh puts faster or fixed passes into opt/ (post through shared memory, pass
        // 11 without out-of-bounds writes); GOW3_FSR4_OPT=0 keeps the downloaded ones.
        const char* opt_env = std::getenv("GOW3_FSR4_OPT");
        const bool use_opt = !(opt_env && opt_env[0] == '0');
        u32 optimized = 0;
        std::string missing;
        const auto load = [&](const char* name, std::vector<u8>& data) {
            if (use_opt && ReadFile(dir + "opt/" + name, data)) {
                ++optimized;
                return true;
            }
            if (ReadFile(dir + name, data)) {
                return true;
            }
            missing = dir + name;
            return false;
        };
        std::array<std::vector<u8>, FFX_FSR4_VK_PASS_COUNT> code;
        std::vector<u8> initializer, weights;
        bool loaded = load(assets.pre, code[0]);
        for (u32 pass = 0; loaded && pass < FFX_FSR4_MODEL_PASS_COUNT; ++pass) {
            loaded = load(assets.model[pass], code[1 + pass]);
        }
        loaded = loaded && load(assets.post, code[13]) && load(assets.rcas, code[14]) &&
                 load(assets.spdAutoExposure, code[15]) && load(assets.initializer, initializer) &&
                 load(assets.prePassWeights, weights);
        if (!loaded) {
            std::printf("UE4 upscaler: FSR 4: missing %s (run tools/fetch_fsr4_assets.sh)\n", missing.c_str());
            return false;
        }
        if (initializer.size() != FFX_FSR4_V07_INITIALIZER_BYTES ||
            weights.size() != FFX_FSR4_V07_PRE_PASS_WEIGHTS_BYTES) {
            std::printf("UE4 upscaler: FSR 4: model weights of an unexpected size\n");
            return false;
        }
        static const std::array<std::string, FFX_FSR4_VK_PASS_COUNT> entries = [] {
            std::array<std::string, FFX_FSR4_VK_PASS_COUNT> names;
            names.fill("main");
            for (u32 pass = 1; pass <= FFX_FSR4_MODEL_PASS_COUNT; ++pass) {
                names[pass] = "fsr4_model_v07_i8_pass" + std::to_string(pass);
            }
            return names;
        }();
        FfxFsr4VkCreateInfo ci{};
        ci.device = instance.GetDevice();
        ci.physicalDevice = instance.GetPhysicalDevice();
        for (u32 i = 0; i < FFX_FSR4_VK_PASS_COUNT; ++i) {
            if (code[i].size() % 4) {
                std::printf("UE4 upscaler: FSR 4: invalid SPIR-V in %s\n", dir.c_str());
                return false;
            }
            ci.shaders[i] = {reinterpret_cast<const uint32_t*>(code[i].data()), code[i].size(), entries[i].c_str()};
        }
        ci.modelInitializer = initializer.data();
        ci.modelInitializerSize = initializer.size();
        ci.prePassWeights = weights.data();
        ci.prePassWeightsSize = weights.size();
        scratch.assign(ffxFsr4VkGetScratchMemorySize(), 0);
        ci.scratchBuffer = scratch.data();
        ci.scratchBufferSize = scratch.size();
        if (const VkResult result = ffxFsr4VkCreateContext(&ci, &backend); result != VK_SUCCESS) {
            std::printf("UE4 upscaler: FSR 4: Vulkan backend creation failed (%d)\n", int(result));
            backend = {};
            return false;
        }
        backend_ok = true;

        ffxCreateContextDescUpscale desc{};
        desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
        // The provider's tensors follow the output aligned to 8; any render size up to it fits.
        desc.maxRenderSize = {(ow + 7) & ~7u, (oh + 7) & ~7u};
        desc.maxUpscaleSize = {(ow + 7) & ~7u, (oh + 7) & ~7u};
        ffxFsr4V07SetBackendInterface(&backend);
        const auto created = ffxFsr4V07CreateContext(&context, &desc.header, nullptr);
        ffxFsr4V07SetBackendInterface(nullptr);
        if (created != FFX_API_RETURN_OK) {
            std::printf("UE4 upscaler: FSR 4: provider context creation failed (%d)\n", int(created));
            return false;
        }
        context_ok = true;
        std::printf("UE4 upscaler: FSR 4 v07 INT8 %s model, %s tier, output %ux%u%s\n",
                    ffxFsr4ModelPresetName(preset), assets.tier, ow, oh,
                    optimized ? ", optimized passes from opt/" : "");
        return true;
    }

    /// The GPU passed these frames' ticks: the provider may reuse their descriptor sets and constants.
    void Retire() {
        while (!in_flight.empty()) {
            const u64 tick = in_flight.front().second;
            if (!semaphore.IsFree(tick)) {
                semaphore.Refresh();
                if (!semaphore.IsFree(tick)) {
                    return;
                }
            }
            ffxFsr4VkRetireFrame(&backend, in_flight.front().first);
            in_flight.pop_front();
        }
    }

    /// The GPU is done with every frame (Create waited, or shutdown).
    void Destroy() {
        if (context_ok) {
            ffxFsr4V07DestroyContext(&context, nullptr);
            context_ok = false;
        }
        if (backend_ok) {
            ffxFsr4VkDestroyContext(reinterpret_cast<FfxFsr4VkContext*>(scratch.data()));
            backend_ok = false;
        }
        backend = {};
        in_flight.clear();
    }

    void Report(const char* what, int code) {
        if (!reported) {
            reported = true;
            std::printf("UE4 upscaler: FSR 4: %s (%d)\n", what, code);
            std::fflush(stdout);
        }
    }

    const Instance& instance;
    Semaphore& semaphore;
    const bool drs; ///< GOW3_FSR4_DRS=1: the dynamic-resolution model at any ratio
    std::vector<u8> scratch; ///< the provider's Vulkan context (FfxFsr4VkContext) lives here
    FfxInterface backend{};
    bool backend_ok = false;
    ffxContext context{};
    bool context_ok = false;
    Ue4UpscaleDesc current{};
    u64 next_frame_id = 1;
    std::deque<std::pair<u64, u64>> in_flight; ///< provider frame id, scheduler tick
    u64 waits = 0, reported_waits = 0;
    bool reported = false;
};

} // namespace

std::unique_ptr<Ue4UpscalerBackend> MakeFsr4Backend(const Instance& instance, Semaphore& semaphore) {
    if (const char* value = std::getenv("GOW3_FSR4"); value && value[0] == '0') {
        return nullptr;
    }
    if (!instance.IsFsr4Int8Supported()) {
        std::printf("UE4 upscaler: FSR 4 needs Float16, Int8, Int16, integer dot products and compute "
                    "derivatives; not available\n");
        return nullptr;
    }
    // The assets are the user's download; without them FSR 4 is not offered (the menu and F9 skip it).
    const std::string license = AssetDir() + "/LICENSE-FSR4-v07.txt";
    if (!std::ifstream(license)) {
        std::printf("UE4 upscaler: FSR 4 assets not found (%s; tools/fetch_fsr4_assets.sh)\n", AssetDir().c_str());
        return nullptr;
    }
    return std::make_unique<Fsr4Backend>(instance, semaphore);
}

} // namespace Vulkan
