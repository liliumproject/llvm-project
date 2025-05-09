// NOTE: Assertions have been autogenerated by utils/update_mc_test_checks.py UTC_ARGS: --unique --version 5
// RUN: llvm-mc -triple=amdgcn -show-encoding -mcpu=gfx1200 %s | FileCheck --check-prefix=GFX12 %s

// Subtargets allow src1 of VOP3 DPP instructions to be SGPR or inlinable
// constant.

v_add3_u32_e64_dpp v5, v1, s2, v3 quad_perm:[3,2,1,0] row_mask:0xf bank_mask:0xf
// GFX12: v_add3_u32_e64_dpp v5, v1, s2, v3 quad_perm:[3,2,1,0] row_mask:0xf bank_mask:0xf ; encoding: [0x05,0x00,0x55,0xd6,0xfa,0x04,0x0c,0x04,0x01,0x1b,0x00,0xff]

v_add3_u32_e64_dpp v5, v1, 42, v3 quad_perm:[3,2,1,0] row_mask:0xf bank_mask:0xf
// GFX12: v_add3_u32_e64_dpp v5, v1, 42, v3 quad_perm:[3,2,1,0] row_mask:0xf bank_mask:0xf ; encoding: [0x05,0x00,0x55,0xd6,0xfa,0x54,0x0d,0x04,0x01,0x1b,0x00,0xff]

v_add3_u32_e64_dpp v5, v1, s2, v0 dpp8:[7,6,5,4,3,2,1,0]
// GFX12: v_add3_u32_e64_dpp v5, v1, s2, v0 dpp8:[7,6,5,4,3,2,1,0] ; encoding: [0x05,0x00,0x55,0xd6,0xe9,0x04,0x00,0x04,0x01,0x77,0x39,0x05]

v_add3_u32_e64_dpp v5, v1, 42, v0 dpp8:[7,6,5,4,3,2,1,0]
// GFX12: v_add3_u32_e64_dpp v5, v1, 42, v0 dpp8:[7,6,5,4,3,2,1,0] ; encoding: [0x05,0x00,0x55,0xd6,0xe9,0x54,0x01,0x04,0x01,0x77,0x39,0x05]

v_add3_u32_e64_dpp v5, v1, s2, s3 dpp8:[7,6,5,4,3,2,1,0]
// GFX12: v_add3_u32_e64_dpp v5, v1, s2, s3 dpp8:[7,6,5,4,3,2,1,0] ; encoding: [0x05,0x00,0x55,0xd6,0xe9,0x04,0x0c,0x00,0x01,0x77,0x39,0x05]

v_cmp_ne_i32_e64_dpp vcc_lo, v1, s2 dpp8:[7,6,5,4,3,2,1,0]
// GFX12: v_cmp_ne_i32_e64_dpp vcc_lo, v1, s2 dpp8:[7,6,5,4,3,2,1,0] ; encoding: [0x6a,0x00,0x45,0xd4,0xe9,0x04,0x00,0x00,0x01,0x77,0x39,0x05]

v_cmp_le_f32 vcc_lo, v1, v2 row_mirror
// GFX12: v_cmp_le_f32 vcc_lo, v1, v2 row_mirror row_mask:0xf bank_mask:0xf ; encoding: [0xfa,0x04,0x26,0x7c,0x01,0x40,0x01,0xff]

v_cmp_eq_f32_e64_dpp s5, v1, s99 row_mirror
// GFX12: v_cmp_eq_f32_e64_dpp s5, v1, s99 row_mirror row_mask:0xf bank_mask:0xf ; encoding: [0x05,0x00,0x12,0xd4,0xfa,0xc6,0x00,0x00,0x01,0x40,0x01,0xff]

v_cmp_eq_f32_e64_dpp s5, v1, s99 row_half_mirror
// GFX12: v_cmp_eq_f32_e64_dpp s5, v1, s99 row_half_mirror row_mask:0xf bank_mask:0xf ; encoding: [0x05,0x00,0x12,0xd4,0xfa,0xc6,0x00,0x00,0x01,0x41,0x01,0xff]

v_cmp_eq_f32_e64_dpp s5, v1, s99 row_shl:15
// GFX12: v_cmp_eq_f32_e64_dpp s5, v1, s99 row_shl:15 row_mask:0xf bank_mask:0xf ; encoding: [0x05,0x00,0x12,0xd4,0xfa,0xc6,0x00,0x00,0x01,0x0f,0x01,0xff]

v_cmp_eq_f32_e64_dpp s5, v1, s99 row_shr:1
// GFX12: v_cmp_eq_f32_e64_dpp s5, v1, s99 row_shr:1 row_mask:0xf bank_mask:0xf ; encoding: [0x05,0x00,0x12,0xd4,0xfa,0xc6,0x00,0x00,0x01,0x11,0x01,0xff]

v_cmp_eq_f32_e64_dpp s5, v1, s99 row_ror:1
// GFX12: v_cmp_eq_f32_e64_dpp s5, v1, s99 row_ror:1 row_mask:0xf bank_mask:0xf ; encoding: [0x05,0x00,0x12,0xd4,0xfa,0xc6,0x00,0x00,0x01,0x21,0x01,0xff]

v_cmp_eq_f32_e64_dpp vcc_hi, |v1|, -s99 row_share:15 row_mask:0x0 bank_mask:0x1
// GFX12: v_cmp_eq_f32_e64_dpp vcc_hi, |v1|, -s99 row_share:15 row_mask:0x0 bank_mask:0x1 ; encoding: [0x6b,0x01,0x12,0xd4,0xfa,0xc6,0x00,0x40,0x01,0x5f,0x01,0x01]

v_cmp_eq_f32_e64_dpp ttmp15, -v1, |s99| row_xmask:0 row_mask:0x1 bank_mask:0x3 bound_ctrl:1 fi:0
// GFX12: v_cmp_eq_f32_e64_dpp ttmp15, -v1, |s99| row_xmask:0 row_mask:0x1 bank_mask:0x3 bound_ctrl:1 ; encoding: [0x7b,0x02,0x12,0xd4,0xfa,0xc6,0x00,0x20,0x01,0x60,0x09,0x13]

v_cmpx_gt_f32_e64_dpp v255, 4.0 dpp8:[0,0,0,0,0,0,0,0] fi:0
// GFX12: v_cmpx_gt_f32_e64_dpp v255, 4.0 dpp8:[0,0,0,0,0,0,0,0] ; encoding: [0x7e,0x00,0x94,0xd4,0xe9,0xec,0x01,0x00,0xff,0x00,0x00,0x00]

// Elements of CPol operand can be given in any order

image_load v0, v0, s[0:7] dmask:0x1 dim:SQ_RSRC_IMG_1D th:TH_LOAD_HT scope:SCOPE_SE
// GFX12: image_load v0, v0, s[0:7] dmask:0x1 dim:SQ_RSRC_IMG_1D th:TH_LOAD_HT scope:SCOPE_SE ; encoding: [0x00,0x00,0x40,0xd0,0x00,0x00,0x24,0x00,0x00,0x00,0x00,0x00]

image_load v0, v0, s[0:7] dmask:0x1 dim:SQ_RSRC_IMG_1D scope:SCOPE_SE th:TH_LOAD_HT
// GFX12: image_load v0, v0, s[0:7] dmask:0x1 dim:SQ_RSRC_IMG_1D th:TH_LOAD_HT scope:SCOPE_SE ; encoding: [0x00,0x00,0x40,0xd0,0x00,0x00,0x24,0x00,0x00,0x00,0x00,0x00]

image_sample v[29:30], [v31, v32, v33], s[32:39], s[68:71] dmask:0x3 dim:SQ_RSRC_IMG_3D th:TH_LOAD_NT scope:SCOPE_SYS
// GFX12: image_sample v[29:30], [v31, v32, v33], s[32:39], s[68:71] dmask:0x3 dim:SQ_RSRC_IMG_3D th:TH_LOAD_NT scope:SCOPE_SYS ; encoding: [0x02,0xc0,0xc6,0xe4,0x1d,0x40,0x1c,0x22,0x1f,0x20,0x21,0x00]

image_sample v[29:30], [v31, v32, v33], s[32:39], s[68:71] dmask:0x3 dim:SQ_RSRC_IMG_3D scope:SCOPE_SYS th:TH_LOAD_NT
// GFX12: image_sample v[29:30], [v31, v32, v33], s[32:39], s[68:71] dmask:0x3 dim:SQ_RSRC_IMG_3D th:TH_LOAD_NT scope:SCOPE_SYS ; encoding: [0x02,0xc0,0xc6,0xe4,0x1d,0x40,0x1c,0x22,0x1f,0x20,0x21,0x00]

global_load_block v[9:40], v[5:6], off th:TH_LOAD_HT scope:SCOPE_SE
// GFX12: global_load_block v[9:40], v[5:6], off th:TH_LOAD_HT scope:SCOPE_SE ; encoding: [0x7c,0xc0,0x14,0xee,0x09,0x00,0x24,0x00,0x05,0x00,0x00,0x00]

global_load_block v[9:40], v[5:6], off scope:SCOPE_SE th:TH_LOAD_HT
// GFX12: global_load_block v[9:40], v[5:6], off th:TH_LOAD_HT scope:SCOPE_SE ; encoding: [0x7c,0xc0,0x14,0xee,0x09,0x00,0x24,0x00,0x05,0x00,0x00,0x00]

buffer_load_b32 v5, off, s[8:11], s3 offset:8388607 th:TH_LOAD_NT_HT scope:SCOPE_DEV
// GFX12: buffer_load_b32 v5, off, s[8:11], s3 offset:8388607 th:TH_LOAD_NT_HT scope:SCOPE_DEV ; encoding: [0x03,0x00,0x05,0xc4,0x05,0x10,0xe8,0x00,0x00,0xff,0xff,0x7f]

buffer_load_b32 v5, off, s[8:11], s3 offset:8388607 scope:SCOPE_DEV th:TH_LOAD_NT_HT
// GFX12: buffer_load_b32 v5, off, s[8:11], s3 offset:8388607 th:TH_LOAD_NT_HT scope:SCOPE_DEV ; encoding: [0x03,0x00,0x05,0xc4,0x05,0x10,0xe8,0x00,0x00,0xff,0xff,0x7f]

tbuffer_load_d16_format_x v4, off, ttmp[4:7], s3 format:[BUF_FMT_8_UINT] offset:8388607 th:TH_LOAD_BYPASS scope:SCOPE_SYS
// GFX12: tbuffer_load_d16_format_x v4, off, ttmp[4:7], s3 format:[BUF_FMT_8_UINT] offset:8388607 th:TH_LOAD_BYPASS scope:SCOPE_SYS ; encoding: [0x03,0x00,0x22,0xc4,0x04,0xe0,0xbc,0x02,0x00,0xff,0xff,0x7f]

tbuffer_load_d16_format_x v4, off, ttmp[4:7], s3 format:[BUF_FMT_8_UINT] offset:8388607 scope:SCOPE_SYS th:TH_LOAD_BYPASS
// GFX12: tbuffer_load_d16_format_x v4, off, ttmp[4:7], s3 format:[BUF_FMT_8_UINT] offset:8388607 th:TH_LOAD_BYPASS scope:SCOPE_SYS ; encoding: [0x03,0x00,0x22,0xc4,0x04,0xe0,0xbc,0x02,0x00,0xff,0xff,0x7f]
