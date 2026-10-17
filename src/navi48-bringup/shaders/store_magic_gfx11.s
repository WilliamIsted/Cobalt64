// ---------------------------------------------------------------------------
//  store_magic_gfx11.s — store_magic for gfx1102 (Navi 33 / GC 11.0.2).
//
//  Build:   MCPU=gfx1102 tools/build-shader.sh store_magic_gfx11
//  Consumed by: the Navi 33 stage 3 step 6 compute test (MEC queue, K4/K5),
//               via src/fw/fw_shader_store_magic_gfx11.c.
//
//  Same contract with the dispatch as store_magic.s (see its header):
//  s[0:1] = result address (USER_SGPR 2), v0 = packed work-item id, one
//  wave32 of 32 lanes, grid 1x1x1, no LDS, no scratch. Lane i stores
//  0xCA11ED47 + i at result[i].
//
//  What differs from the gfx12 source:
//    * no scope:SCOPE_SYS and no global_wb (gfx12 only). The GL2 writeback
//      is the RELEASE_MEM GCR_GL2_WB after the dispatch. The GART result page
//      is MTYPE UC, so the store reaches host memory without it.
//    * the store-counter wait is s_waitcnt_vscnt, not s_wait_storecnt.
// ---------------------------------------------------------------------------

	.amdgcn_target "amdgcn-amd-amdhsa--gfx1102"

	.text
	.globl store_magic_gfx11
	.p2align 8
	.type store_magic_gfx11,@function
store_magic_gfx11:
	v_and_b32     v0, 0x3ff, v0              // v0 = tid_x (0..31)
	v_lshlrev_b32 v1, 2, v0                  // v1 = byte offset = tid * 4
	v_add_nc_u32  v2, 0xca11ed47, v0         // v2 = magic + tid

	// address = s[0:1] + zext(v1)
	global_store_b32 v1, v2, s[0:1]

	s_waitcnt_vscnt null, 0x0                // the store has left the wave
	s_endpgm

.Lstore_magic_gfx11_end:
	.size store_magic_gfx11, .Lstore_magic_gfx11_end-store_magic_gfx11

// HSA kernel descriptor: read by tools/build-shader.sh only, so that LLVM
// derives RSRC1/2/3 (see store_magic.s). Same directives as the gfx12 file.
	.rodata
	.p2align 6
	.amdhsa_kernel store_magic_gfx11
		.amdhsa_group_segment_fixed_size 0          // no LDS
		.amdhsa_private_segment_fixed_size 0        // no scratch
		.amdhsa_kernarg_size 0
		.amdhsa_next_free_vgpr 3                    // v0..v2
		.amdhsa_next_free_sgpr 2                    // s0..s1
		.amdhsa_user_sgpr_kernarg_segment_ptr 1     // s[0:1] = the buffer address
		.amdhsa_system_sgpr_workgroup_id_x 1        // RSRC2.TGID_X_EN
		.amdhsa_system_vgpr_workitem_id 0           // RSRC2.TIDIG_COMP_CNT = 0
		.amdhsa_float_round_mode_32 0
		.amdhsa_float_round_mode_16_64 0
		.amdhsa_float_denorm_mode_32 3
		.amdhsa_float_denorm_mode_16_64 3
		.amdhsa_wavefront_size32 1
		.amdhsa_workgroup_processor_mode 1          // RSRC1.WGP_MODE = 1
		.amdhsa_memory_ordered 1                    // RSRC1.MEM_ORDERED = 1
		.amdhsa_forward_progress 0
	.end_amdhsa_kernel
