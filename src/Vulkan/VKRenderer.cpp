#include "VKRenderer.hpp"

const std::vector<const char*> validationLayers = {"VK_LAYER_KHRONOS_validation"};

const std::vector<const char*> deviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

#ifdef NDEBUG
const bool enableValidationLayers = false;
#else
const bool enableValidationLayers = true;
#endif

VkResult CreateDebugUtilsMessengerEXT(VkInstance instance,
                                      const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
                                      const VkAllocationCallbacks* pAllocator,
                                      VkDebugUtilsMessengerEXT* pDebugMessenger) {
    auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
        instance, "vkCreateDebugUtilsMessengerEXT");
    if (func != nullptr) {
        return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
    } else {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
}

void DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger,
                                   const VkAllocationCallbacks* pAllocator) {
    auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
        instance, "vkDestroyDebugUtilsMessengerEXT");
    if (func != nullptr) {
        func(instance, debugMessenger, pAllocator);
    }
}

void VKRenderer::Run(
    const std::string& windowTitle, const uint32_t windowWidth, const uint32_t windowHeight,
    const uint32_t maxFramesInFlight,
    std::function<void(VulkanState& vulkanState, SDL_Window* window, int32_t width, int32_t height)>
        initCallback,
    std::function<void(VulkanState& vulkanState)> updateCallback,
    std::function<void(VulkanState& vulkanState, VkCommandBuffer commandBuffer, uint32_t imageIndex,
                       uint32_t currentFrame)>
        renderCallback,
    std::function<void(VulkanState& vulkanState, int32_t width, int32_t height)> resizeCallback,
    std::function<void(VulkanState& vulkanState)> cleanupCallback) {

    InitWindow(windowTitle, windowWidth, windowHeight);
    InitVulkan(maxFramesInFlight, initCallback);
    MainLoop(renderCallback, updateCallback, resizeCallback);
    Cleanup(cleanupCallback);
}

void VKRenderer::InitWindow(const std::string& windowTitle, const uint32_t windowWidth,
                          const uint32_t windowHeight) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        RUNTIME_ERROR("Unable to initialize SDL!");
    }

    if (SDL_Vulkan_LoadLibrary(NULL)) {
        RUNTIME_ERROR("Unable to initialize Vulkan!");
    }

    window = SDL_CreateWindow(windowTitle.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              windowWidth, windowHeight,
                              SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
}

void VKRenderer::InitVulkan(
    const uint32_t maxFramesInFlight,
    std::function<void(VulkanState& vulkanState, SDL_Window* window, int32_t width, int32_t height)>
        initCallback) {

    CreateInstance();
    SetupDebugMessenger();
    CreateSurface();
    PickPhysicalDevice();
    createLogicalDevice();
    CreateAllocator();

    int32_t width;
    int32_t height;
    SDL_Vulkan_GetDrawableSize(window, &width, &height);

    vulkanState.maxFramesInFlight = maxFramesInFlight;

    initCallback(vulkanState, window, width, height);

    CreateSyncObjects();
}

void VKRenderer::CreateAllocator() {
    VmaVulkanFunctions vkFuncs = {};

    vkFuncs.vkGetInstanceProcAddr = &vkGetInstanceProcAddr;
    vkFuncs.vkGetDeviceProcAddr = &vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo aci = {};
    aci.vulkanApiVersion = VK_API_VERSION_1_2;
    aci.physicalDevice = vulkanState.physicalDevice;
    aci.device = vulkanState.device;
    aci.instance = instance;
    aci.pVulkanFunctions = &vkFuncs;

    vmaCreateAllocator(&aci, &vulkanState.allocator);
}

void VKRenderer::MainLoop(
    std::function<void(VulkanState& vulkanState, VkCommandBuffer commandBuffer, uint32_t imageIndex,
                       uint32_t currentFrame)>
        renderCallback,
    std::function<void(VulkanState& vulkanState)> updateCallback,
    std::function<void(VulkanState& vulkanState, int32_t width, int32_t height)> resizeCallback) {

    bool isRunning = true;
    SDL_Event event;
    while (isRunning) {
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                    framebufferResized = true;
                }

                break;
            case SDL_QUIT:
                isRunning = false;
                break;
            }
        }

        updateCallback(vulkanState);
        DrawFrame(renderCallback, resizeCallback);
    }

    vkDeviceWaitIdle(vulkanState.device);
}

void VKRenderer::WaitWhileMinimized() {
    int32_t width = 0;
    int32_t height = 0;
    SDL_Vulkan_GetDrawableSize(window, &width, &height);
    while (width == 0 || height == 0) {
        SDL_Vulkan_GetDrawableSize(window, &width, &height);
        SDL_WaitEvent(nullptr);
    }
}

void VKRenderer::Cleanup(std::function<void(VulkanState& vulkanState)> cleanupCallback) {
    vulkanState.swapchain.Cleanup(vulkanState.allocator, vulkanState.device);

    cleanupCallback(vulkanState);

    vmaDestroyAllocator(vulkanState.allocator);

    for (size_t i = 0; i < vulkanState.maxFramesInFlight; i++) {
        vkDestroySemaphore(vulkanState.device, renderFinishedSemaphores[i], nullptr);
        vkDestroySemaphore(vulkanState.device, imageAvailableSemaphores[i], nullptr);
        vkDestroyFence(vulkanState.device, inFlightFences[i], nullptr);
    }

    vulkanState.commands.Destroy(vulkanState.device);

    vkDestroyDevice(vulkanState.device, nullptr);

    if (enableValidationLayers) {
        DestroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
    }

    vkDestroySurfaceKHR(instance, vulkanState.surface, nullptr);
    vkDestroyInstance(instance, nullptr);

    SDL_DestroyWindow(window);
    SDL_Vulkan_UnloadLibrary();
    SDL_Quit();
}

void VKRenderer::CreateInstance() {
    if (enableValidationLayers && !CheckValidationLayerSupport()) {
        RUNTIME_ERROR("Validation layers requested, but not available!");
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Hello Triangle";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "No Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    auto extensions = GetRequiredExtensions();
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
    if (enableValidationLayers) {
        createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
        createInfo.ppEnabledLayerNames = validationLayers.data();

        PopulateDebugMessengerCreateInfo(debugCreateInfo);
        createInfo.pNext = (VkDebugUtilsMessengerCreateInfoEXT*)&debugCreateInfo;
    } else {
        createInfo.enabledLayerCount = 0;

        createInfo.pNext = nullptr;
    }

    if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
        RUNTIME_ERROR("Failed to create instance!");
    }
}

void VKRenderer::PopulateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo) {
    createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = DebugCallback;
}

void VKRenderer::SetupDebugMessenger() {
    if (!enableValidationLayers)
        return;

    VkDebugUtilsMessengerCreateInfoEXT createInfo;
    PopulateDebugMessengerCreateInfo(createInfo);

    if (CreateDebugUtilsMessengerEXT(instance, &createInfo, nullptr, &debugMessenger) !=
        VK_SUCCESS) {
        RUNTIME_ERROR("Failed to set up debug messenger!");
    }
}

void VKRenderer::CreateSurface() {
    if (!SDL_Vulkan_CreateSurface(window, instance, &vulkanState.surface)) {
        RUNTIME_ERROR("Failed to create window surface!");
    }
}

void VKRenderer::PickPhysicalDevice() {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);

    if (deviceCount == 0) {
        RUNTIME_ERROR("Failed to find GPUs with Vulkan support!");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

    for (const auto& device : devices) {
        if (IsDeviceSuitable(device)) {
            vulkanState.physicalDevice = device;
            break;
        }
    }

    if (vulkanState.physicalDevice == VK_NULL_HANDLE) {
        RUNTIME_ERROR("Failed to find a suitable GPU!");
    }
}

void VKRenderer::createLogicalDevice() {
    QueueFamilyIndices indices =
        QueueFamilyIndices::FindQueueFamilies(vulkanState.physicalDevice, vulkanState.surface);

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    std::set<uint32_t> uniqueQueueFamilies = {indices.graphicsFamily.value(),
                                              indices.presentFamily.value()};

    float queuePriority = 1.0f;
    for (uint32_t queueFamily : uniqueQueueFamilies) {
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queueFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    VkPhysicalDeviceFeatures deviceFeatures{};
    deviceFeatures.samplerAnisotropy = VK_TRUE;
    deviceFeatures.sampleRateShading = VK_TRUE;

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;

    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();

    createInfo.pEnabledFeatures = &deviceFeatures;

    createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    createInfo.ppEnabledExtensionNames = deviceExtensions.data();

    if (enableValidationLayers) {
        createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
        createInfo.ppEnabledLayerNames = validationLayers.data();
    } else {
        createInfo.enabledLayerCount = 0;
    }

    if (vkCreateDevice(vulkanState.physicalDevice, &createInfo, nullptr, &vulkanState.device) !=
        VK_SUCCESS) {
        RUNTIME_ERROR("Failed to create logical device!");
    }

    vkGetDeviceQueue(vulkanState.device, indices.graphicsFamily.value(), 0,
                     &vulkanState.graphicsQueue);
    vkGetDeviceQueue(vulkanState.device, indices.presentFamily.value(), 0, &presentQueue);
}

bool VKRenderer::HasStencilComponent(VkFormat format) {
    return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT;
}

uint32_t VKRenderer::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(vulkanState.physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    RUNTIME_ERROR("Failed to find suitable memory type!");
}

void VKRenderer::CreateSyncObjects() {
    imageAvailableSemaphores.resize(vulkanState.maxFramesInFlight);
    renderFinishedSemaphores.resize(vulkanState.maxFramesInFlight);
    inFlightFences.resize(vulkanState.maxFramesInFlight);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (size_t i = 0; i < vulkanState.maxFramesInFlight; i++) {
        if (vkCreateSemaphore(vulkanState.device, &semaphoreInfo, nullptr,
                              &imageAvailableSemaphores[i]) != VK_SUCCESS ||
            vkCreateSemaphore(vulkanState.device, &semaphoreInfo, nullptr,
                              &renderFinishedSemaphores[i]) != VK_SUCCESS ||
            vkCreateFence(vulkanState.device, &fenceInfo, nullptr, &inFlightFences[i]) !=
                VK_SUCCESS) {
            RUNTIME_ERROR("Failed to create synchronization objects for a frame!");
        }
    }
}

void VKRenderer::DrawFrame(
    std::function<void(VulkanState& vulkanState, VkCommandBuffer commandBuffer, uint32_t imageIndex,
                       uint32_t currentFrame)>
        renderCallback,
    std::function<void(VulkanState& vulkanState, int32_t width, int32_t height)> resizeCallback) {

    vkWaitForFences(vulkanState.device, 1, &inFlightFences[currentFrame], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex;
    VkResult result = vulkanState.swapchain.GetNextImage(
        vulkanState.device, imageAvailableSemaphores[currentFrame], imageIndex);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        WaitWhileMinimized();
        int32_t width;
        int32_t height;
        SDL_Vulkan_GetDrawableSize(window, &width, &height);
        vulkanState.swapchain.Recreate(vulkanState.allocator, vulkanState.device,
                                       vulkanState.physicalDevice, vulkanState.surface, width,
                                       height);
        resizeCallback(vulkanState, width, height);
        return;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        RUNTIME_ERROR("Failed to acquire swap chain image!");
    }

    vkResetFences(vulkanState.device, 1, &inFlightFences[currentFrame]);

    vulkanState.commands.ResetBuffer(imageIndex, currentFrame);
    const VkCommandBuffer& currentBuffer = vulkanState.commands.GetBuffer(currentFrame);
    renderCallback(vulkanState, currentBuffer, imageIndex, currentFrame);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore waitSemaphores[] = {imageAvailableSemaphores[currentFrame]};
    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;

    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &currentBuffer;

    VkSemaphore signalSemaphores[] = {renderFinishedSemaphores[currentFrame]};
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    if (vkQueueSubmit(vulkanState.graphicsQueue, 1, &submitInfo, inFlightFences[currentFrame]) !=
        VK_SUCCESS) {
        RUNTIME_ERROR("Failed to submit draw command buffer!");
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;

    VkSwapchainKHR swapChains[] = {vulkanState.swapchain.GetSwapchain()};
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapChains;

    presentInfo.pImageIndices = &imageIndex;

    result = vkQueuePresentKHR(presentQueue, &presentInfo);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || framebufferResized) {
        framebufferResized = false;
        WaitWhileMinimized();
        int32_t width;
        int32_t height;
        SDL_Vulkan_GetDrawableSize(window, &width, &height);
        vulkanState.swapchain.Recreate(vulkanState.allocator, vulkanState.device,
                                       vulkanState.physicalDevice, vulkanState.surface, width,
                                       height);
        resizeCallback(vulkanState, width, height);
    } else if (result != VK_SUCCESS) {
        RUNTIME_ERROR("Failed to present swap chain image!");
    }

    currentFrame = (currentFrame + 1) % vulkanState.maxFramesInFlight;
}

bool VKRenderer::IsDeviceSuitable(VkPhysicalDevice device) {
    QueueFamilyIndices indices = QueueFamilyIndices::FindQueueFamilies(device, vulkanState.surface);

    bool extensionsSupported = CheckDeviceExtensionSupport(device);

    bool swapChainAdequate = false;
    if (extensionsSupported) {
        SwapchainSupportDetails swapchainSupport =
            vulkanState.swapchain.QuerySupport(device, vulkanState.surface);
        swapChainAdequate =
            !swapchainSupport.formats.empty() && !swapchainSupport.presentModes.empty();
    }

    VkPhysicalDeviceFeatures supportedFeatures;
    vkGetPhysicalDeviceFeatures(device, &supportedFeatures);

    return indices.IsComplete() && extensionsSupported && swapChainAdequate &&
           supportedFeatures.samplerAnisotropy;
}

bool VKRenderer::CheckDeviceExtensionSupport(VkPhysicalDevice device) {
    uint32_t extensionCount;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount,
                                         availableExtensions.data());

    std::set<std::string> requiredExtensions(deviceExtensions.begin(), deviceExtensions.end());

    for (const auto& extension : availableExtensions) {
        requiredExtensions.erase(extension.extensionName);
    }

    return requiredExtensions.empty();
}

std::vector<const char*> VKRenderer::GetRequiredExtensions() {
    uint32_t extensionCount = 0;
    std::vector<const char*> extensions;
    if (!SDL_Vulkan_GetInstanceExtensions(window, &extensionCount, nullptr)) {
        RUNTIME_ERROR("Unable to get Vulkan extensions!");
    }
    extensions.resize(extensionCount);
    if (!SDL_Vulkan_GetInstanceExtensions(window, &extensionCount, &extensions[0])) {
        RUNTIME_ERROR("Unable to get Vulkan extensions!");
    }

    if (enableValidationLayers) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    return extensions;
}

bool VKRenderer::CheckValidationLayerSupport() {
    uint32_t layerCount;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

    std::vector<VkLayerProperties> availableLayers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

    for (const char* layerName : validationLayers) {
        bool layerFound = false;

        for (const auto& layerProperties : availableLayers) {
            if (strcmp(layerName, layerProperties.layerName) == 0) {
                layerFound = true;
                break;
            }
        }

        if (!layerFound) {
            return false;
        }
    }

    return true;
}

VKAPI_ATTR VkBool32 VKAPI_CALL VKRenderer::DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData) {
    std::cerr << "Validation layer: " << pCallbackData->pMessage << std::endl;

    return VK_FALSE;
}