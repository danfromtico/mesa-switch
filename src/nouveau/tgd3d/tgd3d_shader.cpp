/* SPDX-License-Identifier: MIT
 *
 * Shader Model 4 and 5 bytecode to Vulkan shader objects.
 *
 * Rebuild DXBC signatures from the runtime's DDI entries, including patch
 * constants for hull and domain shaders. The modern DDI supplies component
 * types. Legacy interfaces supply system values, registers and masks.
 * Vertex input layouts and pixel render-target formats select numeric
 * variants. Domain shaders also link against the hull shader's output map.
 */
#include "tgd3d.h"

#include "dxbc/dxbc_api.h"
#include "dxbc/dxbc_signature.h"
#include "ir/passes/ir_pass_lower_io.h"
#include "spirv/spirv_builder.h"
#include "spirv/spirv_mapping.h"
#include "util/util_log.h"

#include <string.h>
#include <type_traits>

namespace tgd3d {

using namespace dxbc_spv;

namespace {

class TraceLogger : public util::Logger {
public:
   util::LogLevel getMinimumSeverity() override { return util::LogLevel::eError; }
   void message(util::LogLevel, const char *text) override { trace_message(text); }
};

/* Compute has its own layout. The D3D namespaces stay independent even
 * when one register is a texel buffer in one shader and an image in another. */
static UINT compute_binding(ir::ScalarType type, UINT slot)
{
   switch (type) {
   case ir::ScalarType::eCbv: return slot;
   case ir::ScalarType::eSampler: return D3D_CB_SLOTS + slot;
   case ir::ScalarType::eSrv: return D3D_CB_SLOTS + D3D_SAMPLER_SLOTS + slot;
   case ir::ScalarType::eUav: return D3D_CB_SLOTS + D3D_SAMPLER_SLOTS + D3D_SRV_SLOTS + slot;
   case ir::ScalarType::eUavCounter: return D3D_CB_SLOTS + D3D_SAMPLER_SLOTS + D3D_SRV_SLOTS + D3D_UAV_SLOTS + slot;
   default: return UINT_MAX;
   }
}

static HRESULT reflect_bindings(Shader *shader, const ir::Builder &builder)
{
   const bool compute = shader->stage == STAGE_CS;
   auto [begin, end] = builder.getDeclarations();
   for (auto it = begin; it != end; ++it) {
      const ir::Op &op = *it;
      if (!compute && op.getOpCode() != ir::OpCode::eDclUav && op.getOpCode() != ir::OpCode::eDclUavCounter)
         continue;
      const ir::Op *reg = &op;
      ir::ScalarType type;
      VkDescriptorType desc;
      UINT limit, dimension = 0;
      switch (op.getOpCode()) {
      case ir::OpCode::eSetCsWorkgroupSize: {
         const UINT x = static_cast<UINT>(op.getOperand(1));
         const UINT y = static_cast<UINT>(op.getOperand(2));
         const UINT z = static_cast<UINT>(op.getOperand(3));
         if (!x || !y || !z || x > 1024 || y > 1024 || z > 64 || uint64_t(x)*y*z > 1024)
            return E_INVALIDARG;
         shader->groupSize = x*y*z;
         continue;
      }
      case ir::OpCode::eDclCbv:
         type = ir::ScalarType::eCbv; desc = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; limit = D3D_CB_SLOTS;
         break;
      case ir::OpCode::eDclSampler:
         type = ir::ScalarType::eSampler; desc = VK_DESCRIPTOR_TYPE_SAMPLER; limit = D3D_SAMPLER_SLOTS;
         break;
      case ir::OpCode::eDclSrv:
      case ir::OpCode::eDclUav: {
         const bool uav = op.getOpCode() == ir::OpCode::eDclUav;
         dimension = static_cast<UINT>(op.getOperand(4));
         const bool buffer = ir::ResourceKind(dimension) == ir::ResourceKind::eBufferTyped;
         type = uav ? ir::ScalarType::eUav : ir::ScalarType::eSrv;
         limit = uav ? D3D_UAV_SLOTS : D3D_SRV_SLOTS;
         desc = uav ? (buffer ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
                    : (buffer ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
         break;
      }
      case ir::OpCode::eDclUavCounter:
         type = ir::ScalarType::eUavCounter; desc = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; limit = D3D_UAV_SLOTS;
         reg = &builder.getOpForOperand(op, 1);
         break;
      default: continue;
      }
      const UINT slot = static_cast<UINT>(reg->getOperand(2));
      if (slot >= limit || static_cast<UINT>(reg->getOperand(1)) || static_cast<UINT>(reg->getOperand(3)) != 1)
         return DXGI_ERROR_UNSUPPORTED;
      uint32_t binding;
      if (compute) {
         binding = compute_binding(type, slot);
      } else {
         const VulkanCaps &caps = *shader->dev->caps;
         if (slot >= caps.graphicsUavSlots || !(caps.graphicsUavStages & graphics_stage_flag(shader->stage)))
            return DXGI_ERROR_UNSUPPORTED;
         shader->uavMask |= UINT64_C(1) << slot;
         if (desc == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)
            shader->uavBufferMask |= UINT64_C(1) << slot;
         binding = graphics_uav_binding(shader->stage, slot, desc);
      }
      shader->bindings.push_back({binding, slot, desc, dimension});
   }
   if (!compute) return S_OK;
   if (!shader->groupSize) return E_INVALIDARG;
   std::vector<VkDescriptorSetLayoutBinding> entries;
   for (const auto &b : shader->bindings)
      entries.push_back({b.binding, b.type, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
   Device *dev = shader->dev;
   VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
   set.bindingCount = static_cast<UINT>(entries.size());
   set.pBindings = entries.data();
   HRESULT hr = resource_result(dev->vk.CreateDescriptorSetLayout(dev->vkdev, &set, nullptr, &shader->computeSet));
   if (FAILED(hr)) return hr;
   VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
   layout.setLayoutCount = 1;
   layout.pSetLayouts = &shader->computeSet;
   return resource_result(dev->vk.CreatePipelineLayout(dev->vkdev, &layout, nullptr, &shader->computeLayout));
}

class Mapping : public spirv::ResourceMapping {
public:
   Mapping(const Shader &shader, uint32_t cb_slots)
   : m_stage(shader.stage), m_cbSlots(cb_slots), m_bufferMask(shader.srvBufferMask),
     m_uavBufferMask(shader.uavBufferMask) { }

   spirv::DescriptorBinding mapDescriptor(ir::ScalarType type, uint32_t space,
                                          uint32_t index) override
   {
      spirv::DescriptorBinding binding = {};
      if (space)
         failed = true;
      if (m_stage == STAGE_CS) {
         binding.binding = compute_binding(type, index);
         failed |= binding.binding == UINT_MAX;
         return binding;
      }
      switch (type) {
      case ir::ScalarType::eCbv:
         failed |= index >= m_cbSlots;
         binding.set = SET_CB;
         binding.binding = m_stage * m_cbSlots + index;
         break;
      case ir::ScalarType::eSampler:
         failed |= index >= D3D_SAMPLER_SLOTS;
         binding.set = SET_TEX;
         binding.binding = m_stage * SET1_STRIDE + index;
         break;
      case ir::ScalarType::eSrv:
         failed |= index >= D3D_SRV_SLOTS;
         if (index < D3D_SRV_SLOTS && (m_bufferMask[index / 32] & (1u << (index % 32)))) {
            binding.set = SET_TBUF;
            binding.binding = m_stage * D3D_SRV_SLOTS + index;
         } else {
            binding.set = SET_TEX;
            binding.binding = m_stage * SET1_STRIDE + D3D_SAMPLER_SLOTS + index;
         }
         break;
      case ir::ScalarType::eUav:
      case ir::ScalarType::eUavCounter: {
         if (index >= D3D_UAV_SLOTS) { failed = true; break; }
         const bool buffer = m_uavBufferMask & (UINT64_C(1) << index);
         const VkDescriptorType desc = type == ir::ScalarType::eUavCounter ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER :
            buffer ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
         binding.set = SET_GRAPHICS_UAV;
         binding.binding = graphics_uav_binding(m_stage, index, desc);
         break;
      }
      default:
         failed = true;
         break;
      }
      return binding;
   }

   uint32_t mapPushData(ir::ShaderStageMask) override { return 0; }

   bool failed = false;

private:
   Stage m_stage;
   uint32_t m_cbSlots;
   const uint32_t *m_bufferMask;
   uint64_t m_uavBufferMask;
};

void put32(std::vector<uint8_t> &out, uint32_t value)
{
   const uint8_t bytes[4] = {(uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16),
                             (uint8_t)(value >> 24)};
   out.insert(out.end(), bytes, bytes + 4);
}

/* ISGN/OSGN body. `classes` holds two bits per register: 0 float, 1 uint,
 * 2 sint. Every entry shares one placeholder semantic name; stages link by
 * register, exactly as Direct3D 10 does. */
static uint32_t signature_sysval(D3D10_SB_NAME name, uint32_t &index)
{
   using S = dxbc::SignatureSysval;
   // Bytecode names identify individual edges. DXBC signatures identify the
   // array semantic and carry the edge number in the semantic index.
   if (name >= D3D11_SB_NAME_FINAL_QUAD_U_EQ_0_EDGE_TESSFACTOR && name <= D3D11_SB_NAME_FINAL_QUAD_V_EQ_1_EDGE_TESSFACTOR) {
      index = name - D3D11_SB_NAME_FINAL_QUAD_U_EQ_0_EDGE_TESSFACTOR; return uint32_t(S::eQuadEdgeTessFactor);
   }
   if (name >= D3D11_SB_NAME_FINAL_QUAD_U_INSIDE_TESSFACTOR && name <= D3D11_SB_NAME_FINAL_QUAD_V_INSIDE_TESSFACTOR) {
      index = name - D3D11_SB_NAME_FINAL_QUAD_U_INSIDE_TESSFACTOR; return uint32_t(S::eQuadInsideTessFactor);
   }
   if (name >= D3D11_SB_NAME_FINAL_TRI_U_EQ_0_EDGE_TESSFACTOR && name <= D3D11_SB_NAME_FINAL_TRI_W_EQ_0_EDGE_TESSFACTOR) {
      index = name - D3D11_SB_NAME_FINAL_TRI_U_EQ_0_EDGE_TESSFACTOR; return uint32_t(S::eTriEdgeTessFactor);
   }
   switch (name) {
   case D3D11_SB_NAME_FINAL_TRI_INSIDE_TESSFACTOR: index = 0; return uint32_t(S::eTriInsideTessFactor);
   case D3D11_SB_NAME_FINAL_LINE_DETAIL_TESSFACTOR: index = 0; return uint32_t(S::eLineDetailTessFactor);
   case D3D11_SB_NAME_FINAL_LINE_DENSITY_TESSFACTOR: index = 0; return uint32_t(S::eLineDensityTessFactor);
   default: return uint32_t(name);
   }
}

std::vector<uint8_t> signature_chunk(const std::vector<D3D11_1DDIARG_SIGNATURE_ENTRY2> &entries,
                                     bool input, bool formatOverride, uint32_t classes)
{
   std::vector<uint8_t> out;
   put32(out, (uint32_t)entries.size());
   put32(out, 8);
   const uint32_t name_offset = 8 + (uint32_t)entries.size() * 24;
   for (size_t i = 0; i < entries.size(); i++) {
      const auto &e = entries[i];
      uint32_t type = e.RegisterComponentType ? uint32_t(e.RegisterComponentType) : 3;
      if (formatOverride && e.Register < 16) {
         const uint32_t c = (classes >> (2 * e.Register)) & 3;
         type = c == 1 ? 1 : c == 2 ? 2 : 3;
      }
      uint32_t semanticIndex = uint32_t(i);
      uint32_t systemValue = signature_sysval(e.SystemValue, semanticIndex);
      put32(out, name_offset);
      put32(out, semanticIndex);
      put32(out, systemValue);
      put32(out, type);
      put32(out, e.Register);
      put32(out, (uint32_t)e.Mask | (input ? (uint32_t)e.Mask << 8 : 0));
   }
   static const char name[] = "TGIO\0\0\0";
   out.insert(out.end(), name, name + 8);
   return out;
}

std::vector<uint8_t> build_container(const Shader *shader, uint32_t in_classes, uint32_t out_classes)
{
   const auto isgn = signature_chunk(shader->inputs, true, shader->stage == STAGE_VS, in_classes);
   const auto osgn = signature_chunk(shader->outputs, false, shader->stage == STAGE_PS, out_classes);
   const bool tess = shader->stage == STAGE_HS || shader->stage == STAGE_DS;
   const auto pcsg = tess ? signature_chunk(shader->patchConstants, shader->stage == STAGE_DS, false, 0) : std::vector<uint8_t>{};
   const uint32_t code_bytes = (uint32_t)shader->tokens.size() * 4;
   const uint32_t chunks = tess ? 4 : 3;
   const uint32_t header = 32 + chunks * 4;
   std::vector<uint8_t> out;
   out.insert(out.end(), {'D', 'X', 'B', 'C'});
   out.resize(20);           /* the checksum is not consulted by the converter */
   put32(out, 1);
   put32(out, header + chunks * 8 + uint32_t(isgn.size() + osgn.size() + pcsg.size()) + code_bytes);
   put32(out, chunks);
   put32(out, header);
   put32(out, header + 8 + (uint32_t)isgn.size());
   put32(out, header + 16 + (uint32_t)isgn.size() + (uint32_t)osgn.size());
   if (tess) put32(out, header + 24 + uint32_t(isgn.size() + osgn.size() + pcsg.size()));
   out.insert(out.end(), {'I', 'S', 'G', 'N'});
   put32(out, (uint32_t)isgn.size());
   out.insert(out.end(), isgn.begin(), isgn.end());
   out.insert(out.end(), {'O', 'S', 'G', 'N'});
   put32(out, (uint32_t)osgn.size());
   out.insert(out.end(), osgn.begin(), osgn.end());
   if (tess) {
      out.insert(out.end(), {'P', 'C', 'S', 'G'});
      put32(out, uint32_t(pcsg.size()));
      out.insert(out.end(), pcsg.begin(), pcsg.end());
   }
   if (((shader->tokens[0] >> 4) & 15) >= 5)
      out.insert(out.end(), {'S', 'H', 'E', 'X'});
   else
      out.insert(out.end(), {'S', 'H', 'D', 'R'});
   put32(out, code_bytes);
   const uint8_t *code = reinterpret_cast<const uint8_t *>(shader->tokens.data());
   out.insert(out.end(), code, code + code_bytes);
   return out;
}

static uint64_t tess_io_mask(const ir::IoMap &map)
{
   uint64_t mask = 0;
   for (const auto &entry : map) {
      const auto type = entry.getType();
      if (type == ir::IoEntryType::ePerVertex || type == ir::IoEntryType::ePerPatch)
         mask |= UINT64_C(1) << (entry.getLocationIndex() + (type == ir::IoEntryType::ePerPatch ? 32 : 0));
   }
   return mask;
}

static bool link_tess_io(Shader *shader, ir::Builder &ir, ir::LowerIoPass &pass, uint64_t key)
{
   const bool hull = shader->stage == STAGE_HS;
   auto map = hull ? ir::IoMap::forOutputs(ir, 0) : ir::IoMap::forInputs(ir);
   shader->tessIoMask = tess_io_mask(map);
   if (hull || !shader->reflected || shader->tessIoMask == key)
      return pass.resolvePatchConstantLocations(map);
   // A DS can read a proper subset of its HS outputs. Use the producer's
   // complete map when allocating distinct Vulkan locations to both namespaces.
   ir::IoMap producer;
   for (uint64_t bits = key; bits; bits &= bits - 1) {
      const UINT i = __builtin_ctzll(bits);
      const auto kind = i < 32 ? ir::IoEntryType::ePerVertex : ir::IoEntryType::ePerPatch;
      producer.add(ir::IoLocation(kind, i % 32, 0xf), ir::IoSemantic());
   }
   return pass.resolvePatchConstantLocations(producer);
}

void reflect(Shader *shader, const ir::Builder &builder)
{
   auto [begin, end] = builder.getDeclarations();
   for (auto iter = begin; iter != end; iter++) {
      const ir::Op &op = *iter;
      switch (op.getOpCode()) {
      case ir::OpCode::eSetTessControlPoints:
         shader->inputControlPoints = uint32_t(op.getOperand(1u));
         break;
      case ir::OpCode::eSetTessDomain:
         shader->tessDomain = uint32_t(op.getOperand(1u));
         break;
      case ir::OpCode::eDclCbv: {
         const uint32_t index = uint32_t(op.getOperand(2u));
         if (index < D3D_CB_SLOTS)
            shader->cbMask |= 1u << index;
         break;
      }
      case ir::OpCode::eDclSampler: {
         const uint32_t index = uint32_t(op.getOperand(2u));
         if (index < D3D_SAMPLER_SLOTS)
            shader->samplerMask |= 1u << index;
         break;
      }
      case ir::OpCode::eDclSrv: {
         const uint32_t index = uint32_t(op.getOperand(2u));
         const auto kind = ir::ResourceKind(op.getOperand(4u));
         if (index >= D3D_SRV_SLOTS)
            break;
         shader->srvMask[index / 32] |= 1u << (index % 32);
         shader->srvKind[index] = (uint8_t)kind;
         if (kind == ir::ResourceKind::eBufferTyped || kind == ir::ResourceKind::eBufferRaw ||
             kind == ir::ResourceKind::eBufferStructured) {
            shader->srvBufferMask[index / 32] |= 1u << (index % 32);
            shader->usesTexelBuffers = true;
         }
         break;
      }
      case ir::OpCode::eDclInput:
         if (shader->stage == STAGE_VS) {
            const uint32_t location = uint32_t(op.getOperand(1u));
            if (location < 16)
               shader->inputRegisters |= 1u << location;
         }
         break;
      case ir::OpCode::eDclOutput:
         if (shader->stage == STAGE_PS) {
            const uint32_t location = uint32_t(op.getOperand(1u));
            if (location < 8)
               shader->outputRegisters |= 1u << location;
         }
         break;
      default:
         break;
      }
   }
   shader->reflected = true;
}

/* The register belongs to the producer currently feeding SO. Distinct
 * producer signatures may list the same register at different indices. */
static HRESULT capture_semantic(const Shader *producer, const SoCapture &field, UINT &index)
{
   index = UINT_MAX;
   for (UINT j = 0; j < producer->outputs.size(); ++j) {
      const auto &out = producer->outputs[j];
      if (out.Register != field.reg) continue;
      if (out.Stream || index != UINT_MAX || (field.mask & out.Mask) != field.mask)
         return DXGI_ERROR_UNSUPPORTED;
      index = j;
   }
   return index == UINT_MAX ? E_INVALIDARG : S_OK;
}

/* The runtime supplies register masks; dxbc-spirv addresses captures by
 * semantic and components relative to that semantic. Preserve declaration
 * order (including gaps) when assigning byte offsets. */
HRESULT describe_stream_output(Shader *shader, const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT &so)
{
   if (so.NumStrides > 4 || (so.NumStrides && !so.BufferStridesInBytes) ||
       so.NumEntries > 512 || (so.NumEntries && !so.pOutputStreamDecl)) return E_INVALIDARG;
   if (so.RasterizedStream != 0 && so.RasterizedStream != UINT_MAX) return DXGI_ERROR_UNSUPPORTED;
   if ((so.NumEntries || so.RasterizedStream) && !shader->dev->caps->transformFeedback)
      return DXGI_ERROR_UNSUPPORTED;
   if (!so.NumEntries && !so.RasterizedStream) return S_OK;
   auto layout = std::make_shared<StreamOutput>();
   layout->discardRaster = so.RasterizedStream == UINT_MAX;
   shader->so = layout;
   if (!so.NumEntries) return S_OK;
   for (const auto &out : shader->outputs)
      if (out.Stream) return DXGI_ERROR_UNSUPPORTED;

   UINT bytes = 0;
   bool usedRegister[32]{};
   for (UINT i = 0; i < so.NumEntries; ++i) {
      const auto &entry = so.pOutputStreamDecl[i];
      if (entry.Stream || entry.OutputSlot) return DXGI_ERROR_UNSUPPORTED;
      if (!entry.RegisterMask || entry.RegisterMask > 15) return E_INVALIDARG;
      const UINT width = 4 * __builtin_popcount(entry.RegisterMask);
      if (entry.RegisterIndex != D3D10_SO_DDI_REGISTER_INDEX_DENOTING_GAP) {
         // Maxwell has 128 component selectors per buffer. The stride can
         // include trailing padding beyond the final captured component.
         if (bytes + width > 512) return DXGI_ERROR_UNSUPPORTED;
         if (entry.RegisterIndex >= 32 || usedRegister[entry.RegisterIndex]) return DXGI_ERROR_UNSUPPORTED;
         usedRegister[entry.RegisterIndex] = true;
         SoCapture field{uint8_t(entry.RegisterIndex), entry.RegisterMask, uint16_t(bytes)};
         if (!shader->soPassthrough || !shader->outputs.empty()) {
            UINT index;
            HRESULT hr = capture_semantic(shader, field, index);
            if (FAILED(hr)) return hr;
         }
         layout->fields.push_back(field);
      }
      bytes += width;
      if (bytes > 2048) return E_INVALIDARG;
   }
   if (layout->fields.empty()) return DXGI_ERROR_UNSUPPORTED;
   layout->stride = so.NumStrides ? so.BufferStridesInBytes[0] : bytes;
   if (layout->stride < bytes || layout->stride > 2048 || (layout->stride & 3)) return E_INVALIDARG;
   return S_OK;
}

bool lower_stream_output(const Shader *shader, ir::Builder &builder, const StreamOutput &layout)
{
   std::vector<ir::IoXfbInfo> entries;
   UINT components = 0;
   for (const auto &capture : layout.fields) {
      UINT index;
      if (FAILED(capture_semantic(shader, capture, index))) return false;
      const UINT mask = capture.mask >> __builtin_ctz(shader->outputs[index].Mask);
      entries.push_back({"TGIO", uint8_t(index), uint8_t(mask), 0, 0, capture.offset, uint16_t(layout.stride)});
      components += __builtin_popcount(mask);
   }
   if (components > 128) return false;
   if (!layout.discardRaster) {
      uint32_t locations = 0;
      for (const auto &e : ir::IoMap::forOutputs(builder, 0))
         if (e.getType() == ir::IoEntryType::ePerVertex) locations |= 1u << e.getLocationIndex();
      if (components > 4u * (32u - __builtin_popcount(locations))) return false;
   }
   ir::LowerIoPass pass(builder);
   if (!pass.resolveXfbOutputs(entries.size(), entries.data(), layout.discardRaster ? -1 : 0)) return false;
   // A missing semantic is silently ignored by the converter.
   auto [first, last] = builder.getDeclarations();
   UINT lowered = 0;
   for (auto it = first; it != last; ++it)
      lowered += it->getOpCode() == ir::OpCode::eDclXfb;
   return lowered == components;
}

VkShaderEXT compile(Shader *shader, uint64_t key, const StreamOutput *capture)
{
   Device *dev = shader->dev;
   const VulkanCaps &caps = *dev->caps;
   const uint64_t begin = qpc_now();
   const uint64_t identity = (uintptr_t)shader;
   const uint64_t frontend = (UINT64_C(1) << 32) | shader->stage;
   const uint64_t backend = (UINT64_C(2) << 32) | shader->stage;
   trace(TGD3D_TRACE_SHADER_PHASE, E_PENDING, frontend, identity);
   TraceLogger logger;

   uint32_t in_classes = 0, out_classes = 0;
   bool dual_source = false;
   if (shader->stage == STAGE_VS)
      in_classes = key;
   if (shader->stage == STAGE_PS) {
      out_classes = key & 0xffff;
      dual_source = (key >> 31) != 0;
   }
   const std::vector<uint8_t> container = build_container(shader, in_classes, out_classes);

   dxbc::Converter::Options convert = {};
   convert.includeDebugNames = false;
   ir::CompileOptions options = {};
   // A four-byte view offset is valid for D3D structured and raw buffers.
   // R32_UINT texel views preserve this offset and the exact descriptor bounds on GM20B.
   options.bufferOptions.useTypedForRaw = true;
   options.bufferOptions.useTypedForStructured = true;
   auto ir = dxbc::compileShaderToIr(container.data(), container.size(), convert);
   if (!ir) {
      shader->error = E_INVALIDARG;
      trace(TGD3D_TRACE_SHADER_PHASE, shader->error, frontend, identity);
      return VK_NULL_HANDLE;
   }
   if (capture && !capture->fields.empty() && !lower_stream_output(shader, *ir, *capture)) {
      shader->error = DXGI_ERROR_UNSUPPORTED;
      return VK_NULL_HANDLE;
   }
   ir::legalizeIr(*ir, options);
   {
      ir::LowerIoPass pass(*ir);
      pass.lowerSampleCountToSpecConstant(0u);
      // Resource creation currently supports only single-sample attachments.
      // GetRenderTargetSampleCount also reads one for WARP's UAV-only draws
      // when the forced coverage pattern has 4, 8 or 16 samples.
      auto [first, last] = ir->getDeclarations();
      for (auto it = first; it != last; ++it) {
         if (it->getOpCode() == ir::OpCode::eDclSpecConstant && UINT(it->getOperand(1)) == 0) {
            const auto def = it->getDef();
            ir->rewriteDef(def, ir->makeConstant(1u));
            break;
         }
      }
      if ((shader->stage == STAGE_HS || shader->stage == STAGE_DS) && !link_tess_io(shader, *ir, pass, key)) {
         shader->error = DXGI_ERROR_UNSUPPORTED;
         return VK_NULL_HANDLE;
      }
   }
   if (!shader->reflected) {
      shader->error = reflect_bindings(shader, *ir);
      if (FAILED(shader->error)) return VK_NULL_HANDLE;
      reflect(shader, *ir);
   }

   Mapping mapping(*shader, caps.cbSlots);
   spirv::SpirvBuilder::Options spv = {};
   spv.includeDebugNames = false;
   spv.dualSourceBlending = dual_source;
   spv.floatControls2 = false;
   if (caps.roundRTE)
      spv.supportedRoundModesF32 |= ir::RoundMode::eNearestEven;
   if (caps.roundRTZ)
      spv.supportedRoundModesF32 |= ir::RoundMode::eZero;
   if (caps.denormPreserve)
      spv.supportedDenormModesF32 |= ir::DenormMode::ePreserve;
   if (caps.denormFlush)
      spv.supportedDenormModesF32 |= ir::DenormMode::eFlush;
   spv.supportsZeroInfNanPreserveF32 = caps.zeroInfNanPreserve;
   spirv::SpirvBuilder builder(*ir, mapping, spv);
   builder.buildSpirvBinary();
   const std::vector<uint32_t> code = builder.getSpirvBinary();
   if (mapping.failed || code.empty()) {
      /* A register this layout cannot address; nothing is remapped silently. */
      shader->error = DXGI_ERROR_UNSUPPORTED;
      trace(TGD3D_TRACE_SHADER_PHASE, shader->error, frontend, identity);
      return VK_NULL_HANDLE;
   }
   trace(TGD3D_TRACE_SHADER_PHASE, S_OK, frontend, identity);

   VkShaderCreateInfoEXT info = {VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
   switch (shader->stage) {
   case STAGE_CS:
      info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      break;
   case STAGE_VS:
      info.stage = VK_SHADER_STAGE_VERTEX_BIT;
      info.nextStage = VK_SHADER_STAGE_FRAGMENT_BIT |
                       (caps.geometryShader ? VK_SHADER_STAGE_GEOMETRY_BIT : 0) |
                       (caps.tessellationShader ? VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT : 0);
      break;
   case STAGE_HS:
      info.stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
      info.nextStage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
      break;
   case STAGE_DS:
      info.stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
      info.nextStage = VK_SHADER_STAGE_FRAGMENT_BIT | (caps.geometryShader ? VK_SHADER_STAGE_GEOMETRY_BIT : 0);
      break;
   case STAGE_GS:
      info.stage = VK_SHADER_STAGE_GEOMETRY_BIT;
      info.nextStage = VK_SHADER_STAGE_FRAGMENT_BIT;
      break;
   default:
      info.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
      break;
   }
   info.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
   info.codeSize = code.size() * sizeof(uint32_t);
   info.pCode = code.data();
   info.pName = "main";
   const bool compute = shader->stage == STAGE_CS;
   info.setLayoutCount = compute ? 1u : caps.graphicsSetCount();
   info.pSetLayouts = compute ? &shader->computeSet : dev->setLayouts;
   VkShaderEXT handle = VK_NULL_HANDLE;
   trace(TGD3D_TRACE_SHADER_PHASE, E_PENDING, backend, identity);
   const VkResult result = create_shader_cached(dev, info, &handle, compute ? shader : nullptr);
   const HRESULT hr = result == VK_SUCCESS ? S_OK :
      result == VK_ERROR_DEVICE_LOST ? D3DDDIERR_DEVICEREMOVED :
      result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY ?
         E_OUTOFMEMORY : E_FAIL;
   trace(TGD3D_TRACE_SHADER_PHASE, hr, backend, identity);
   const uint64_t end = qpc_now();
   LARGE_INTEGER frequency;
   QueryPerformanceFrequency(&frequency);
   trace(TGD3D_TRACE_SHADER, hr,
         (uint64_t)shader->stage << 32 | (uint32_t)shader->tokens.size(),
         (end - begin) * 1000000 / (uint64_t)frequency.QuadPart);
   if (result != VK_SUCCESS) {
      shader->error = hr;
      return VK_NULL_HANDLE;
   }
   return handle;
}

SIZE_T APIENTRY CalcPrivateShaderSize(D3D10DDI_HDEVICE, const UINT *, const D3D10DDIARG_STAGE_IO_SIGNATURES *)
{
   return sizeof(Shader);
}

void shader_create_failed(Shader *shader, HRESULT hr)
{
   Device *dev = shader->dev;
   /* A failed Create*Shader invalidates the private handle. The runtime
    * never calls DestroyShader for it, and may reclaim the handle when we
    * report the error. Release the token/signature arrays first. */
   if (shader->computeLayout)
      dev->vk.DestroyPipelineLayout(dev->vkdev, shader->computeLayout, nullptr);
   if (shader->computeSet)
      dev->vk.DestroyDescriptorSetLayout(dev->vkdev, shader->computeSet, nullptr);
   shader->~Shader();
   TG_ERROR(dev, hr);
}

template<typename Entry>
void copy_signature(std::vector<D3D11_1DDIARG_SIGNATURE_ENTRY2> &dest, const Entry *source, UINT count)
{
   if (!count) return;
   if constexpr (std::is_same_v<Entry, D3D11_1DDIARG_SIGNATURE_ENTRY2>) {
      dest.assign(source, source + count);
   } else {
      dest.resize(count);
      for (UINT i = 0; i < count; ++i) {
         dest[i].SystemValue = source[i].SystemValue;
         dest[i].Register = source[i].Register;
         dest[i].Mask = source[i].Mask;
      }
   }
}

template<typename Signatures>
void create_shader(D3D10DDI_HDEVICE hDevice, const UINT *code, D3D10DDI_HSHADER handle, Stage stage,
                   const Signatures *signatures, const D3D11_1DDIARG_SIGNATURE_ENTRY2 *patch = nullptr, UINT patchCount = 0,
                   const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *so = nullptr)
{
   Device *dev = D(hDevice);
   if (FAILED(dev->lost)) {
      TG_ERROR(dev, dev->lost);
      return;
   }
   constexpr UINT types[] = {D3D10_SB_VERTEX_SHADER, D3D10_SB_GEOMETRY_SHADER, D3D10_SB_PIXEL_SHADER,
                             D3D11_SB_HULL_SHADER, D3D11_SB_DOMAIN_SHADER, D3D11_SB_COMPUTE_SHADER};
   const bool passthrough = so && (!code || (code[0] >> 16) == D3D10_SB_VERTEX_SHADER ||
                                     (code[0] >> 16) == D3D11_SB_DOMAIN_SHADER);
   if (!handle.pDrvPrivate || (!code && !passthrough) || (code && (code[1] < 2 ||
       (!passthrough && (code[0] >> 16) != types[stage]))) ||
       patchCount > 128 || (patchCount && !patch) ||
       (signatures && (signatures->NumInputSignatureEntries > 128 || signatures->NumOutputSignatureEntries > 128 ||
        (signatures->NumInputSignatureEntries && !signatures->pInputSignature) ||
        (signatures->NumOutputSignatureEntries && !signatures->pOutputSignature)))) {
      TG_ERROR(dev, E_INVALIDARG); return;
   }
   Shader *shader = new (handle.pDrvPrivate) Shader();
   shader->dev = dev;
   shader->stage = stage;
   if (stage != STAGE_CS && !dev->caps->graphicsStageSupported(stage)) {
      shader_create_failed(shader, DXGI_ERROR_UNSUPPORTED);
      return;
   }
   /* Token 1 of a shader program is its length in 32-bit words. */
   VkShaderEXT base = VK_NULL_HANDLE;
   try {
      shader->soPassthrough = passthrough;
      if (!passthrough) shader->tokens.assign(code, code + code[1]);
      if (signatures) {
         copy_signature(shader->inputs, signatures->pInputSignature, signatures->NumInputSignatureEntries);
         copy_signature(shader->outputs, signatures->pOutputSignature, signatures->NumOutputSignatureEntries);
      }
      copy_signature(shader->patchConstants, patch, patchCount);
      if (so) {
         HRESULT hr = describe_stream_output(shader, *so);
         if (FAILED(hr)) { shader_create_failed(shader, hr); return; }
      }
      // With no application GS the active VS or DS will own the Xfb
      // variant. Do not insert a geometry invocation into the D3D pipeline.
      if (passthrough) return;
      shader->variants.reserve(1);
      base = compile(shader, 0, shader->so.get());
   } catch (const std::bad_alloc &) {
      shader_create_failed(shader, E_OUTOFMEMORY);
      return;
   }
   if (!base) {
      shader_create_failed(shader, FAILED(shader->error) ? shader->error : E_FAIL);
      return;
   }
   shader->variants.push_back({stage == STAGE_DS ? shader->tessIoMask : 0, base});
}

void APIENTRY CreateVertexShader(D3D10DDI_HDEVICE hDevice, const UINT *code, D3D10DDI_HSHADER handle,
                                 D3D10DDI_HRTSHADER, const D3D10DDIARG_STAGE_IO_SIGNATURES *signatures)
{
   create_shader(hDevice, code, handle, STAGE_VS, signatures);
}

void APIENTRY CreateGeometryShader(D3D10DDI_HDEVICE hDevice, const UINT *code, D3D10DDI_HSHADER handle,
                                   D3D10DDI_HRTSHADER, const D3D10DDIARG_STAGE_IO_SIGNATURES *signatures)
{
   create_shader(hDevice, code, handle, STAGE_GS, signatures);
}

void APIENTRY CreatePixelShader(D3D10DDI_HDEVICE hDevice, const UINT *code, D3D10DDI_HSHADER handle,
                                D3D10DDI_HRTSHADER, const D3D10DDIARG_STAGE_IO_SIGNATURES *signatures)
{
   create_shader(hDevice, code, handle, STAGE_PS, signatures);
}

SIZE_T APIENTRY CalcPrivateGeometryShaderWithStreamOutput(
   D3D10DDI_HDEVICE, const D3D10DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *,
   const D3D10DDIARG_STAGE_IO_SIGNATURES *)
{
   return sizeof(Shader);
}

void APIENTRY CreateGeometryShaderWithStreamOutput(
   D3D10DDI_HDEVICE hDevice, const D3D10DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *args,
   D3D10DDI_HSHADER handle, D3D10DDI_HRTSHADER, const D3D10DDIARG_STAGE_IO_SIGNATURES *signatures)
{
   Device *dev = D(hDevice);
   if (args->NumEntries) {
      /* Legacy DDI capture declarations still need translation. */
      TG_ERROR(dev, E_NOTIMPL);
      return;
   }
   create_shader(hDevice, args->pShaderCode, handle, STAGE_GS, signatures);
}

void APIENTRY DestroyShader(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER handle)
{
   Device *dev = D(hDevice);
   Shader *shader = static_cast<Shader *>(handle.pDrvPrivate);
   for (auto &stage : dev->stage) {
      if (stage.shader == shader) {
         stage.shader = nullptr;
         dev->dirty |= DIRTY_SHADERS;
      }
   }
   for (const auto &variant : shader->variants)
      defer(dev, DeferredType::Shader, (uint64_t)variant.shader, dev->lastSeq());
   defer(dev, DeferredType::PipelineLayout, (uint64_t)shader->computeLayout, dev->lastSeq());
   defer(dev, DeferredType::SetLayout, (uint64_t)shader->computeSet, dev->lastSeq());
   shader->~Shader();
}

void set_shader(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER handle, Stage stage)
{
   Device *dev = D(hDevice);
   Shader *shader = static_cast<Shader *>(handle.pDrvPrivate);
   if (shader && (shader->dev != dev || shader->stage != stage)) { TG_ERROR(dev, E_INVALIDARG); return; }
   if (dev->stage[stage].shader == shader)
      return;
   dev->stage[stage].shader = shader;
   /* A different shader reads a different set of slots. */
   dev->stage[stage].cbDirty = ~0u;
   dev->dirty |= DIRTY_SHADERS | DIRTY_TEXTURES | DIRTY_TEXEL_BUFFERS | DIRTY_GRAPHICS_UAVS;
}

void APIENTRY VsSetShader(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER s) { set_shader(h, s, STAGE_VS); }
void APIENTRY GsSetShader(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER s) { set_shader(h, s, STAGE_GS); }
void APIENTRY PsSetShader(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER s) { set_shader(h, s, STAGE_PS); }

}

void create_shader11(D3D10DDI_HDEVICE h, const UINT *code, D3D10DDI_HSHADER handle, Stage stage,
                     const D3D11_1DDIARG_STAGE_IO_SIGNATURES *io, const D3D11_1DDIARG_SIGNATURE_ENTRY2 *patch,
                     UINT patchCount, const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *so)
{
   create_shader(h, code, handle, stage, io, patch, patchCount, so);
}

void set_graphics_shader(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER handle, Stage stage)
{
   set_shader(h, handle, stage);
}

void APIENTRY CreateComputeShader(D3D10DDI_HDEVICE h, const UINT *code, D3D10DDI_HSHADER handle,
                                 D3D10DDI_HRTSHADER)
{
   Device *dev = D(h);
   if (!dev->caps || !dev->caps->compute) { TG_ERROR(dev, DXGI_ERROR_UNSUPPORTED); return; }
   if (!handle.pDrvPrivate || !code || code[1] < 2 || (code[0] >> 16) != 5) {
      TG_ERROR(dev, E_INVALIDARG); return;
   }
   create_shader<D3D10DDIARG_STAGE_IO_SIGNATURES>(h, code, handle, STAGE_CS, nullptr);
}

void APIENTRY CsSetShader(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER handle)
{
   Device *dev = D(h);
   auto *shader = static_cast<Shader *>(handle.pDrvPrivate);
   if (shader && (shader->dev != dev || shader->stage != STAGE_CS)) {
      TG_ERROR(dev, E_INVALIDARG); return;
   }
   dev->stage[STAGE_CS].shader = shader;
}

VkShaderEXT shader_variant(Shader *shader, uint64_t key, const std::shared_ptr<const StreamOutput> &capture)
{
   if (!shader || FAILED(shader->dev->lost) || FAILED(shader->error))
      return VK_NULL_HANDLE;
   for (const auto &variant : shader->variants)
      if (variant.key == key && (variant.capture == capture ||
          (variant.capture && capture && *variant.capture == *capture)))
         return variant.shader;
   VkShaderEXT handle;
   try {
      shader->variants.reserve(shader->variants.size() + 1);
      handle = compile(shader, key, capture ? capture.get() : shader->so.get());
   } catch (const std::bad_alloc &) {
      TG_ERROR(shader->dev, E_OUTOFMEMORY);
      return VK_NULL_HANDLE;
   }
   if (!handle) {
      TG_ERROR(shader->dev, FAILED(shader->error) ? shader->error : E_FAIL);
      if (SUCCEEDED(shader->dev->lost))
         shader->error = S_OK;    /* a transient failure leaves the base usable */
      return VK_NULL_HANDLE;
   }
   shader->variants.push_back({key, handle, capture});
   return handle;
}

void shader_fill_table(D3D10DDI_DEVICEFUNCS *t)
{
   t->pfnCalcPrivateShaderSize = CalcPrivateShaderSize;
   t->pfnCreateVertexShader = CreateVertexShader;
   t->pfnCreateGeometryShader = CreateGeometryShader;
   t->pfnCreatePixelShader = CreatePixelShader;
   t->pfnCalcPrivateGeometryShaderWithStreamOutput = CalcPrivateGeometryShaderWithStreamOutput;
   t->pfnCreateGeometryShaderWithStreamOutput = CreateGeometryShaderWithStreamOutput;
   t->pfnDestroyShader = DestroyShader;
   t->pfnVsSetShader = VsSetShader;
   t->pfnGsSetShader = GsSetShader;
   t->pfnPsSetShader = PsSetShader;
}

}
