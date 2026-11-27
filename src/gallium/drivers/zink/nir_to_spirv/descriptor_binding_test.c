/* SPDX-License-Identifier: MIT */
#include "nir_to_spirv.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/spirv/spirv.h"
#include <stdio.h>

/* Exercise the full NIR-to-SPIR-V path with a sampler's Gallium slot
 * different from its Vulkan binding, as used by D3D's independent t/s slots. */
static bool
check_binding(unsigned descriptor_set, unsigned binding)
{
   const nir_shader_compiler_options options = {0};
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options,
                                                  "independent_sampler_binding");
   nir_variable *texture = nir_variable_create(b.shader, nir_var_uniform,
      glsl_texture_type(GLSL_SAMPLER_DIM_2D, false, GLSL_TYPE_FLOAT), "texture");
   texture->data.driver_location = 5;
   texture->data.descriptor_set = descriptor_set;
   texture->data.binding = 133;
   nir_variable *sampler = nir_variable_create(b.shader, nir_var_uniform,
      glsl_bare_sampler_type(), "sampler");
   sampler->data.driver_location = 2;
   sampler->data.descriptor_set = descriptor_set;
   sampler->data.binding = binding;
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                            glsl_vec4_type(), "color");
   out->data.location = FRAG_RESULT_DATA0;
   nir_tex_instr *tex = nir_tex_instr_create(b.shader, 3);
   tex->op = nir_texop_tex;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->dest_type = nir_type_float32;
   tex->coord_components = 2;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_texture_deref,
                                     &nir_build_deref_var(&b, texture)->def);
   tex->src[1] = nir_tex_src_for_ssa(nir_tex_src_sampler_deref,
                                     &nir_build_deref_var(&b, sampler)->def);
   tex->src[2] = nir_tex_src_for_ssa(nir_tex_src_coord, nir_imm_vec2(&b, 0.25, 1.25));
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);
   nir_store_var(&b, out, &tex->def, 0xf);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   const struct ntv_info info = {.spirv_version = SPIRV_VERSION(1, 6), .bindless_set_idx = 7};
   struct spirv_shader *spirv = nir_to_spirv(b.shader, &info);
   bool valid = spirv != NULL;
   uint32_t sampler_type = 0, pointer_type = 0, sampler_var = 0;
   unsigned actual_binding = ~0u, actual_set = ~0u, samples = 0;
   /* Types/variables follow annotations in SPIR-V. Resolve the sampler's
    * variable first, then inspect its encoded binding/set decorations. */
   for (unsigned pass = 0; valid && pass < 2; pass++) {
      for (size_t i = 5; i < spirv->num_words;) {
         const uint32_t *w = &spirv->words[i];
         unsigned n = w[0] >> 16, op = w[0] & 0xffff;
         if (!n || i + n > spirv->num_words) { valid = false; break; }
         if (!pass) {
            if (op == SpvOpTypeSampler && n == 2) sampler_type = w[1];
            if (op == SpvOpTypePointer && n == 4 && sampler_type && w[3] == sampler_type)
               pointer_type = w[1];
            if (op == SpvOpVariable && n == 4 && pointer_type && w[1] == pointer_type)
               sampler_var = w[2];
            samples += op == SpvOpImageSampleImplicitLod;
         } else if (op == SpvOpDecorate && n == 4 && w[1] == sampler_var) {
            if (w[2] == SpvDecorationBinding) actual_binding = w[3];
            if (w[2] == SpvDecorationDescriptorSet) actual_set = w[3];
         }
         i += n;
      }
   }
   valid = valid && sampler_var && samples == 1 &&
           actual_binding == binding && actual_set == descriptor_set;
   if (!valid)
      fprintf(stderr, "sampler slot 2: set/binding %u/%u, expected %u/%u\n",
              actual_set, actual_binding, descriptor_set, binding);
   spirv_shader_delete(spirv);
   ralloc_free(b.shader);
   return valid;
}

int main(void)
{
   glsl_type_singleton_init_or_ref();
   bool valid = check_binding(2, 450) && check_binding(1, 0) && check_binding(3, 2);
   glsl_type_singleton_decref();
   if (valid) puts("NIR-to-SPIR-V independent sampler descriptor bindings pass (3 cases)");
   return valid ? 0 : 1;
}
