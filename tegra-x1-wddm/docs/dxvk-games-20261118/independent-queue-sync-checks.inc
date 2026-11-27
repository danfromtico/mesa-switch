static const char *skip_sync_label;
static unsigned proofIdleCalls, proofViews, proofDeviceCloses;
static bool advanceWait;
static VkResult idleResult;

static void cross_queue_observations()
{
   Adapter adapter;
   for (bool advance : {true, false}) {
      reset();
#if OWNED_DEVICE_STORAGE
      Device dev; init_device(dev, &adapter);
#else
      Device dev = make_device(&adapter);
#endif
      dev.nvk = true;
      dev.submitted = 7;
      completedValue = 6; advanceWait = advance;
      dev.runtimeFenceValue = &completedValue;
      dev.vk.WaitSemaphores = [](VkDevice, const VkSemaphoreWaitInfo *wait, uint64_t) {
         CHECK(wait->semaphoreCount == 1 && *wait->pValues == 7);
         if (advanceWait) completedValue = 7;
         return VK_SUCCESS;
      };
      CHECK(!dev.runtimeQueue && !dev.runtimeQueueAccepted);
      const HRESULT status = wait_sequence(&dev, 7, TGD3D_WAIT_DESTROY);
      std::printf("SYNC_CHECK %s wait advance=%d target=7 observed=%llu returned=%08lx device=%08lx\n",
                  skip_sync_label, int(advance), static_cast<unsigned long long>(completedValue),
                  static_cast<unsigned long>(status), static_cast<unsigned long>(dev.lost));
   }
   for (int scenario : {0, 1, 2}) {
      reset();
#if OWNED_DEVICE_STORAGE
      auto *dev = new Device(); init_device(*dev, &adapter); DeviceSlot slot{dev};
      D3D10DDI_HDEVICE handleDevice = {&slot};
#else
      alignas(Device) unsigned char storage[sizeof(Device)];
      auto *dev = new (storage) Device(make_device(&adapter));
      D3D10DDI_HDEVICE handleDevice = {dev};
#endif
      dev->nvk = true; dev->submitted = 3; dev->cmdPool = handle<VkCommandPool>(8);
      dev->lost = DXGI_ERROR_DEVICE_REMOVED;
      uint64_t actual = scenario == 2 ? 2 : 3;
      dev->runtimeFenceValue = &actual;
      dev->submittedCommands.push_back({handle<VkCommandBuffer>(9), 3, {handle<VkDescriptorPool>(10)}});
      defer(dev, DeferredType::ImageView, 11, 3);
      proofIdleCalls = proofViews = proofDeviceCloses = 0;
      idleResult = scenario == 1 ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
      dev->vk.DeviceWaitIdle = [](VkDevice) { ++proofIdleCalls; return idleResult; };
      dev->vk.DestroyImageView = [](VkDevice, VkImageView, const VkAllocationCallbacks *) { ++proofViews; };
      dev->vk.DestroyDevice = [](VkDevice, const VkAllocationCallbacks *) { ++proofDeviceCloses; };
      dev->kt.pfnDestroyContextCb = [](HANDLE, const D3DDDICB_DESTROYCONTEXT *) -> HRESULT { return S_OK; };
      D3D10DDI_DEVICEFUNCS functions = {}; device_fill_table(&functions);
      CHECK(!dev->runtimeQueue && !dev->runtimeQueueAccepted);
      functions.pfnDestroyDevice(handleDevice);
      bool retained = false;
#if OWNED_DEVICE_STORAGE
      if (adapter.undrainedDevices == dev) {
         retained = !dev->deferred.empty();
         adapter.undrainedDevices = nullptr; delete dev;
      }
#endif
      std::printf("SYNC_CHECK %s teardown scenario=%d target=3 observed=%llu idle_result=%d "
                  "idle_calls=%u image_views_destroyed=%u devices_destroyed=%u retained=%d\n",
                  skip_sync_label, scenario, static_cast<unsigned long long>(actual), int(idleResult),
                  proofIdleCalls, proofViews, proofDeviceCloses, int(retained));
   }
}
