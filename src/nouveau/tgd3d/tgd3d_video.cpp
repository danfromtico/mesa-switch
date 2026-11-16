/* SPDX-License-Identifier: MIT
 *
 * D3D11VA H.264 decode DDIs on the T210 NVDEC probe backend.
 *
 * IMPORTANT: this table is not advertised through
 * D3D10DDIARG_CREATEDEVICE::ppfnRetrieveSubObject yet.  The shipped tgd3d
 * device still exposes only D3D10.0/10.0.x/10.0.7, so no application can
 * discover or call this path.  It becomes reachable only after the modern
 * D3D11 device table and its required DDIs are enabled and hardware-tested.
 * Capability queries check the probe's firmware-ready state. This transport
 * is incomplete and does not represent production video support.
 */
#include "tgd3d.h"
#include "tgd3d_csc.h"
#include "tgd3d_h264_map.h"
#include "../nvdec/nvdec.h"
#include "../../../tegra-x1-wddm/windows/nvdec_control.h"

#include "tgd3d_dxva_h264.h"
#include <string.h>
#include <vector>

namespace tgd3d {

#define TGD3D_VIDEO_EXT_LOAD_FIRMWARE 1u

static const GUID h264_profile = {
    0x1b81be68, 0xa0c7, 0x11d3, {0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5}
};

static const GUID no_encrypt = {
    0x1b81bed0, 0xa0c7, 0x11d3, {0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5}
};

static bool decoder_desc_supported(const D3D11_1DDI_VIDEO_DECODER_DESC *desc)
{
    return desc && IsEqualGUID(desc->Guid, h264_profile) &&
           desc->OutputFormat == DXGI_FORMAT_NV12 &&
           desc->SampleWidth >= 16 && desc->SampleHeight >= 16 &&
           desc->SampleWidth <= 1280 && desc->SampleHeight <= 720 &&
           !(desc->SampleWidth & 1u) && !(desc->SampleHeight & 1u);
}

static bool decoder_config_supported(const D3D11_1DDI_VIDEO_DECODER_CONFIG &c)
{
    /* AVC short-slice buffers require raw=2. Raw=1 uses the long structure. */
    return c.ConfigBitstreamRaw == 2 && c.ConfigMinRenderTargetBuffCount == 1 &&
           IsEqualGUID(c.guidConfigBitstreamEncryption, no_encrypt) &&
           IsEqualGUID(c.guidConfigMBcontrolEncryption, no_encrypt) &&
           IsEqualGUID(c.guidConfigResidDiffEncryption, no_encrypt) &&
           !(c.ConfigMBcontrolRasterOrder | c.ConfigResidDiffHost |
             c.ConfigSpatialResid8 | c.ConfigResid8Subtraction |
             c.ConfigSpatialHost8or9Clipping | c.ConfigSpatialResidInterleaved |
             c.ConfigIntraResidUnsigned | c.ConfigResidDiffAccelerator |
             c.ConfigHostInverseScan | c.ConfigSpecificIDCT |
             c.Config4GroupedCoefs | c.ConfigDecoderSpecific);
}

struct VideoDecoder {
    Device *dev;
    D3D11_1DDI_VIDEO_DECODER_DESC desc;
    D3D11_1DDI_VIDEO_DECODER_CONFIG config;
    HANDLE probe;
    bool configured;
    uint32_t pitch, aligned_height, frame_size;
    tgd3d_dxva_pic_h264 dxva_pic;
    tgd3d_dxva_qmatrix_h264 qmatrix;
    bool have_qmatrix;
    std::vector<tgd3d_dxva_slice_h264_short> slices;
    std::vector<uint8_t> bitstream;
    struct OutputView *output;
};

struct OutputView {
    Device *dev;
    Resource *res;
    UINT mip, first_array, array_size;
};

static bool probe_open(HANDLE *out)
{
    HANDLE h = CreateFileA("\\\\.\\TegraGpuProbe", GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    *out = h;
    return true;
}

static bool probe_ready()
{
    HANDLE h = INVALID_HANDLE_VALUE;
    struct tg_nvdec_status_result status;
    DWORD bytes = 0;
    bool ready;

    if (!probe_open(&h))
        return false;
    memset(&status, 0, sizeof(status));
    ready = DeviceIoControl(h, TG_GPU_IOCTL_NVDEC_STATUS, nullptr, 0,
                            &status, sizeof(status), &bytes, nullptr) &&
            status.firmware_ready;
    CloseHandle(h);
    return ready;
}

static bool resource_cpu(Resource *res, uint32_t offset, uint32_t size,
                         const uint8_t **out, uint32_t *stride)
{
    if (!res || !res->slice.cpu())
        return false;
    if (offset > res->size || size > res->size - offset)
        return false;
    if (res->kind == ResKind::Buffer) {
        *out = res->slice.cpu() + offset;
        *stride = 0;
        return true;
    }
    if (res->kind == ResKind::Staging && !res->layout.empty()) {
        if (offset > res->slice.size || size > res->slice.size - offset)
            return false;
        *out = res->slice.cpu() + res->layout[0].offset + offset;
        *stride = res->layout[0].rowPitch;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Decoder capability and lifecycle                                     */

static SIZE_T APIENTRY CalcPrivateVideoDecoderSize(D3D10DDI_HDEVICE,
                                                   const D3D11_1DDIARG_CREATEVIDEODECODER *)
{
    return sizeof(VideoDecoder);
}

static HRESULT APIENTRY CreateVideoDecoder(D3D10DDI_HDEVICE hDevice,
                                           const D3D11_1DDIARG_CREATEVIDEODECODER *args,
                                           D3D11_1DDI_HDECODE hDecode,
                                           D3D11_1DDI_HRTDECODE)
{
    Device *dev = D(hDevice);
    VideoDecoder *dec;
    HANDLE probe = INVALID_HANDLE_VALUE;

    if (!args || !hDecode.pDrvPrivate)
        return E_INVALIDARG;
    if (!decoder_desc_supported(&args->Desc) ||
        !decoder_config_supported(args->Config))
        return DXGI_ERROR_UNSUPPORTED;
    if (!probe_open(&probe))
        return DXGI_ERROR_UNSUPPORTED;
    dec = new (hDecode.pDrvPrivate) VideoDecoder();
    dec->dev = dev;
    dec->desc = args->Desc;
    dec->config = args->Config;
    dec->probe = probe;
    dec->configured = false;
    dec->pitch = dec->aligned_height = dec->frame_size = 0;
    dec->have_qmatrix = false;
    dec->output = nullptr;
    return S_OK;
}

static VOID APIENTRY DestroyVideoDecoder(D3D10DDI_HDEVICE, D3D11_1DDI_HDECODE hDecode)
{
    VideoDecoder *dec = static_cast<VideoDecoder *>(hDecode.pDrvPrivate);
    if (!dec)
        return;
    if (dec->probe != INVALID_HANDLE_VALUE)
        CloseHandle(dec->probe);
    dec->~VideoDecoder();
}

static HRESULT APIENTRY VideoDecoderExtension(D3D10DDI_HDEVICE,
                                              D3D11_1DDI_HDECODE hDecode,
                                              const D3D11_1DDIARG_VIDEODECODEREXTENSION *args)
{
    VideoDecoder *dec = static_cast<VideoDecoder *>(hDecode.pDrvPrivate);
    if (!dec || !args)
        return E_INVALIDARG;
    if (args->Function == TGD3D_VIDEO_EXT_LOAD_FIRMWARE) {
        if (!args->pPrivateInputData || args->PrivateInputDataSize < TG_NVDEC_SHA256_SIZE)
            return E_INVALIDARG;
        {
            const uint8_t *p = static_cast<const uint8_t *>(args->pPrivateInputData);
            struct tg_nvdec_load_request *req;
            struct tg_nvdec_load_result res;
            const size_t fw_size = args->PrivateInputDataSize - TG_NVDEC_SHA256_SIZE;
            std::vector<uint8_t> in(sizeof(*req) + fw_size);
            DWORD bytes = 0;
            req = reinterpret_cast<struct tg_nvdec_load_request *>(in.data());
            memset(req, 0, sizeof(*req));
            req->version = TG_NVDEC_CONTROL_VERSION;
            req->size = (uint32_t)fw_size;
            memcpy(req->sha256, p, TG_NVDEC_SHA256_SIZE);
            memcpy(in.data() + sizeof(*req), p + TG_NVDEC_SHA256_SIZE, fw_size);
            if (!DeviceIoControl(dec->probe, TG_GPU_IOCTL_NVDEC_LOAD, in.data(),
                                 (DWORD)in.size(), &res, sizeof(res), &bytes, nullptr) ||
                res.result != 0 || !res.booted)
                return DXGI_ERROR_UNSUPPORTED;
        }
        return S_OK;
    }
    return E_NOTIMPL;
}

static HRESULT APIENTRY VideoDecoderBeginFrame(D3D10DDI_HDEVICE,
                                               D3D11_1DDI_HDECODE hDecode,
                                               const D3D11_1DDIARG_VIDEODECODERBEGINFRAME *args)
{
    VideoDecoder *dec = static_cast<VideoDecoder *>(hDecode.pDrvPrivate);
    if (!dec || !args || !args->hOutputView.pDrvPrivate)
        return E_INVALIDARG;
    if (args->pContentKey || args->ContentKeySize)
        return E_INVALIDARG;   /* encrypted decode is not implemented */
    dec->output = static_cast<OutputView *>(args->hOutputView.pDrvPrivate);
    dec->slices.clear();
    dec->bitstream.clear();
    dec->have_qmatrix = false;
    return S_OK;
}

static HRESULT APIENTRY VideoDecoderSubmitBuffers(
    D3D10DDI_HDEVICE, D3D11_1DDI_HDECODE hDecode, UINT count,
    const D3D11_1DDI_VIDEO_DECODER_BUFFER_DESC *buffers)
{
    VideoDecoder *dec = static_cast<VideoDecoder *>(hDecode.pDrvPrivate);
    UINT i;

    if (!dec || (!buffers && count))
        return E_INVALIDARG;
    for (i = 0; i < count; i++) {
        const D3D11_1DDI_VIDEO_DECODER_BUFFER_DESC *b = &buffers[i];
        Resource *res = R(b->hResource);
        const uint8_t *p = nullptr;
        uint32_t stride = 0;

        if (!resource_cpu(res, b->DataOffset, b->DataSize, &p, &stride))
            return E_NOTIMPL;   /* only CPU-visible bring-up buffers today */
        switch (b->BufferType) {
        case D3D11_1DDI_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS:
            if (b->DataSize < sizeof(tgd3d_dxva_pic_h264))
                return E_INVALIDARG;
            memcpy(&dec->dxva_pic, p, sizeof(dec->dxva_pic));
            break;
        case D3D11_1DDI_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX:
            if (b->DataSize < sizeof(tgd3d_dxva_qmatrix_h264))
                return E_INVALIDARG;
            memcpy(&dec->qmatrix, p, sizeof(dec->qmatrix));
            dec->have_qmatrix = true;
            break;
        case D3D11_1DDI_VIDEO_DECODER_BUFFER_SLICE_CONTROL: {
            const size_t n = b->DataSize / sizeof(tgd3d_dxva_slice_h264_short);
            if (!n)
                return E_INVALIDARG;
            dec->slices.assign(reinterpret_cast<const tgd3d_dxva_slice_h264_short *>(p),
                               reinterpret_cast<const tgd3d_dxva_slice_h264_short *>(p) + n);
            break;
        }
        case D3D11_1DDI_VIDEO_DECODER_BUFFER_BITSTREAM:
            dec->bitstream.assign(p, p + b->DataSize);
            break;
        default:
            return E_NOTIMPL;   /* macroblock/residual off-host buffers */
        }
    }
    return S_OK;
}

static HRESULT write_nv12(Resource *res, const uint8_t *nv12, uint32_t width,
                          uint32_t height, uint32_t pitch, uint32_t aligned_height)
{
    uint8_t *dst;
    uint32_t row, chroma_height = height / 2;

    if (!res || res->kind != ResKind::Staging || res->layout.empty() ||
        !res->slice.cpu() || res->format != DXGI_FORMAT_NV12)
        return E_NOTIMPL;
    dst = res->slice.cpu() + res->layout[0].offset;
    if (res->layout[0].rowPitch < width)
        return E_INVALIDARG;
    for (row = 0; row < height; row++)
        memcpy(dst + (size_t)row * res->layout[0].rowPitch,
               nv12 + (size_t)row * pitch, width);
    dst += (size_t)res->layout[0].rowPitch * aligned_height;
    for (row = 0; row < chroma_height; row++)
        memcpy(dst + (size_t)row * res->layout[0].rowPitch,
               nv12 + (size_t)pitch * aligned_height + (size_t)row * pitch, width);
    return S_OK;
}

static VOID APIENTRY VideoDecoderEndFrame(D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HDECODE hDecode)
{
    VideoDecoder *dec = static_cast<VideoDecoder *>(hDecode.pDrvPrivate);
    struct tg_nvdec_h264_pic pic;
    std::vector<uint8_t> assembled;
    std::vector<uint32_t> offsets;
    struct tgd3d_h264_slice_in slices[TG_NVDEC_MAX_SLICES];
    struct tg_nvdec_decode_request *req;
    struct tg_nvdec_decode_result *res;
    std::vector<uint8_t> input;
    std::vector<uint8_t> output;
    char err[160] = {0};
    DWORD bytes = 0;
    HRESULT hr;

    if (!dec || !dec->output || !dec->output->res || dec->slices.empty() ||
        dec->bitstream.empty()) {
        TG_ERROR(D(hDevice), E_INVALIDARG);
        return;
    }
    if (tgd3d_h264_map_dxva(&dec->dxva_pic,
                          dec->have_qmatrix ? &dec->qmatrix : nullptr,
                          dec->desc.SampleWidth, dec->desc.SampleHeight,
                          &pic, err, sizeof(err)) != TGD3D_H264_MAP_OK) {
        TG_ERROR(D(hDevice), DXGI_ERROR_UNSUPPORTED);
        return;
    }
    if (dec->slices.size() > TG_NVDEC_MAX_SLICES) {
        TG_ERROR(D(hDevice), E_INVALIDARG);
        return;
    }
    for (size_t i = 0; i < dec->slices.size(); i++) {
        slices[i].offset = dec->slices[i].BSNALunitDataLocation;
        slices[i].size = dec->slices[i].SliceBytesInBuffer;
    }
    assembled.resize(dec->bitstream.size() + dec->slices.size() * 3u + 16u);
    offsets.resize(dec->slices.size() + 1u);
    {
        uint32_t size = 0, count = 0;
        if (tgd3d_h264_assemble_bitstream(dec->bitstream.data(), dec->bitstream.size(),
                                          slices, (uint32_t)dec->slices.size(),
                                          assembled.data(), assembled.size(),
                                          &size, offsets.data(), &count) !=
            TGD3D_H264_MAP_OK) {
            TG_ERROR(D(hDevice), E_INVALIDARG);
            return;
        }
        assembled.resize(size);
    }

    if (!dec->configured) {
        struct tg_nvdec_config_request creq;
        struct tg_nvdec_config_result cres;
        creq.version = TG_NVDEC_CONTROL_VERSION;
        creq.width = dec->desc.SampleWidth;
        creq.height = dec->desc.SampleHeight;
        if (!DeviceIoControl(dec->probe, TG_GPU_IOCTL_NVDEC_CONFIGURE, &creq,
                             sizeof(creq), &cres, sizeof(cres), &bytes, nullptr) ||
            cres.result != 0) {
            TG_ERROR(D(hDevice), DXGI_ERROR_UNSUPPORTED);
            return;
        }
        dec->pitch = cres.pitch;
        dec->aligned_height = cres.aligned_height;
        dec->frame_size = cres.frame_size;
        dec->configured = true;
    }
    pic.pitch_luma = dec->pitch;
    pic.pitch_chroma = dec->pitch;

    input.resize(sizeof(*req) + assembled.size() + offsets.size() * sizeof(uint32_t));
    req = reinterpret_cast<struct tg_nvdec_decode_request *>(input.data());
    memset(req, 0, sizeof(*req));
    req->version = TG_NVDEC_CONTROL_VERSION;
    req->bitstream_size = (uint32_t)assembled.size();
    req->slice_count = (uint32_t)dec->slices.size();
    req->output_size = dec->frame_size;
    req->pic = pic;
    memcpy(input.data() + sizeof(*req), assembled.data(), assembled.size());
    memcpy(input.data() + sizeof(*req) + assembled.size(), offsets.data(),
           offsets.size() * sizeof(uint32_t));

    output.resize(sizeof(*res) + dec->frame_size);
    res = reinterpret_cast<struct tg_nvdec_decode_result *>(output.data());
    if (!DeviceIoControl(dec->probe, TG_GPU_IOCTL_NVDEC_DECODE, input.data(),
                         (DWORD)input.size(), output.data(), (DWORD)output.size(),
                         &bytes, nullptr) || res->result != 0 ||
        res->error_status || res->mbs_in_error ||
        res->expected_syncpoint != res->observed_syncpoint) {
        TG_ERROR(D(hDevice), DXGI_ERROR_UNSUPPORTED);
        return;
    }
    hr = write_nv12(dec->output->res, output.data() + sizeof(*res),
                    dec->desc.SampleWidth, dec->desc.SampleHeight,
                    dec->pitch, dec->aligned_height);
    if (FAILED(hr))
        TG_ERROR(D(hDevice), hr);
}

/* ------------------------------------------------------------------ */
/* Output views                                                         */

static SIZE_T APIENTRY CalcPrivateVideoDecoderOutputViewSize(
    D3D10DDI_HDEVICE, const D3D11_1DDIARG_CREATEVIDEODECODEROUTPUTVIEW *)
{
    return sizeof(OutputView);
}

static HRESULT APIENTRY CreateVideoDecoderOutputView(
    D3D10DDI_HDEVICE hDevice, const D3D11_1DDIARG_CREATEVIDEODECODEROUTPUTVIEW *args,
    D3D11_1DDI_HVIDEODECODEROUTPUTVIEW hView, D3D11_1DDI_HRTVIDEODECODEROUTPUTVIEW)
{
    Device *dev = D(hDevice);
    OutputView *view;
    Resource *res;

    if (!args || !hView.pDrvPrivate)
        return E_INVALIDARG;
    res = R(args->hDrvResource);
    if (!res || res->format != DXGI_FORMAT_NV12)
        return E_INVALIDARG;
    view = new (hView.pDrvPrivate) OutputView();
    view->dev = dev;
    view->res = res;
    view->mip = args->MipSlice;
    view->first_array = args->FirstArraySlice;
    view->array_size = args->ArraySize;
    return S_OK;
}

static VOID APIENTRY DestroyVideoDecoderOutputView(
    D3D10DDI_HDEVICE, D3D11_1DDI_HVIDEODECODEROUTPUTVIEW hView)
{
    OutputView *view = static_cast<OutputView *>(hView.pDrvPrivate);
    if (view)
        view->~OutputView();
}

/* ------------------------------------------------------------------ */
/* Profiles and configurations                                          */

static VOID APIENTRY GetVideoDecoderProfileCount(D3D10DDI_HDEVICE, UINT *count)
{
    *count = probe_ready() ? 1u : 0u;
}

static VOID APIENTRY GetVideoDecoderProfile(D3D10DDI_HDEVICE, UINT index, GUID *profile)
{
    if (!profile)
        return;
    *profile = (index == 0 && probe_ready()) ?
               h264_profile : GUID_NULL;
}

static VOID APIENTRY CheckVideoDecoderFormat(D3D10DDI_HDEVICE, const GUID *profile,
                                             DXGI_FORMAT format, BOOL *supported)
{
    *supported = profile && IsEqualGUID(*profile, h264_profile) &&
                 format == DXGI_FORMAT_NV12 && probe_ready();
}

static VOID APIENTRY GetVideoDecoderConfigCount(
    D3D10DDI_HDEVICE, const D3D11_1DDI_VIDEO_DECODER_DESC *desc, UINT *count)
{
    *count = decoder_desc_supported(desc) &&
             probe_ready() ? 1u : 0u;
}

static VOID APIENTRY GetVideoDecoderConfig(
    D3D10DDI_HDEVICE, const D3D11_1DDI_VIDEO_DECODER_DESC *desc, UINT index,
    D3D11_1DDI_VIDEO_DECODER_CONFIG *config)
{
    if (!config)
        return;
    memset(config, 0, sizeof(*config));
    if (index || !decoder_desc_supported(desc) || !probe_ready())
        return;
    config->guidConfigBitstreamEncryption = no_encrypt;
    config->guidConfigMBcontrolEncryption = no_encrypt;
    config->guidConfigResidDiffEncryption = no_encrypt;
    config->ConfigBitstreamRaw = 2;
    config->ConfigMinRenderTargetBuffCount = 1;
}

static VOID APIENTRY GetVideoDecoderBufferTypeCount(D3D10DDI_HDEVICE,
                                                    const D3D11_1DDI_VIDEO_DECODER_DESC *,
                                                    UINT *count)
{
    *count = 4;
}

static VOID APIENTRY GetVideoDecoderBufferInfo(D3D10DDI_HDEVICE,
                                               const D3D11_1DDI_VIDEO_DECODER_DESC *,
                                               UINT index,
                                               D3D11_1DDI_VIDEO_DECODER_BUFFER_INFO *info)
{
    static const D3D11_1DDI_VIDEO_DECODER_BUFFER_TYPE types[4] = {
        D3D11_1DDI_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS,
        D3D11_1DDI_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX,
        D3D11_1DDI_VIDEO_DECODER_BUFFER_SLICE_CONTROL,
        D3D11_1DDI_VIDEO_DECODER_BUFFER_BITSTREAM,
    };
    if (!info || index >= 4)
        return;
    info->Type = types[index];
    info->Size = 0;      /* variable */
    info->Usage = 0;
}

void video_fill_table(D3D11_1DDI_VIDEODEVICEFUNCS *table)
{
    if (!table)
        return;
    memset(table, 0, sizeof(*table));
    table->pfnGetVideoDecoderProfileCount = GetVideoDecoderProfileCount;
    table->pfnGetVideoDecoderProfile = GetVideoDecoderProfile;
    table->pfnCheckVideoDecoderFormat = CheckVideoDecoderFormat;
    table->pfnGetVideoDecoderConfigCount = GetVideoDecoderConfigCount;
    table->pfnGetVideoDecoderConfig = GetVideoDecoderConfig;
    table->pfnGetVideoDecoderBufferTypeCount = GetVideoDecoderBufferTypeCount;
    table->pfnGetVideoDecoderBufferInfo = GetVideoDecoderBufferInfo;
    table->pfnCalcPrivateVideoDecoderSize = CalcPrivateVideoDecoderSize;
    table->pfnCreateVideoDecoder = CreateVideoDecoder;
    table->pfnDestroyVideoDecoder = DestroyVideoDecoder;
    table->pfnVideoDecoderExtension = VideoDecoderExtension;
    table->pfnVideoDecoderBeginFrame = VideoDecoderBeginFrame;
    table->pfnVideoDecoderSubmitBuffers = VideoDecoderSubmitBuffers;
    table->pfnVideoDecoderEndFrame = VideoDecoderEndFrame;
    table->pfnCalcPrivateVideoDecoderOutputViewSize = CalcPrivateVideoDecoderOutputViewSize;
    table->pfnCreateVideoDecoderOutputView = CreateVideoDecoderOutputView;
    table->pfnDestroyVideoDecoderOutputView = DestroyVideoDecoderOutputView;
    /* Video processor, crypto and capture DDIs are deliberately absent until
     * the GPU CSC pipeline and encrypted-content paths are implemented. */
}

} // namespace tgd3d
