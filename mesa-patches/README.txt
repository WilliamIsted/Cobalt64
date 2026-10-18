Mesa patches (RADV Darwin / macOS port for the Navi 48 native kext)
===================================================================

These patches add a macOS backend to Mesa's RADV Vulkan driver so that it
talks to the Navi48 bring-up kext (user-client ABI "N48N") instead of a DRM
node. Only our changes are included, not the Mesa tree.

Upstream base (apply on top of exactly this commit):
    mesa  f5cb8ee032adabef599ae892ec4e42d796b84da9   (VERSION 26.3.0-devel)
    https://gitlab.freedesktop.org/mesa/mesa.git

Licence: Mesa is MIT licensed. These patches are changes to Mesa and are
released under the same licence (see Mesa's docs/license.rst and
third-party/THIRD-PARTY-LICENSES.txt).

Apply:
    git clone https://gitlab.freedesktop.org/mesa/mesa.git
    cd mesa
    git checkout f5cb8ee032adabef599ae892ec4e42d796b84da9
    git am /path/to/Navi48-MacOS/mesa-patches/*.patch
(The patch author and date headers are placeholders; "git am" works as-is,
or use "git apply" / "patch -p1" in order.)

Order matters: 0001 .. 0013.

0001 .. 0009 are the Navi 48 (gfx1201) port. 0010 .. 0013 retarget it to the
Navi 33 build of this kext (RX 7600 XT, gfx1102, the navi33-stage1 branch):
    0010  canned device navi33 (gfx1102), GB_ADDR_CONFIG 0x343
    0011  mock answers BoImportHost (selector 21)
    0012  one compute ring (the kext runs MEC ring 0 only); mock BO trace
    0013  mock seqnos start at 0x1000; mock traces Ctx, GemVA, Submit
          (header, IBs, PM4 opcodes), WaitSeq and CPU maps
Applied in order they reproduce branch navi33-w3 of the Mesa tree used for
W3/W4 (checked 2026-10-18: no diff). For compute-only use run RADV with
RADV_QUEUE_DISABLE=gfx,sparse; RADV_DARWIN_MOCK=1 selects the in-process mock
(no kext), RADV_DARWIN_TRACE=1 logs every kext call. The kext ABI header that patches 0002-0004 mirror
is src/navi48-bringup/src/Navi48NativeABI.h in this repository; Mesa's copy and
the kext's copy must be the same ABI version (currently 1.11 or newer).
