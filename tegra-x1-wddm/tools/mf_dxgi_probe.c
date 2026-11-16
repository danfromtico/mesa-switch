/* SPDX-License-Identifier: MIT
 *
 * Standard Media Foundation / D3D11VA capability audit for the Switch WO
 * ARM64 build.  It uses only documented MF and D3D11 APIs; it does not
 * register a custom MFT and never reports a decoded frame from a successful
 * object creation.  Run it elevated/on the target to see whether the OS
 * exposes a hardware H.264 decoder to the standard media path.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <stdio.h>
#include <string.h>

static ID3D11Device *g_device;
static IMFDXGIDeviceManager *g_manager;

static const char *hr_text(HRESULT hr)
{
    static char text[32];
    snprintf(text, sizeof(text), "0x%08lx", (unsigned long)hr);
    return text;
}

static int audit_d3d11_video(void)
{
    ID3D11Device *device = NULL;
    ID3D11VideoDevice *video = NULL;
    D3D_FEATURE_LEVEL level = 0;
    HRESULT hr;
    UINT count = 0, i;
    int found_nv12 = 0;

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                           D3D11_CREATE_DEVICE_VIDEO_SUPPORT |
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                           NULL, 0, D3D11_SDK_VERSION, &device, &level, NULL);
    if (FAILED(hr)) {
        printf("D3D11_DEVICE=unavailable (%s)\n", hr_text(hr));
        return 0;
    }
    printf("D3D11_DEVICE=available feature_level=0x%x\n", (unsigned)level);
    {
        IDXGIDevice *dxgi = NULL;
        IDXGIAdapter *adapter = NULL;
        DXGI_ADAPTER_DESC desc;
        if (SUCCEEDED(ID3D11Device_QueryInterface(device, &IID_IDXGIDevice,
                                                  (void **)&dxgi)) && dxgi) {
            if (SUCCEEDED(IDXGIDevice_GetAdapter(dxgi, &adapter)) && adapter &&
                SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &desc))) {
                printf("D3D11_ADAPTER vendor=0x%04x device=0x%04x desc=\"%ls\"\n",
                       desc.VendorId, desc.DeviceId, desc.Description);
                printf("D3D11_ADAPTER_IS_NVIDIA=%s\n",
                       desc.VendorId == 0x10de ? "yes" : "no");
            }
            if (adapter) IDXGIAdapter_Release(adapter);
            IDXGIDevice_Release(dxgi);
        }
    }
    g_device = device;
    {
        UINT token = 0;
        HRESULT mhr = MFCreateDXGIDeviceManager(&token, &g_manager);
        if (SUCCEEDED(mhr) && g_manager) {
            mhr = IMFDXGIDeviceManager_ResetDevice(g_manager, (IUnknown *)device, token);
            printf("MF_DXGI_DEVICE_MANAGER=%s token=%u\n",
                   SUCCEEDED(mhr) ? "available" : "reset-failed", token);
        } else {
            printf("MF_DXGI_DEVICE_MANAGER=unavailable (%s)\n", hr_text(mhr));
        }
    }
    hr = ID3D11Device_QueryInterface(device, &IID_ID3D11VideoDevice, (void **)&video);
    if (FAILED(hr) || !video) {
        printf("D3D11_VIDEO_DEVICE=unavailable (%s)\n", hr_text(hr));
        return 0;
    }
    count = ID3D11VideoDevice_GetVideoDecoderProfileCount(video);
    printf("D3D11_VIDEO_DECODER_PROFILES=%u\n", count);
    for (i = 0; i < count; i++) {
        GUID profile;
        BOOL nv12 = FALSE;
        if (FAILED(ID3D11VideoDevice_GetVideoDecoderProfile(video, i, &profile)))
            continue;
        if (FAILED(ID3D11VideoDevice_CheckVideoDecoderFormat(video, &profile,
                                                             DXGI_FORMAT_NV12,
                                                             &nv12)))
            nv12 = FALSE;
        if (IsEqualGUID(&profile, &D3D11_DECODER_PROFILE_H264_VLD_NOFGT) && nv12)
            found_nv12 = 1;
        printf("  profile[%u]=%08lx-%04x-%04x nv12=%u\n", i,
               (unsigned long)profile.Data1, profile.Data2, profile.Data3,
               nv12 ? 1u : 0u);
    }
    printf("D3D11VA_H264_NV12=%s\n", found_nv12 ? "enumerated" : "absent");
    ID3D11VideoDevice_Release(video);
    return found_nv12;
}

static void audit_mfts(void)
{
    MFT_REGISTER_TYPE_INFO in = { MFMediaType_Video, MFVideoFormat_H264 };
    MFT_REGISTER_TYPE_INFO out = { MFMediaType_Video, MFVideoFormat_NV12 };
    IMFActivate **activate = NULL;
    UINT32 count = 0, i;
    HRESULT hr;

    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                   MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                   &in, &out, &activate, &count);
    if (FAILED(hr)) {
        printf("MF_H264_ENUM=failed (%s)\n", hr_text(hr));
        return;
    }
    printf("MF_H264_HARDWARE_DECODERS=%u\n", count);
    for (i = 0; i < count; i++) {
        IMFAttributes *attr = NULL;
        IMFTransform *transform = NULL;
        UINT32 aware = 0;
        UINT32 async = 0;
        printf("  mft[%u]:\n", i);
        if (SUCCEEDED(IUnknown_QueryInterface((IUnknown *)activate[i], &IID_IMFAttributes,
                                              (void **)&attr)) && attr) {
            IMFAttributes_GetUINT32(attr, &MF_SA_D3D11_AWARE, &aware);
            IMFAttributes_GetUINT32(attr, &MF_TRANSFORM_ASYNC, &async);
            printf("    MF_SA_D3D11_AWARE=%u MF_TRANSFORM_ASYNC=%u\n", aware, async);
            IMFAttributes_Release(attr);
        }
        if (SUCCEEDED(IMFActivate_ActivateObject(activate[i], &IID_IMFTransform,
                                                 (void **)&transform)) && transform) {
            MFT_OUTPUT_STREAM_INFO info = {0};
            DWORD status = 0;
            /* A D3D manager is required for a D3D11-aware MFT.  The caller
             * supplies one through the standard message below; this audit
             * only verifies the message is accepted. */
            HRESULT msg = IMFTransform_ProcessMessage(transform,
                MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)g_manager);
            HRESULT stream = IMFTransform_GetOutputStreamInfo(transform, 0, &info);
            printf("    SET_D3D_MANAGER(%s)=%s output_info=%s flags=0x%lx\n",
                   g_manager ? "manager" : "null",
                   hr_text(msg), hr_text(stream), (unsigned long)info.dwFlags);
            (void)status;
            IMFTransform_Release(transform);
        }
        IMFActivate_Release(activate[i]);
    }
    if (activate)
        CoTaskMemFree(activate);
}

int main(void)
{
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) {
        printf("MF_STARTUP=failed (%s)\n", hr_text(hr));
        return 3;
    }
    printf("MF_STARTUP=ok\n");
    (void)audit_d3d11_video();
    audit_mfts();
    printf("MF_FRAME_DECODE=not-tested (object/profile enumeration is not a decoded frame)\n");
    if (g_manager) { IMFDXGIDeviceManager_Release(g_manager); g_manager = NULL; }
    if (g_device) { ID3D11Device_Release(g_device); g_device = NULL; }
    MFShutdown();
    return 0;
}
