/* SPDX-License-Identifier: MIT */
#include <gtest/gtest.h>
#include "nir/tgsi_to_nir.h"
#include "compiler/nir/nir_builder.h"
#include "tgsi/tgsi_ureg.h"

class TgsiSample : public testing::Test {
protected:
   nir_shader_compiler_options options = {};
   nir_shader *shader = nullptr;

   void SetUp() override { glsl_type_singleton_init_or_ref(); }
   void TearDown() override {
      ralloc_free(shader);
      glsl_type_singleton_decref();
   }

   nir_tex_instr *translate(tgsi_texture_type target, unsigned texture,
                            unsigned sampler, bool swizzle = false,
                            bool offset = false, bool legacy = false,
                            bool fetch = false,
                            tgsi_return_type type = TGSI_RETURN_TYPE_FLOAT)
   {
      ureg_program *u = ureg_create(MESA_SHADER_FRAGMENT);
      auto coord = fetch ? ureg_imm4u(u, 7, 11, 3, 2) :
         ureg_DECL_fs_input(u, TGSI_SEMANTIC_GENERIC, 0,
                           TGSI_INTERPOLATE_PERSPECTIVE);
      auto output = ureg_DECL_output(u, TGSI_SEMANTIC_COLOR, 0);
      auto view = ureg_DECL_sampler_view(u, texture, target,
         type, type, type, type);
      struct ureg_src state = {};
      if (!fetch)
         state = ureg_DECL_sampler(u, sampler);
      if (swizzle)
         view = ureg_swizzle(view, TGSI_SWIZZLE_W, TGSI_SWIZZLE_Z,
                            TGSI_SWIZZLE_Y, TGSI_SWIZZLE_X);
      struct ureg_src src[] = {coord, legacy ? state : view, state};
      tgsi_texture_offset tex_offset = {};
      if (offset) {
         auto imm = ureg_imm3i(u, -1, 2, 0);
         tex_offset.File = imm.File;
         tex_offset.Index = imm.Index;
         tex_offset.SwizzleX = imm.SwizzleX;
         tex_offset.SwizzleY = imm.SwizzleY;
         tex_offset.SwizzleZ = imm.SwizzleZ;
      }
      ureg_tex_insn(u, fetch ? TGSI_OPCODE_SAMPLE_I :
                         legacy ? TGSI_OPCODE_TEX : TGSI_OPCODE_SAMPLE,
                    &output, 1, legacy ? target : TGSI_TEXTURE_UNKNOWN,
                    &tex_offset, offset, src, legacy || fetch ? 2 : 3);
      ureg_END(u);
      const tgsi_token *tokens = ureg_get_tokens(u, nullptr);
      shader = tgsi_to_nir_noscreen(tokens, &options);
      ureg_free_tokens(tokens);
      ureg_destroy(u);
      nir_foreach_function_impl(impl, shader) {
         nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
               if (instr->type == nir_instr_type_tex)
                  return nir_instr_as_tex(instr);
            }
         }
      }
      return nullptr;
   }

   nir_variable *binding(nir_tex_instr *tex, nir_tex_src_type type) {
      int index = nir_tex_instr_src_index(tex, type);
      if (index < 0) return nullptr;
      return nir_deref_instr_get_variable(nir_src_as_deref(tex->src[index].src));
   }
};

TEST_F(TgsiSample, IndependentBindings)
{
   auto tex = translate(TGSI_TEXTURE_2D, 5, 2);
   ASSERT_NE(tex, nullptr);
   EXPECT_EQ(tex->op, nir_texop_tex);
   EXPECT_EQ(tex->coord_components, 2);
   auto image = binding(tex, nir_tex_src_texture_deref);
   auto sampler = binding(tex, nir_tex_src_sampler_deref);
   ASSERT_NE(image, nullptr);
   ASSERT_NE(sampler, nullptr);
   EXPECT_NE(image, sampler);
   EXPECT_TRUE(glsl_type_is_texture(image->type));
   EXPECT_TRUE(glsl_type_is_bare_sampler(sampler->type));
   EXPECT_EQ(image->data.binding, 5);
   EXPECT_EQ(sampler->data.binding, 2);
   EXPECT_EQ(shader->info.num_textures, 6);
   EXPECT_TRUE(BITSET_TEST(shader->info.textures_used, 5));
   EXPECT_FALSE(BITSET_TEST(shader->info.textures_used, 2));
   EXPECT_TRUE(BITSET_TEST(shader->info.samplers_used, 2));
}

TEST_F(TgsiSample, ViewIndexBeyondSamplerLimit)
{
   auto tex = translate(TGSI_TEXTURE_2D, 63, 7);
   ASSERT_NE(tex, nullptr);
   ASSERT_NE(binding(tex, nir_tex_src_texture_deref), nullptr);
   EXPECT_EQ(binding(tex, nir_tex_src_texture_deref)->data.binding, 63);
   EXPECT_EQ(binding(tex, nir_tex_src_sampler_deref)->data.binding, 7);
   EXPECT_EQ(shader->info.num_textures, 64);
}

TEST_F(TgsiSample, ArrayCoordinates)
{
   auto tex = translate(TGSI_TEXTURE_2D_ARRAY, 3, 1);
   ASSERT_NE(tex, nullptr);
   EXPECT_TRUE(tex->is_array);
   EXPECT_FALSE(tex->is_shadow);
   EXPECT_EQ(tex->sampler_dim, GLSL_SAMPLER_DIM_2D);
   EXPECT_EQ(tex->coord_components, 3);
}

TEST_F(TgsiSample, TexelOffset)
{
   auto tex = translate(TGSI_TEXTURE_2D, 3, 1, false, true);
   ASSERT_NE(tex, nullptr);
   int index = nir_tex_instr_src_index(tex, nir_tex_src_offset);
   ASSERT_GE(index, 0);
   EXPECT_EQ(nir_src_num_components(tex->src[index].src), 2);
   EXPECT_EQ(nir_scalar_as_int(nir_scalar_resolved(tex->src[index].src.ssa, 0)), -1);
   EXPECT_EQ(nir_scalar_as_int(nir_scalar_resolved(tex->src[index].src.ssa, 1)), 2);
}

TEST_F(TgsiSample, ResultSwizzle)
{
   auto tex = translate(TGSI_TEXTURE_2D, 3, 1, true);
   ASSERT_NE(tex, nullptr);
   bool found = false;
   nir_foreach_use(use, &tex->def) {
      nir_instr *parent = nir_src_use_instr(use);
      if (parent->type != nir_instr_type_alu) continue;
      auto alu = nir_instr_as_alu(parent);
      if (alu->op != nir_op_mov) continue;
      found = true;
      for (unsigned i = 0; i < 4; ++i)
         EXPECT_EQ(alu->src[0].swizzle[i], 3 - i);
   }
   EXPECT_TRUE(found);
}

TEST_F(TgsiSample, LegacyCombinedBinding)
{
   auto tex = translate(TGSI_TEXTURE_2D, 2, 2, false, false, true);
   ASSERT_NE(tex, nullptr);
   EXPECT_EQ(binding(tex, nir_tex_src_texture_deref),
             binding(tex, nir_tex_src_sampler_deref));
   EXPECT_EQ(binding(tex, nir_tex_src_texture_deref)->data.binding, 2);
}

TEST_F(TgsiSample, IntegerLoadHasNoSampler)
{
   auto tex = translate(TGSI_TEXTURE_2D, 63, 0, false, false, false, true);
   ASSERT_NE(tex, nullptr);
   EXPECT_EQ(tex->op, nir_texop_txf);
   EXPECT_EQ(tex->coord_components, 2);
   ASSERT_NE(binding(tex, nir_tex_src_texture_deref), nullptr);
   EXPECT_EQ(binding(tex, nir_tex_src_texture_deref)->data.binding, 63);
   EXPECT_EQ(binding(tex, nir_tex_src_sampler_deref), nullptr);
   EXPECT_TRUE(BITSET_TEST(shader->info.textures_used_by_txf, 63));
   EXPECT_TRUE(BITSET_IS_EMPTY(shader->info.samplers_used));
   int coord = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   int lod = nir_tex_instr_src_index(tex, nir_tex_src_lod);
   ASSERT_GE(coord, 0);
   ASSERT_GE(lod, 0);
   EXPECT_EQ(nir_scalar_as_uint(nir_scalar_resolved(tex->src[coord].src.ssa, 0)), 7);
   EXPECT_EQ(nir_scalar_as_uint(nir_scalar_resolved(tex->src[coord].src.ssa, 1)), 11);
   EXPECT_EQ(nir_scalar_as_uint(nir_scalar_resolved(tex->src[lod].src.ssa, 0)), 2);
}

TEST_F(TgsiSample, IntegerArrayLoad)
{
   auto tex = translate(TGSI_TEXTURE_2D_ARRAY, 3, 0, false, false, false, true);
   ASSERT_NE(tex, nullptr);
   EXPECT_TRUE(tex->is_array);
   EXPECT_EQ(tex->coord_components, 3);
   int coord = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   ASSERT_GE(coord, 0);
   EXPECT_EQ(nir_scalar_as_uint(nir_scalar_resolved(tex->src[coord].src.ssa, 2)), 3);
}

TEST_F(TgsiSample, BufferLoadIgnoresMip)
{
   auto tex = translate(TGSI_TEXTURE_BUFFER, 3, 0, false, false, false, true);
   ASSERT_NE(tex, nullptr);
   EXPECT_EQ(tex->sampler_dim, GLSL_SAMPLER_DIM_BUF);
   EXPECT_EQ(tex->coord_components, 1);
   EXPECT_LT(nir_tex_instr_src_index(tex, nir_tex_src_lod), 0);
   EXPECT_EQ(binding(tex, nir_tex_src_sampler_deref), nullptr);
}

TEST_F(TgsiSample, IntegerLoadOffsetAndSwizzle)
{
   auto tex = translate(TGSI_TEXTURE_2D, 3, 0, true, true, false, true);
   ASSERT_NE(tex, nullptr);
   int offset = nir_tex_instr_src_index(tex, nir_tex_src_offset);
   ASSERT_GE(offset, 0);
   EXPECT_EQ(nir_scalar_as_int(nir_scalar_resolved(tex->src[offset].src.ssa, 0)), -1);
   EXPECT_EQ(nir_scalar_as_int(nir_scalar_resolved(tex->src[offset].src.ssa, 1)), 2);
   bool found = false;
   nir_foreach_use(use, &tex->def) {
      nir_instr *parent = nir_src_use_instr(use);
      if (parent->type != nir_instr_type_alu) continue;
      auto alu = nir_instr_as_alu(parent);
      if (alu->op != nir_op_mov) continue;
      found = true;
      for (unsigned i = 0; i < 4; ++i)
         EXPECT_EQ(alu->src[0].swizzle[i], 3 - i);
   }
   EXPECT_TRUE(found);
}

TEST_F(TgsiSample, IntegerLoadUnsignedResult)
{
   auto tex = translate(TGSI_TEXTURE_3D, 3, 0, false, false, false, true,
                         TGSI_RETURN_TYPE_UINT);
   ASSERT_NE(tex, nullptr);
   EXPECT_EQ(tex->dest_type, nir_type_uint32);
   EXPECT_EQ(tex->coord_components, 3);
}

TEST_F(TgsiSample, IntegerLoadSignedResult)
{
   auto tex = translate(TGSI_TEXTURE_1D_ARRAY, 3, 0, false, false, false, true,
                         TGSI_RETURN_TYPE_SINT);
   ASSERT_NE(tex, nullptr);
   EXPECT_EQ(tex->dest_type, nir_type_int32);
   EXPECT_EQ(tex->coord_components, 2);
}
