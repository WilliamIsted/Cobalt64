#!/bin/bash
# preflight-gmc-smu.sh: read-only snapshot for the GMC step (G0, stage3-gmc-design.md) and the SMU
# mailboxes (S0, stage3-smu-handshake-design.md), overview repo. Every line is one `n33ctl reg` read; no
# register is written. Left out on purpose: the MMHUB invalidation-engine semaphores (e.g. 0x1a6e2),
# because reading one acquires it (gmc_v11_0.c:267), and every GC register (not before RLC autoload).
#
#   sudo tools/n33/preflight-gmc-smu.sh [n33ctl] 2>&1 | tee ~/ScratchPad/opencore-tahoe/psp2/preflight-output.txt
set -u
N33CTL="${1:-./n33ctl}"
[ -x "$N33CTL" ] || { echo "no n33ctl at $N33CTL" >&2; exit 2; }

# name  dword  (expected or meaning)
REGS="
MMHUB_FB_LOCATION_BASE      0x1a86c  measured 0x8000
MMHUB_FB_LOCATION_TOP       0x1a86d  measured 0x83ff
MMHUB_FB_OFFSET             0x1a857  measured 0
MMHUB_AGP_TOP               0x1a86e  Linux 0
MMHUB_AGP_BOT               0x1a86f  Linux 0xffffff
MMHUB_AGP_BASE              0x1a870  Linux 0
MMHUB_SYS_APERTURE_LOW      0x1a871  Linux 0x200000
MMHUB_SYS_APERTURE_HIGH     0x1a872  Linux 0x20ffbf
MMHUB_SYS_APERTURE_DEF_LSB  0x1a858  -
MMHUB_SYS_APERTURE_DEF_MSB  0x1a859  -
MMHUB_MX_L1_TLB_CNTL        0x1a873  Linux fields 0x1859 under mask 0x3ff9
MMHUB_VM_L2_CNTL            0x1a680  -
MMHUB_VM_L2_CNTL2           0x1a681  -
MMHUB_VM_L2_CNTL3           0x1a682  -
MMHUB_VM_L2_CNTL4           0x1a698  -
MMHUB_VM_L2_CNTL5           0x1a69e  -
MMHUB_L2_PROT_FAULT_CNTL    0x1a688  -
MMHUB_L2_PROT_FAULT_CNTL2   0x1a689  -
MMHUB_L2_PROT_FAULT_STATUS  0x1a68c  must be 0
MMHUB_L2_PROT_FAULT_ADDR_LO 0x1a68d  -
MMHUB_L2_PROT_FAULT_ADDR_HI 0x1a68e  -
MMHUB_L2_PROT_FAULT_DEF_LO  0x1a68f  -
MMHUB_L2_PROT_FAULT_DEF_HI  0x1a690  -
MMHUB_VM_CONTEXT0_CNTL      0x1a6c0  is VMID 0 enabled?
MMHUB_CTX0_PT_BASE_LO32     0x1a72b  -
MMHUB_CTX0_PT_BASE_HI32     0x1a72c  -
MMHUB_CTX0_PT_START_LO32    0x1a74b  -
MMHUB_CTX0_PT_START_HI32    0x1a74c  -
MMHUB_CTX0_PT_END_LO32      0x1a76b  -
MMHUB_CTX0_PT_END_HI32      0x1a76c  -
MMHUB_IDENTITY_0            0x1a692  Linux 0xffffffff
MMHUB_IDENTITY_1            0x1a693  Linux 0xf
MMHUB_IDENTITY_2            0x1a694  Linux 0
MMHUB_IDENTITY_3            0x1a695  Linux 0
MMHUB_IDENTITY_4            0x1a696  Linux 0
MMHUB_IDENTITY_5            0x1a697  Linux 0
MMHUB_INV_ENG17_ACK         0x1a706  idle
MMHUB_INV_ENG17_ADDR_LO32   0x1a707  -
MMHUB_INV_ENG17_ADDR_HI32   0x1a708  -
NBIF_HDP_REMAP_FLUSH_CNTL   0xe4d    Linux 0x7f000
NBIF_HDP_REMAP_FLUSH_REQ    0xe4e    Linux 0x7f004
NBIF_DOORBELL_APER_EN       0xde0    Linux bit 0 = 1
NBIF_BIF_FB_EN              0xe20    read and write enabled
RCC_CONFIG_MEMSIZE          0xde3    measured 0x3ff0
DCN_VM_FB_LOCATION_BASE     0x3935   snapshot, never written
DCN_VM_FB_LOCATION_TOP      0x3936   snapshot
DCN_VM_FB_OFFSET            0x3937   snapshot
DCN_VM_AGP_BOT              0x3938   snapshot
DCN_VM_AGP_TOP              0x3939   snapshot
DCN_VM_AGP_BASE             0x393a   snapshot
DCN_VM_CONTEXT0_CNTL        0x3a19   snapshot
MP1_C2PMSG_90_resp          0x1629a  1 = last message ok
MP1_C2PMSG_66_msg           0x16282  -
MP1_C2PMSG_82_arg           0x16292  -
MP1_C2PMSG_75_dbg_msg       0x1628b  -
MP1_C2PMSG_53_dbg_arg       0x16275  -
MP1_C2PMSG_54_dbg_resp      0x16276  -
MP1_C2PMSG_74_dal_msg       0x1628a  -
MP1_C2PMSG_51_dal_arg       0x16273  -
MP1_C2PMSG_52_dal_resp      0x16274  -
MP0_C2PMSG_81_sos_alive     0x16091  non-zero = sOS alive
MP0_C2PMSG_64_ring          0x16080  -
MP0_C2PMSG_67_wptr          0x16083  -
"
date
echo "$REGS" | while read -r name reg note; do
	[ -n "$name" ] || continue
	v=$("$N33CTL" reg "$reg" 2>&1 | sed -n 's/.*= //p')
	printf '%-28s %-8s %-12s %s\n' "$name" "$reg" "${v:-READ-FAILED}" "$note"
done
"$N33CTL" status
echo "AppleVTD instances in the registry (expect 0: DisableIoMapper): $(ioreg -l | grep -ic AppleVTD)"
