/* SPDX-License-Identifier: MIT
 *
 * D3D11VA H.264 decode on the display KMD's coexisting NVDEC backend.
 * Each decoder owns a real KMT device/session on this adapter's LUID.
 * The backend returns linear NV12 only after actual hardware completion.
 * Failed closes remain attached to the device for cleanup; no probe device,
 * private runtime layout, synthetic fence or fabricated OS handle is used.
 */
#include "tgd3d.h"
#include "tgd3d_csc.h"
#include "tgd3d_h264_map.h"
#include "../nvdec/nvdec.h"
#include "../../../tegra-x1-wddm/windows/nvdec_control.h"
#include "../vulkan/nvkmd/windows/nvdec_native.h"
#include "../vulkan/nvkmd/windows/nvdec_abi.h"

#include "tgd3d_dxva_h264.h"
#include <string.h>
#include <vector>

namespace tgd3d {

static constexpr uint32_t bitstream_capacity = 2u * 1024u * 1024u;

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
           desc->SampleWidth == 1280 && desc->SampleHeight == 720;
}

static bool decoder_config_supported(const D3D11_1DDI_VIDEO_DECODER_CONFIG &c)
{
    /* AVC short-slice buffers require raw=2. Raw=1 uses the long structure. */
    return c.ConfigBitstreamRaw == 2 && c.ConfigMinRenderTargetBuffCount == TG_NVDEC_MAX_SURFACES &&
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

struct VideoSession {
    tg_nvdec_native native = {};
    VideoSession *next = nullptr;
};

struct VideoDecoder {
    Device *dev;
    D3D11_1DDI_VIDEO_DECODER_DESC desc;
    D3D11_1DDI_VIDEO_DECODER_CONFIG config;
    VideoSession *session;
    bool configured;
    uint32_t pitch, aligned_height, frame_size;
    tgd3d_dxva_pic_h264 dxva_pic;
    tgd3d_dxva_qmatrix_h264 qmatrix;
    bool have_qmatrix;
    bool have_pic, in_frame;
    HRESULT frame_error;
    std::vector<tgd3d_dxva_slice_h264_short> slices;
    std::vector<uint8_t> bitstream;
    Slice output;
    SubLayout output_layout;
    UINT output_slot;
};

struct OutputView {
    Device *dev;
    Resource *res;
    UINT mip, first_array, array_size;
};

static HRESULT native_result(LONG status)
{
    return !status ? S_OK : status < 0 ? HRESULT_FROM_NT(status) : E_FAIL;
}

static HRESULT native_call(VideoSession *s, uint32_t command,
                           const void *input, uint32_t input_size,
                           void *output, uint32_t output_size)
{
    trace(TGD3D_TRACE_VIDEO, E_PENDING, command, s->native.session);
    HRESULT hr = native_result(tg_nvdec_native_call(&s->native, command,
                                                   input, input_size, output, output_size));
    trace(TGD3D_TRACE_VIDEO, hr, command, s->native.session);
    return hr;
}

static bool close_session(Device *dev, VideoSession *s)
{
    if (!s) return true;
    HRESULT hr = native_result(tg_nvdec_native_close(&s->native));
    trace(TGD3D_TRACE_VIDEO, hr, 6, s->native.session);
    if (s->native.adapter || s->native.device || s->native.session)
        return false;
    VideoSession **link = &dev->videoSessions;
    while (*link && *link != s) link = &(*link)->next;
    if (*link) *link = s->next;
    delete s;
    return true;
}

bool video_device_destroy(Device *dev)
{
    bool closed = true;
    for (VideoSession *s = dev->videoSessions, *next; s; s = next) {
        next = s->next;
        if (!close_session(dev, s)) closed = false;
    }
    return closed;
}

bool video_device_supported(const Device *dev)
{
    return dev && dev->adapter && dev->adapter->videoEnabled &&
           dev->adapter->videoFirmware.size() == 128000 &&
           dev->interfaceVersion == D3D11_1_DDI_INTERFACE_VERSION;
}

void video_adapter_init(Adapter *adapter)
{
    /* Controlled application opt-in while video integration is being
     * validated. DWM and ordinary applications keep legacy negotiation. */
    wchar_t enabled[2];
    if (GetEnvironmentVariableW(L"TGD3D_VIDEO_VALIDATE", enabled, 2) != 1 || enabled[0] != L'1')
        return;
    tg_gpu_adapter_nvdec_clients_info info = {};
    info.resource.base.version = TG_GPU_ADAPTER_ABI;
    info.resource.base.size = sizeof(tg_gpu_adapter_info);
    D3DDDICB_QUERYADAPTERINFO query = {};
    query.pPrivateDriverData = &info;
    query.PrivateDriverDataSize = sizeof(info);
    HRESULT hr = adapter->callbacks->pfnQueryAdapterInfoCb(adapter->hRTAdapter, &query);
    const auto &r = info.resource.nvdec;
    if (FAILED(hr) || memcmp(&info.resource.base, &adapter->info, sizeof(adapter->info)) ||
        r.version != TG_NVDEC_QUERY_ABI || r.size != sizeof(r) || (r.flags & 3u) != 3u ||
        r.resource_mask != 0xfffu || info.clients.version != TG_NVDEC_CLIENT_QUERY_ABI ||
        info.clients.size != sizeof(info.clients) || (info.clients.flags & 3u) != 3u)
        return;
    HMODULE module = nullptr;
    wchar_t path[32768];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&video_adapter_init), &module)) return;
    DWORD length = GetModuleFileNameW(module, path, ARRAYSIZE(path));
    if (!length || length >= ARRAYSIZE(path)) return;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash || (slash - path) + 11 >= ARRAYSIZE(path)) return;
    wcscpy(slash + 1, L"nvdec.bin");
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size = {};
    std::vector<uint8_t> firmware;
    bool valid = GetFileSizeEx(file, &size) && size.QuadPart == 128000;
    if (valid) {
        try { firmware.resize(128000); } catch (const std::bad_alloc &) { valid = false; }
        DWORD read = 0;
        valid = valid && ReadFile(file, firmware.data(), (DWORD)firmware.size(), &read, nullptr) &&
                read == firmware.size();
    }
    CloseHandle(file);
    if (!valid) return;
    static const uint8_t expected[32] = {
        0x7b,0x5a,0xc5,0xad,0x66,0xda,0xd4,0x7e,0x77,0xf2,0xa0,0x56,0x99,0x1e,0x42,0x2d,
        0xd2,0x63,0x0f,0xa1,0x7e,0x0a,0x57,0xa5,0x87,0x4a,0xf4,0x92,0xef,0xa0,0xf9,0x09};
    uint8_t hash[32];
    tg_nvdec_sha256(firmware.data(), firmware.size(), hash);
    if (memcmp(hash, expected, sizeof(hash))) return;
    adapter->videoFirmware.swap(firmware);
    adapter->videoEnabled = true;
}

static bool resource_cpu(Resource *res, uint32_t offset, uint32_t size,
                         const uint8_t **out, uint32_t *stride)
{
    if (!res || !res->slice || res->bufferMapped || res->kind != ResKind::Buffer)
        return false;
    if (offset > res->size || size > res->size - offset)
        return false;
    if (FAILED(wait_sequence(res->dev, res->sliceLastWrite, TGD3D_WAIT_READBACK)) ||
        FAILED(slice_map(res->dev, res->slice))) return false;
    *out = res->slice.cpu() + offset;
    *stride = 0;
    return true;
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
    if (!dev || !args || !hDecode.pDrvPrivate)
        return E_INVALIDARG;
    if (!video_device_supported(dev) || !decoder_desc_supported(&args->Desc) ||
        !decoder_config_supported(args->Config))
        return DXGI_ERROR_UNSUPPORTED;
    VideoSession *session = new (std::nothrow) VideoSession();
    if (!session) return E_OUTOFMEMORY;
    session->next = dev->videoSessions;
    dev->videoSessions = session;
    HRESULT hr = native_result(tg_nvdec_native_open(&session->native, &dev->adapter->info));
    if (SUCCEEDED(hr)) {
        try {
            const auto &firmware = dev->adapter->videoFirmware;
            std::vector<uint8_t> input(sizeof(tg_nvdec_load_request) + firmware.size());
            auto *req = reinterpret_cast<tg_nvdec_load_request *>(input.data());
            req->version = TG_NVDEC_CONTROL_VERSION;
            req->size = (uint32_t)firmware.size();
            tg_nvdec_sha256(firmware.data(), firmware.size(), req->sha256);
            memcpy(req + 1, firmware.data(), firmware.size());
            tg_nvdec_load_result result = {};
            hr = native_call(session, TG_NVDEC_ESCAPE_LOAD, input.data(), (uint32_t)input.size(),
                             &result, sizeof(result));
            if (SUCCEEDED(hr) && (result.version != TG_NVDEC_CONTROL_VERSION || result.result ||
                !result.booted || result.firmware_bytes != firmware.size() ||
                memcmp(result.sha256, req->sha256, sizeof(req->sha256)))) hr = E_FAIL;
        } catch (const std::bad_alloc &) { hr = E_OUTOFMEMORY; }
    }
    tg_nvdec_config_result config = {};
    if (SUCCEEDED(hr)) {
        const tg_nvdec_config_request request = {TG_NVDEC_CONTROL_VERSION, args->Desc.SampleWidth,
                                                args->Desc.SampleHeight};
        hr = native_call(session, TG_NVDEC_ESCAPE_CONFIGURE, &request, sizeof(request), &config, sizeof(config));
        if (SUCCEEDED(hr) && (config.version != TG_NVDEC_CONTROL_VERSION || config.result ||
            config.pitch != 1280 || config.aligned_height != 720 || config.frame_size != 1382400)) hr = E_FAIL;
    }
    if (FAILED(hr)) {
        close_session(dev, session); /* still-linked node retains a failed cleanup */
        return hr;
    }
    VideoDecoder *dec = new (hDecode.pDrvPrivate) VideoDecoder();
    dec->dev = dev;
    dec->desc = args->Desc;
    dec->config = args->Config;
    dec->session = session;
    dec->configured = true;
    dec->pitch = config.pitch;
    dec->aligned_height = config.aligned_height;
    dec->frame_size = config.frame_size;
    dec->have_qmatrix = false;
    dec->have_pic = dec->in_frame = false;
    dec->frame_error = S_OK;
    dec->output = {};
    return S_OK;
}

static VOID APIENTRY DestroyVideoDecoder(D3D10DDI_HDEVICE, D3D11_1DDI_HDECODE hDecode)
{
    VideoDecoder *dec = static_cast<VideoDecoder *>(hDecode.pDrvPrivate);
    if (!dec)
        return;
    if (dec->output) {
        --dec->output.chunk->videoWrites;
        slice_release(dec->dev, dec->output, 0);
    }
    close_session(dec->dev, dec->session);
    dec->session = nullptr;
    dec->~VideoDecoder();
}

static HRESULT APIENTRY VideoDecoderExtension(D3D10DDI_HDEVICE,
                                              D3D11_1DDI_HDECODE,
                                              const D3D11_1DDIARG_VIDEODECODEREXTENSION *)
{
    /* Firmware belongs to the signed driver package; ordinary DXVA clients
     * never provide a private extension or choose arbitrary firmware bytes. */
    return E_NOTIMPL;
}

static HRESULT APIENTRY VideoDecoderBeginFrame(D3D10DDI_HDEVICE hDevice,
                                               D3D11_1DDI_HDECODE hDecode,
                                               const D3D11_1DDIARG_VIDEODECODERBEGINFRAME *args)
{
    VideoDecoder *dec = static_cast<VideoDecoder *>(hDecode.pDrvPrivate);
    if (!dec || dec->dev != D(hDevice) || dec->in_frame || !dec->session ||
        !args || !args->hOutputView.pDrvPrivate)
        return E_INVALIDARG;
    if (args->pContentKey || args->ContentKeySize)
        return E_INVALIDARG;   /* encrypted decode is not implemented */
    if (FAILED(dec->dev->lost)) return dec->dev->lost;
    OutputView *view = static_cast<OutputView *>(args->hOutputView.pDrvPrivate);
    Resource *res = view->res;
    if (view->dev != dec->dev || !res || res->dev != dec->dev ||
        res->kind != ResKind::Staging || !res->slice || res->format != DXGI_FORMAT_NV12 ||
        res->width != dec->desc.SampleWidth || res->height != dec->desc.SampleHeight ||
        view->mip || view->array_size != 1 || view->first_array >= res->layout.size() ||
        view->first_array >= TG_NVDEC_MAX_SURFACES ||
        !(res->bind & D3D11_DDI_BIND_DECODER)) return E_INVALIDARG;
    for (const auto &map : res->maps) if (map.mapped) return E_INVALIDARG;
    HRESULT hr = wait_sequence(dec->dev, res->sliceLastUse > res->sliceLastWrite ?
        res->sliceLastUse : res->sliceLastWrite, TGD3D_WAIT_READBACK);
    if (SUCCEEDED(hr)) hr = slice_map(dec->dev, res->slice);
    if (FAILED(hr)) return hr;
    /* Hold backing, not runtime-private view/resource memory. Begin/End may
     * span application releases; the slice cannot retire in that interval. */
    dec->output = res->slice;
    ++dec->output.chunk->live;
    ++dec->output.chunk->videoWrites;
    dec->output_layout = res->layout[view->first_array];
    dec->output_slot = view->first_array;
    dec->slices.clear();
    dec->bitstream.clear();
    dec->have_qmatrix = false;
    dec->have_pic = false;
    dec->frame_error = S_OK;
    dec->dxva_pic = {};
    dec->in_frame = true;
    return S_OK;
}

static HRESULT collect_buffers(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HDECODE hDecode, UINT count,
    const D3D11_1DDI_VIDEO_DECODER_BUFFER_DESC *buffers)
{
    VideoDecoder *dec = static_cast<VideoDecoder *>(hDecode.pDrvPrivate);
    UINT i;

    if (!dec || dec->dev != D(hDevice) || !dec->in_frame || (!buffers && count))
        return E_INVALIDARG;
    try {
    for (i = 0; i < count; i++) {
        const D3D11_1DDI_VIDEO_DECODER_BUFFER_DESC *b = &buffers[i];
        if (b->ReservedBits || b->pIV || b->IVSize || b->PartialEncryption ||
            b->EncryptedBlockInfo.NumEncryptedBytesAtBeginning ||
            b->EncryptedBlockInfo.NumBytesInSkipPattern || b->EncryptedBlockInfo.NumBytesInEncryptPattern)
            return E_INVALIDARG;
        Resource *res = R(b->hResource);
        const uint8_t *p = nullptr;
        uint32_t stride = 0;

        if (!res || res->dev != dec->dev || !resource_cpu(res, b->DataOffset, b->DataSize, &p, &stride))
            return FAILED(dec->dev->lost) ? dec->dev->lost : E_INVALIDARG;
        switch (b->BufferType) {
        case D3D11_1DDI_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS:
            if (dec->have_pic || b->DataSize != sizeof(tgd3d_dxva_pic_h264))
                return E_INVALIDARG;
            memcpy(&dec->dxva_pic, p, sizeof(dec->dxva_pic));
            dec->have_pic = true;
            break;
        case D3D11_1DDI_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX:
            if (dec->have_qmatrix || b->DataSize != sizeof(tgd3d_dxva_qmatrix_h264))
                return E_INVALIDARG;
            memcpy(&dec->qmatrix, p, sizeof(dec->qmatrix));
            dec->have_qmatrix = true;
            break;
        case D3D11_1DDI_VIDEO_DECODER_BUFFER_SLICE_CONTROL: {
            const size_t n = b->DataSize / sizeof(tgd3d_dxva_slice_h264_short);
            if (!dec->slices.empty() || !n || n > TG_NVDEC_MAX_SLICES ||
                b->DataSize % sizeof(tgd3d_dxva_slice_h264_short))
                return E_INVALIDARG;
            dec->slices.resize(n);
            memcpy(dec->slices.data(), p, b->DataSize);
            for (const auto &slice : dec->slices)
                if (slice.wBadSliceChopping) return E_INVALIDARG;
            break;
        }
        case D3D11_1DDI_VIDEO_DECODER_BUFFER_BITSTREAM:
            if (!dec->bitstream.empty() || !b->DataSize || b->DataSize > bitstream_capacity)
                return E_INVALIDARG;
            dec->bitstream.assign(p, p + b->DataSize);
            break;
        default:
            return E_NOTIMPL;   /* macroblock/residual off-host buffers */
        }
    }
    } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
    return S_OK;
}

static HRESULT APIENTRY VideoDecoderSubmitBuffers(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HDECODE hDecode, UINT count,
    const D3D11_1DDI_VIDEO_DECODER_BUFFER_DESC *buffers)
{
    auto *dec = static_cast<VideoDecoder *>(hDecode.pDrvPrivate);
    if (!dec || dec->dev != D(hDevice) || !dec->in_frame) return E_INVALIDARG;
    if (FAILED(dec->frame_error)) return dec->frame_error;
    return dec->frame_error = collect_buffers(hDevice, hDecode, count, buffers);
}

static HRESULT write_nv12(VideoDecoder *dec, const uint8_t *nv12, uint32_t width,
                          uint32_t height, uint32_t pitch, uint32_t aligned_height)
{
    uint8_t *dst;
    uint32_t row, chroma_height = height / 2;

    const auto &layout = dec->output_layout;
    if (!dec->output || !dec->output.cpu() || layout.rowPitch < width ||
        layout.width != width || layout.height != height ||
        layout.depthPitch < uint64_t(layout.rowPitch) * height * 3 / 2 ||
        layout.offset > dec->output.size || layout.depthPitch > dec->output.size - layout.offset)
        return E_INVALIDARG;
    dst = dec->output.cpu() + layout.offset;
    for (row = 0; row < height; row++)
        memcpy(dst + (size_t)row * layout.rowPitch,
               nv12 + (size_t)row * pitch, width);
    dst += (size_t)layout.rowPitch * height;
    for (row = 0; row < chroma_height; row++)
        memcpy(dst + (size_t)row * layout.rowPitch,
               nv12 + (size_t)pitch * aligned_height + (size_t)row * pitch, width);
    return S_OK;
}

static HRESULT finish_frame(VideoDecoder *dec)
{
    if (FAILED(dec->frame_error)) return dec->frame_error;
    if (!dec->output || !dec->configured || !dec->have_pic || !dec->have_qmatrix ||
        dec->slices.empty() || dec->bitstream.empty()) return E_INVALIDARG;
    tg_nvdec_h264_pic pic = {};
    char error[TG_NVDEC_ERROR_SIZE] = {};
    if (tgd3d_h264_map_dxva(&dec->dxva_pic, &dec->qmatrix,
                          dec->desc.SampleWidth, dec->desc.SampleHeight,
                          &pic, error, sizeof(error)) != TGD3D_H264_MAP_OK) {
        trace_message(error);
        return DXGI_ERROR_UNSUPPORTED;
    }
    if (pic.curr_slot != dec->output_slot) return E_INVALIDARG;
    pic.pitch_luma = pic.pitch_chroma = dec->pitch;
    tgd3d_h264_slice_in slices[TG_NVDEC_MAX_SLICES];
    for (size_t i = 0; i < dec->slices.size(); ++i) {
        slices[i].offset = dec->slices[i].BSNALunitDataLocation;
        slices[i].size = dec->slices[i].SliceBytesInBuffer;
    }
    std::vector<uint8_t> assembled(dec->bitstream.size() + dec->slices.size() * 3 + 16);
    std::vector<uint32_t> offsets(dec->slices.size() + 1);
    uint32_t bytes = 0, count = 0;
    if (tgd3d_h264_assemble_bitstream(dec->bitstream.data(), dec->bitstream.size(),
            slices, (uint32_t)dec->slices.size(), assembled.data(), assembled.size(),
            &bytes, offsets.data(), &count) != TGD3D_H264_MAP_OK || count != dec->slices.size())
        return E_INVALIDARG;
    std::vector<uint8_t> input(sizeof(tg_nvdec_decode_request) + bytes + offsets.size() * sizeof(uint32_t));
    auto *request = reinterpret_cast<tg_nvdec_decode_request *>(input.data());
    request->version = TG_NVDEC_CONTROL_VERSION;
    request->bitstream_size = bytes;
    request->slice_count = count;
    request->output_size = dec->frame_size;
    request->pic = pic;
    memcpy(request + 1, assembled.data(), bytes);
    memcpy(input.data() + sizeof(*request) + bytes, offsets.data(), offsets.size() * sizeof(uint32_t));
    std::vector<uint8_t> output(sizeof(tg_nvdec_decode_result) + dec->frame_size);
    auto *result = reinterpret_cast<tg_nvdec_decode_result *>(output.data());
    HRESULT hr = native_call(dec->session, TG_NVDEC_ESCAPE_DECODE, input.data(), (uint32_t)input.size(),
                             output.data(), (uint32_t)output.size());
    trace(TGD3D_TRACE_VIDEO, hr, 7,
          uint64_t(result->expected_syncpoint) << 32 | result->observed_syncpoint);
    if (FAILED(hr)) {
        result->error[sizeof(result->error)-1] = 0;
        trace_message(result->error);
        return hr;
    }
    if (result->version != TG_NVDEC_CONTROL_VERSION || result->result ||
        result->error_status || result->mbs_in_error || result->slice_header_error_code ||
        result->expected_syncpoint != result->observed_syncpoint ||
        result->mbs_correctly_decoded != 3600 || result->width != 1280 || result->height != 720 ||
        result->pitch != dec->pitch || result->aligned_height != dec->aligned_height ||
        result->frame_size != dec->frame_size || result->bytes_written != dec->frame_size)
        return E_FAIL;
    return write_nv12(dec, output.data() + sizeof(*result), dec->desc.SampleWidth,
                       dec->desc.SampleHeight, dec->pitch, dec->aligned_height);
}

static VOID APIENTRY VideoDecoderEndFrame(D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HDECODE hDecode)
{
    Device *dev = D(hDevice);
    VideoDecoder *dec = static_cast<VideoDecoder *>(hDecode.pDrvPrivate);
    if (!dec || dec->dev != dev || !dec->in_frame) { TG_ERROR(dev, E_INVALIDARG); return; }
    HRESULT hr;
    try { hr = FAILED(dev->lost) ? dev->lost : finish_frame(dec); }
    catch (const std::bad_alloc &) { hr = E_OUTOFMEMORY; }
    /* The KMD copies input into owned DMA storage and never DMAs into this
     * userspace target. Even on failure its retained in-flight backing is
     * separate from this held output slice. No incomplete pixels are copied. */
    --dec->output.chunk->videoWrites;
    slice_release(dev, dec->output, 0);
    dec->output = {};
    dec->in_frame = false;
    dec->have_pic = dec->have_qmatrix = false;
    if (FAILED(hr)) TG_ERROR(dev, hr);
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
    if (!video_device_supported(dev) || !IsEqualGUID(args->DecodeProfile, h264_profile) ||
        !res || res->dev != dev ||
        res->format != DXGI_FORMAT_NV12 || res->kind != ResKind::Staging ||
        !(res->bind & D3D11_DDI_BIND_DECODER) || args->MipSlice || args->ArraySize != 1 ||
        args->FirstArraySlice >= res->arraySize || args->FirstArraySlice >= TG_NVDEC_MAX_SURFACES)
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

static VOID APIENTRY GetVideoDecoderProfileCount(D3D10DDI_HDEVICE hDevice, UINT *count)
{
    if (count) *count = video_device_supported(D(hDevice)) ? 1u : 0u;
}

/* The 11.1 runtime checks processor capabilities while obtaining the video
 * device. Every description has the same empty set until processing is
 * implemented, so the runtime-owned enumerator needs no driver storage. */
static SIZE_T APIENTRY CalcPrivateVideoProcessorEnumSize(
    D3D10DDI_HDEVICE, const D3D11_1DDIARG_CREATEVIDEOPROCESSORENUM *)
{
    return 0;
}

static HRESULT APIENTRY CreateVideoProcessorEnum(D3D10DDI_HDEVICE device,
    const D3D11_1DDIARG_CREATEVIDEOPROCESSORENUM *args,
    D3D11_1DDI_HVIDEOPROCESSORENUM, D3D11_1DDI_HRTVIDEOPROCESSORENUM)
{
    if (!args || !video_device_supported(D(device))) return E_INVALIDARG;
    trace(TGD3D_TRACE_VIDEO, S_OK, 11,
          (uint64_t(args->Desc.InputWidth) << 32) | args->Desc.InputHeight);
    return S_OK;
}

static VOID APIENTRY DestroyVideoProcessorEnum(D3D10DDI_HDEVICE, D3D11_1DDI_HVIDEOPROCESSORENUM)
{
}

static VOID APIENTRY GetVideoProcessorCaps(D3D10DDI_HDEVICE, D3D11_1DDI_HVIDEOPROCESSORENUM,
                                          D3D11_1DDI_VIDEO_PROCESSOR_CAPS *caps)
{
    if (caps) {
        *caps = {};
        trace(TGD3D_TRACE_VIDEO, S_OK, 12, 0);
    }
}

static VOID APIENTRY CheckVideoProcessorFormat(D3D10DDI_HDEVICE, D3D11_1DDI_HVIDEOPROCESSORENUM,
                                              DXGI_FORMAT, UINT *flags)
{
    if (flags) *flags = 0;
}

static VOID APIENTRY GetVideoDecoderProfile(D3D10DDI_HDEVICE hDevice, UINT index, GUID *profile)
{
    if (!profile)
        return;
    *profile = (index == 0 && video_device_supported(D(hDevice))) ?
               h264_profile : GUID_NULL;
}

static VOID APIENTRY CheckVideoDecoderFormat(D3D10DDI_HDEVICE hDevice, const GUID *profile,
                                             DXGI_FORMAT format, BOOL *supported)
{
    if (supported) *supported = profile && IsEqualGUID(*profile, h264_profile) &&
                 format == DXGI_FORMAT_NV12 && video_device_supported(D(hDevice));
}

static VOID APIENTRY GetVideoDecoderConfigCount(
    D3D10DDI_HDEVICE hDevice, const D3D11_1DDI_VIDEO_DECODER_DESC *desc, UINT *count)
{
    if (count) *count = decoder_desc_supported(desc) &&
             video_device_supported(D(hDevice)) ? 1u : 0u;
}

static VOID APIENTRY GetVideoDecoderConfig(
    D3D10DDI_HDEVICE hDevice, const D3D11_1DDI_VIDEO_DECODER_DESC *desc, UINT index,
    D3D11_1DDI_VIDEO_DECODER_CONFIG *config)
{
    if (!config)
        return;
    memset(config, 0, sizeof(*config));
    if (index || !decoder_desc_supported(desc) || !video_device_supported(D(hDevice)))
        return;
    config->guidConfigBitstreamEncryption = no_encrypt;
    config->guidConfigMBcontrolEncryption = no_encrypt;
    config->guidConfigResidDiffEncryption = no_encrypt;
    config->ConfigBitstreamRaw = 2;
    config->ConfigMinRenderTargetBuffCount = TG_NVDEC_MAX_SURFACES;
}

static VOID APIENTRY GetVideoDecoderBufferTypeCount(D3D10DDI_HDEVICE hDevice,
                                                    const D3D11_1DDI_VIDEO_DECODER_DESC *desc,
                                                    UINT *count)
{
    if (count) *count = video_device_supported(D(hDevice)) && decoder_desc_supported(desc) ? 4 : 0;
}

static VOID APIENTRY GetVideoDecoderBufferInfo(D3D10DDI_HDEVICE hDevice,
                                               const D3D11_1DDI_VIDEO_DECODER_DESC *desc,
                                               UINT index,
                                               D3D11_1DDI_VIDEO_DECODER_BUFFER_INFO *info)
{
    static const D3D11_1DDI_VIDEO_DECODER_BUFFER_TYPE types[4] = {
        D3D11_1DDI_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS,
        D3D11_1DDI_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX,
        D3D11_1DDI_VIDEO_DECODER_BUFFER_SLICE_CONTROL,
        D3D11_1DDI_VIDEO_DECODER_BUFFER_BITSTREAM,
    };
    if (!info) return;
    *info = {};
    if (index >= 4 || !video_device_supported(D(hDevice)) || !decoder_desc_supported(desc))
        return;
    static const UINT sizes[4] = {sizeof(tgd3d_dxva_pic_h264), sizeof(tgd3d_dxva_qmatrix_h264),
        TG_NVDEC_MAX_SLICES * sizeof(tgd3d_dxva_slice_h264_short), bitstream_capacity};
    info->Type = types[index];
    info->Size = sizes[index];
    info->Usage = D3D11_1DDI_VIDEO_USAGE_PLAYBACK_NORMAL;
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
    table->pfnCalcPrivateVideoProcessorEnumSize = CalcPrivateVideoProcessorEnumSize;
    table->pfnCreateVideoProcessorEnum = CreateVideoProcessorEnum;
    table->pfnDestroyVideoProcessorEnum = DestroyVideoProcessorEnum;
    table->pfnGetVideoProcessorCaps = GetVideoProcessorCaps;
    table->pfnCheckVideoProcessorFormat = CheckVideoProcessorFormat;
}

} // namespace tgd3d
