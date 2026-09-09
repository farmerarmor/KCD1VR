#include <openxr/openxr.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

std::uint32_t AlignUp(std::uint32_t value, std::uint32_t alignment)
{
    return (value + alignment - 1U) / alignment * alignment;
}

std::uint32_t AlignDown(std::uint32_t value, std::uint32_t alignment)
{
    return value / alignment * alignment;
}

}  // namespace

int main(int argc, char** argv)
{
    double scale = 1.0;
    if (argc == 3 && std::string_view(argv[1]) == "--scale") {
        char* end = nullptr;
        scale = std::strtod(argv[2], &end);
        if (end == argv[2] || *end != '\0' || scale < 0.25 || scale > 3.0) {
            std::cerr << "Render scale must be between 0.25 and 3.0.\n";
            return 2;
        }
    } else if (argc != 1) {
        std::cerr << "Usage: KCD1VRResolution.exe [--scale 1.0]\n";
        return 2;
    }

    XrInstanceCreateInfo instanceInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    std::snprintf(instanceInfo.applicationInfo.applicationName,
                  XR_MAX_APPLICATION_NAME_SIZE, "KCD1VR Resolution Setup");
    instanceInfo.applicationInfo.applicationVersion = 1;
    std::snprintf(instanceInfo.applicationInfo.engineName,
                  XR_MAX_ENGINE_NAME_SIZE, "KCD1VR");
    instanceInfo.applicationInfo.engineVersion = 1;
    instanceInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

    XrInstance instance = XR_NULL_HANDLE;
    XrResult result = xrCreateInstance(&instanceInfo, &instance);
    if (XR_FAILED(result)) {
        std::cerr << "OpenXR runtime initialization failed (" << result << ").\n";
        return 3;
    }

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    result = xrGetSystem(instance, &systemInfo, &systemId);
    if (XR_FAILED(result)) {
        std::cerr << "No active OpenXR headset was available (" << result << ").\n";
        xrDestroyInstance(instance);
        return 4;
    }

    std::uint32_t viewCount = 0;
    result = xrEnumerateViewConfigurationViews(
        instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        0, &viewCount, nullptr);
    if (XR_FAILED(result) || viewCount < 2) {
        std::cerr << "The runtime did not expose a stereo view configuration.\n";
        xrDestroyInstance(instance);
        return 5;
    }

    std::vector<XrViewConfigurationView> views(
        viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    result = xrEnumerateViewConfigurationViews(
        instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        viewCount, &viewCount, views.data());
    if (XR_FAILED(result)) {
        std::cerr << "Reading the headset view dimensions failed (" << result << ").\n";
        xrDestroyInstance(instance);
        return 6;
    }

    std::uint32_t recommendedWidth = 0;
    std::uint32_t recommendedHeight = 0;
    std::uint32_t maximumWidth = UINT32_MAX;
    std::uint32_t maximumHeight = UINT32_MAX;
    for (const auto& view : views) {
        recommendedWidth = std::max(recommendedWidth,
                                    view.recommendedImageRectWidth);
        recommendedHeight = std::max(recommendedHeight,
                                     view.recommendedImageRectHeight);
        maximumWidth = std::min(maximumWidth, view.maxImageRectWidth);
        maximumHeight = std::min(maximumHeight, view.maxImageRectHeight);
    }

    constexpr std::uint32_t alignment = 8;
    const std::uint32_t requestedWidth = AlignUp(
        static_cast<std::uint32_t>(std::lround(recommendedWidth * scale)), alignment);
    const std::uint32_t requestedHeight = AlignUp(
        static_cast<std::uint32_t>(std::lround(recommendedHeight * scale)), alignment);
    const std::uint32_t eyeWidth = std::min(requestedWidth,
                                            AlignDown(maximumWidth, alignment));
    const std::uint32_t eyeHeight = std::min(requestedHeight,
                                             AlignDown(maximumHeight, alignment));

    XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(instance, systemId, &properties);
    std::cout << "{\"eyeWidth\":" << eyeWidth
              << ",\"eyeHeight\":" << eyeHeight
              << ",\"recommendedWidth\":" << recommendedWidth
              << ",\"recommendedHeight\":" << recommendedHeight
              << ",\"maximumWidth\":" << maximumWidth
              << ",\"maximumHeight\":" << maximumHeight
              << ",\"systemName\":\"" << properties.systemName << "\"}\n";

    xrDestroyInstance(instance);
    return eyeWidth > 0 && eyeHeight > 0 ? 0 : 7;
}
