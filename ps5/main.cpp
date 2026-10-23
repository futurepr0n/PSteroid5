#include "vulkanexamplebase.h"
#include "ps5_samples.h"
#include <memory>
#include <fstream>
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance, const char *);
int main() {
    platform_init(PS5_TITLE_NAME);
    pad_open();
    volkInitializeCustom(vk_icdGetInstanceProcAddr);
    try {
        std::unique_ptr<VulkanExampleBase> app(ps5Samples[0].create());
        app->ps5.optionsEnds = false;
        // Optional console smoke test: write a frame count to /app0/smoke-test.txt.
        std::ifstream smoke("/app0/smoke-test.txt");
        unsigned frames = 0;
        if (smoke >> frames; frames > 0) {
            app->ps5.frameBudget = frames;
            app->benchmark.active = true;
            app->ps5.screenshotPath = "/app0/smoke-test.ppm";
        }
        if (!app->initVulkan()) return 1;
        app->prepare();
        app->renderLoop();
        vkDeviceWaitIdle(app->vulkanDevice->logicalDevice);
        return 0;
    } catch(const std::exception &error) {
        say("CSSteroids startup failed: %s", error.what());
        return 1;
    }
}
