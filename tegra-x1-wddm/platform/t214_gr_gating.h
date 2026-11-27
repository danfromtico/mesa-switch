/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2014-2018, NVIDIA CORPORATION.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */
/* GM20B force-run GR profiles from Switchroot nvgpu 1ae0167d.
 * Only the published disable values are retained; no PMU governor is active.
 */
#ifndef T214_GR_GATING_H
#define T214_GR_GATING_H
#include <stdint.h>
static const uint32_t t214_gr_force_run[][2] = {
    /* gm20b_slcg_gr */
    {0x004041f4u, 0x03fffffeu},
    {0x0040917cu, 0x0003fffeu},
    {0x00409894u, 0x0003fffeu},
    {0x004078c4u, 0x000001feu},
    {0x00406004u, 0x0001fffeu},
    {0x00405864u, 0x000001feu},
    {0x00405910u, 0xfffffffeu},
    {0x00408044u, 0x000007feu},
    {0x00407004u, 0x0000007eu},
    {0x0041a17cu, 0x0003fffeu},
    {0x0041a894u, 0x0003fffeu},
    {0x00418504u, 0x0007fffeu},
    {0x0041860cu, 0x000001feu},
    {0x0041868cu, 0x0000001eu},
    {0x0041871cu, 0x0000003eu},
    {0x00418388u, 0x00000001u},
    {0x0041882cu, 0x0001fffeu},
    {0x00418bc0u, 0x000001feu},
    {0x00418974u, 0x0001fffeu},
    {0x00418c74u, 0xfffffffeu},
    {0x00418cf4u, 0xfffffffeu},
    {0x00418d74u, 0xfffffffeu},
    {0x00418f10u, 0xfffffffeu},
    {0x00418e10u, 0xfffffffeu},
    {0x00419024u, 0x000001feu},
    {0x0041889cu, 0x000001feu},
    {0x00419d64u, 0x000001ffu},
    {0x00419a44u, 0x0000000eu},
    {0x00419a4cu, 0x000001feu},
    {0x00419a54u, 0x0000003eu},
    {0x00419a5cu, 0x0000000eu},
    {0x00419a64u, 0x000001feu},
    {0x00419a6cu, 0x0000000eu},
    {0x00419a74u, 0x0000000eu},
    {0x00419a7cu, 0x0000003eu},
    {0x00419a84u, 0x0000000eu},
    {0x0041986cu, 0x00fffffeu},
    {0x00419cd8u, 0x001ffffeu},
    {0x00419ce0u, 0x001ffffeu},
    {0x00419c74u, 0x0000001eu},
    {0x00419fd4u, 0x0003fffeu},
    {0x00419fdcu, 0xfffffffeu},
    {0x00419fe4u, 0x00001ffeu},
    {0x00419ff4u, 0x00003ffeu},
    {0x00419ffcu, 0x0001fffeu},
    {0x0041be2cu, 0xfffffffeu},
    {0x0041bfecu, 0xfffffffeu},
    {0x0041bed4u, 0xfffffffeu},
    {0x00408814u, 0x0001fffeu},
    {0x0040881cu, 0x0001fffeu},
    {0x00408a84u, 0x0001fffeu},
    {0x00408a8cu, 0x0001fffeu},
    {0x00408a94u, 0x0001fffeu},
    {0x00408a9cu, 0x0001fffeu},
    {0x00408aa4u, 0x0001fffeu},
    {0x00408aacu, 0x0001fffeu},
    {0x004089acu, 0x0001fffeu},
    {0x00408a24u, 0x000001ffu},
    /* gm20b_blcg_gr */
    {0x004041f0u, 0x00000000u},
    {0x00409890u, 0x00000000u},
    {0x004098b0u, 0x00000000u},
    {0x004078c0u, 0x00000000u},
    {0x00406000u, 0x00000000u},
    {0x00405860u, 0x00000000u},
    {0x0040590cu, 0x00000000u},
    {0x00408040u, 0x00000000u},
    {0x00407000u, 0x00000000u},
    {0x00405bf0u, 0x00000000u},
    {0x0041a890u, 0x00000000u},
    {0x0041a8b0u, 0x00000000u},
    {0x00418500u, 0x00000000u},
    {0x00418608u, 0x00000000u},
    {0x00418688u, 0x00000000u},
    {0x00418718u, 0x00000000u},
    {0x00418828u, 0x00000000u},
    {0x00418bbcu, 0x00000000u},
    {0x00418970u, 0x00000000u},
    {0x00418c70u, 0x00000000u},
    {0x00418cf0u, 0x00000000u},
    {0x00418d70u, 0x00000000u},
    {0x00418f0cu, 0x00000000u},
    {0x00418e0cu, 0x00000000u},
    {0x00419020u, 0x00000000u},
    {0x00419038u, 0x00000000u},
    {0x00418898u, 0x00000000u},
    {0x00419a40u, 0x00000000u},
    {0x00419a48u, 0x00000000u},
    {0x00419a50u, 0x00000000u},
    {0x00419a58u, 0x00000000u},
    {0x00419a60u, 0x00000000u},
    {0x00419a68u, 0x00000000u},
    {0x00419a70u, 0x00000000u},
    {0x00419a78u, 0x00000000u},
    {0x00419a80u, 0x00000000u},
    {0x00419868u, 0x00000000u},
    {0x00419cd4u, 0x00000000u},
    {0x00419cdcu, 0x00000000u},
    {0x00419c70u, 0x00000000u},
    {0x00419fd0u, 0x00000000u},
    {0x00419fd8u, 0x00000000u},
    {0x00419fe0u, 0x00000000u},
    {0x00419fe8u, 0x00000000u},
    {0x00419ff0u, 0x00000000u},
    {0x00419ff8u, 0x00000000u},
    {0x00419f90u, 0x00000000u},
    {0x0041be28u, 0x00000000u},
    {0x0041bfe8u, 0x00000000u},
    {0x0041bed0u, 0x00000000u},
    {0x00408810u, 0x00000000u},
    {0x00408818u, 0x00000000u},
    {0x00408a80u, 0x00000000u},
    {0x00408a88u, 0x00000000u},
    {0x00408a90u, 0x00000000u},
    {0x00408a98u, 0x00000000u},
    {0x00408aa0u, 0x00000000u},
    {0x00408aa8u, 0x00000000u},
    {0x004089a8u, 0x00000000u},
    {0x004089b0u, 0x00000000u},
    {0x004089b8u, 0x00000000u},
};
#endif
