// ngx_probe: the Vulkan extensions and support NGX reports for DLSS SR and DLSS-G on this PC (plan-dlss.md D0).
// No game, no NGX init: only the requirement queries, on a throwaway Vulkan instance.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_vk.h>

static const wchar_t* dll_path = L".";

static NVSDK_NGX_FeatureDiscoveryInfo Discovery(NVSDK_NGX_Feature feature, NVSDK_NGX_FeatureCommonInfo& common) {
    NVSDK_NGX_FeatureDiscoveryInfo d{};
    common.PathListInfo = {&dll_path, 1};
    d.SDKVersion = NVSDK_NGX_Version_API;
    d.FeatureID = feature;
    d.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    d.Identifier.v.ProjectDesc = {"42359704-c9f3-4806-9fb5-d469000fdb8a", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "gow3-windows"};
    d.ApplicationDataPath = L".";
    d.FeatureInfo = &common;
    return d;
}

int main(int argc, char** argv) {
    if (argc > 1) {
        static std::wstring dir(argv[1], argv[1] + std::strlen(argv[1]));
        dll_path = dir.c_str();
    }
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    if (!vk) { std::puts("vulkan-1.dll not found"); return 1; }
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vk, "vkGetInstanceProcAddr"));
    auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    const struct { NVSDK_NGX_Feature id; const char* name; } features[] = {
        {NVSDK_NGX_Feature_SuperSampling, "DLSS SR (SuperSampling)"},
        {NVSDK_NGX_Feature_FrameGeneration, "DLSS-G (FrameGeneration)"}};

    std::set<std::string> instance_ext;
    for (const auto& f : features) {
        NVSDK_NGX_FeatureCommonInfo common{};
        auto d = Discovery(f.id, common);
        uint32_t n = 0;
        VkExtensionProperties* ext = nullptr;
        NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&d, &n, &ext);
        std::printf("%s: instance extensions (result 0x%08x):", f.name, unsigned(r));
        for (uint32_t i = 0; i < n; ++i) { std::printf(" %s", ext[i].extensionName); instance_ext.insert(ext[i].extensionName); }
        std::puts("");
    }
    std::vector<const char*> names;
    for (const auto& e : instance_ext) names.push_back(e.c_str());
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = uint32_t(names.size());
    ci.ppEnabledExtensionNames = names.data();
    VkInstance instance{};
    if (VkResult vr = create_instance(&ci, nullptr, &instance); vr != VK_SUCCESS) { std::printf("vkCreateInstance %d\n", vr); return 1; }
    auto enum_devices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
    auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"));
    auto enum_ext = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(gipa(instance, "vkEnumerateDeviceExtensionProperties"));
    uint32_t count = 0;
    enum_devices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    enum_devices(instance, &count, devices.data());
    for (VkPhysicalDevice pd : devices) {
        VkPhysicalDeviceProperties p{};
        props(pd, &p);
        std::printf("\nGPU: %s (vendor 0x%04x, driver 0x%08x, API %u.%u.%u)\n", p.deviceName, p.vendorID, p.driverVersion,
                    VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
        if (p.vendorID != 0x10de) continue;
        uint32_t m = 0;
        enum_ext(pd, nullptr, &m, nullptr);
        std::vector<VkExtensionProperties> have(m);
        enum_ext(pd, nullptr, &m, have.data());
        for (const auto& f : features) {
            NVSDK_NGX_FeatureCommonInfo common{};
            auto d = Discovery(f.id, common);
            NVSDK_NGX_FeatureRequirement req{};
            NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, pd, &d, &req);
            std::printf("%s: requirement query 0x%08x, supported %s (code 0x%x), min HW arch 0x%x, min OS %s\n", f.name,
                        unsigned(r), req.FeatureSupported == NVSDK_NGX_FeatureSupportResult_Supported ? "yes" : "NO",
                        unsigned(req.FeatureSupported), req.MinHWArchitecture, req.MinOSVersion);
            uint32_t n = 0;
            VkExtensionProperties* ext = nullptr;
            r = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance, pd, &d, &n, &ext);
            std::printf("  device extensions (result 0x%08x):\n", unsigned(r));
            for (uint32_t i = 0; i < n; ++i) {
                bool ok = false;
                for (const auto& h : have) ok |= std::strcmp(h.extensionName, ext[i].extensionName) == 0;
                std::printf("    %s%s\n", ext[i].extensionName, ok ? "" : "  (NOT offered by this GPU)");
            }
        }
    }
    reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"))(instance, nullptr);
    return 0;
}
