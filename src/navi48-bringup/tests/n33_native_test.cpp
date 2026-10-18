// n33_native_test.cpp: host test of amd/n33_native_pure.h (stage 3 step 7 W3: the N48N user-client subset on the N33
// build - Hello, QueryInfo, ReadRegs, Ctx, GemVa, Submit and the fence check).
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I src/navi48-bringup/src -I src/navi48-bringup/src/amd src/navi48-bringup/tests/n33_native_test.cpp \
//       -o $SCRATCH/n33_native_test && $SCRATCH/n33_native_test [<linux tree root>]
// Part 1 is the pure logic against the design and the ABI. Part 2, given the pinned Linux tree's root (3857c2fe), pins
// AMDGPU_HW_IP_COMPUTE / _GFX to the IP type the Submit parser accepts. The build version is pinned to Info.plist.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include "n33_native_pure.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

using namespace n33n;

// A VM with one R|X mapping covering [mapVa, mapVa + mapBytes) and a VRAM BO slab-style record, so the Submit and fence
// checks have real state. The mapping handle is 1, the fence BO handle is 2 (4 pages).
static void make_state(n33vm::State &vm, n33bo::State &bo, uint64_t mapVa, uint64_t mapBytes) {
	vm = n33vm::State{};
	vm.live = true;
	n33vm::record_map(vm, /*handle*/1, n33vm::kVmDefault, mapVa, mapBytes, /*off*/0);   // R|W|X
	bo = n33bo::State{};
	bo.bo[1] = {};                          // handle 2 (index 1): a live 4-page BO for the fence
	bo.bo[1].live = true; bo.bo[1].pages = 4;   // 16384 bytes
}

static void hello_cases() {
	uint64_t o[4] = {};
	CHECK(hello_check(N48N_ABI_VERSION, 0) == kOk);
	CHECK(hello_check(N48N_ABI_VERSION, N48N_HELLO_F_MINOR) == kOk);
	CHECK(hello_check(N48N_ABI_VERSION, 1u) == kBadArg);                 // T1 leg-1 flag, refused
	CHECK(hello_check(N48N_ABI_VERSION + 1, 0) == kUnsupported);
	hello_fill(0, kVmid, o);
	CHECK(o[0] == N48N_ABI_VERSION && o[1] == kKextBuild && o[2] == N48N_MAX_IBS && o[3] == 8);
	hello_fill(N48N_HELLO_F_MINOR, kVmid, o);
	CHECK(o[0] == ((uint64_t)N48N_ABI_VERSION | ((uint64_t)N48N_ABI_MINOR << 16)));
}

static void info_cases() {
	CHECK(info_ranges_ok());                                            // the reported ranges contain the navi33 row
	n48n_info info; memset(&info, 0, sizeof info);
	InfoLive L{};
	L.gb_addr_config = 0x343u; L.ring_size_dw = 1024u; L.info_flags = N48N_INFO_NATIVE_BOOT | N48N_INFO_S1B_POSITIVE;
	L.vram_vis_total = 256ull << 20; L.vram_vis_free = 200ull << 20;
	L.gtt_cap = 512ull << 20; L.gtt_max_bo = 1ull << 20; L.pt_cap = 8ull << 20;
	L.seq_emitted = 5; L.seq_retired = 4;
	info_fill(&info, L);
	CHECK(info.abi_version == N48N_ABI_VERSION && info.kext_build == 674);
	CHECK(info.gb_addr_config == 0x343u);
	CHECK(info.gc_version == 0x000B0002u);                              // gfx11.0.2
	CHECK(info.vmid == 8u && info.max_ibs == N48N_MAX_IBS);
	CHECK(info.va_low_first == 0x10000ull && info.va_low_last == 0x7FFFFFFFFFFFull);
	CHECK(info.va_high_first == 0xFFFF800000000000ull && info.va_high_last == 0xFFFFFFFFFFFFFFFFull);
	// the Darwin backend's predicate (ac_darwin_drm.c:163-174) over exactly these reported ranges:
	CHECK(n33vm::kRowVaOffset >= info.va_low_first && (n33vm::kRowVaMax - 1) <= info.va_low_last);
	CHECK(n33vm::kRowHighOffset >= info.va_high_first && (n33vm::kRowHighMax - 1) <= info.va_high_last);
	CHECK(info.max_bos == N48N_MAX_BOS && info.fence_slots == N48N_FENCE_SLOTS);
	CHECK(info.reserved[2] == N48N_ABI_MINOR);
	CHECK(info.vram_vis_total == (256ull << 20) && info.seq_emitted == 5 && info.seq_retired == 4);
}

static void read_regs_cases() {
	CHECK(read_regs_check(0x263eu, 1, 0xFFFFFFFFull) == kOk);           // GB_ADDR_CONFIG
	CHECK(read_regs_check(0x2249u, 1, 0xFFFFFFFFull) == kOk);           // CC_GC_SA_UNIT_DISABLE
	CHECK(read_regs_check(0x226fu, 1, 0xFFFFFFFFull) == kOk);           // CC_GC_SHADER_ARRAY_CONFIG
	CHECK(read_regs_check(0x263du, 2, 0xFFFFFFFFull) == kOk);           // 0x263d..0x263e both allowed
	CHECK(read_regs_check(0x263eu, 2, 0xFFFFFFFFull) == kBadArg);       // 0x263f not in the set
	CHECK(read_regs_check(0x1000u, 1, 0xFFFFFFFFull) == kBadArg);       // outside the allow-list
	CHECK(read_regs_check(0x263eu, 0, 0xFFFFFFFFull) == kBadArg);       // count 0
	CHECK(read_regs_check(0x263eu, 17, 0xFFFFFFFFull) == kBadArg);      // count > 16
	CHECK(read_regs_check(0x263eu, 1, 0) == kBadArg);                   // instance must be 0xFFFFFFFF
}

static void ctx_cases() {
	Client c{}; n48n_ctx in{}, out{};
	in.op = N48N_CTX_OP_ALLOC;
	CHECK(ctx_op(c, &in, &out) == kOk && out.ctx_id == 1u);
	CHECK(ctx_op(c, &in, &out) == kOk && out.ctx_id == 2u);
	CHECK(ctx_valid(c, 1) && ctx_valid(c, 2) && !ctx_valid(c, 3));
	in.op = N48N_CTX_OP_QUERY_STATE2; in.ctx_id = 1u;
	CHECK(ctx_op(c, &in, &out) == kOk && out.ctx_id == 0u);            // no reset / guilty
	in.op = N48N_CTX_OP_FREE; in.ctx_id = 1u;
	CHECK(ctx_op(c, &in, &out) == kOk && !ctx_valid(c, 1));
	in.ctx_id = 1u; CHECK(ctx_op(c, &in, &out) == kBadArg);           // double free
	in.op = 3u; CHECK(ctx_op(c, &in, &out) == kBadArg);              // unknown op
	// exhaust the 64 ids
	Client c2{}; in.op = N48N_CTX_OP_ALLOC;
	for (uint32_t i = 0; i < N48N_MAX_CTX; i++) CHECK(ctx_op(c2, &in, &out) == kOk);
	CHECK(ctx_op(c2, &in, &out) == kNoResources);
}

static void gemva_cases() {
	uint32_t op = 0, f = 0;
	n48n_gem_va v{};
	v.operation = N48N_VA_OP_MAP; v.flags = N48N_VM_PAGE_READABLE | N48N_VM_PAGE_WRITEABLE | N48N_VM_PAGE_EXECUTABLE;
	CHECK(gemva_check(&v, &op, &f) == kOk && op == N48N_VA_OP_MAP && f == (n33vm::kVmRead | n33vm::kVmWrite | n33vm::kVmExec));
	v.operation = N48N_VA_OP_UNMAP;
	CHECK(gemva_check(&v, &op, &f) == kOk && op == N48N_VA_OP_UNMAP);
	v.operation = N48N_VA_OP_CLEAR;                                    // Unsupported op
	CHECK(gemva_check(&v, &op, &f) == kUnsupported);
	v.operation = N48N_VA_OP_MAP; v.flags = N48N_VM_PAGE_PRT | N48N_VM_PAGE_READABLE;   // PRT bit -> refused
	CHECK(gemva_check(&v, &op, &f) == kBadArg);
	v.flags = N48N_VM_PAGE_READABLE; v._pad = 1u;                      // reserved pad set
	CHECK(gemva_check(&v, &op, &f) == kBadArg);
	v._pad = 0; v.num_syncobj_handles = 1u;                            // syncobj field set
	CHECK(gemva_check(&v, &op, &f) == kBadArg);
	// MTYPE passes through
	v = n48n_gem_va{}; v.operation = N48N_VA_OP_MAP;
	v.flags = N48N_VM_PAGE_READABLE | N48N_VM_PAGE_WRITEABLE | N48N_VM_MTYPE_UC;
	CHECK(gemva_check(&v, &op, &f) == kOk && n33vm::vm_mtype(f) == n33vm::kMtypeUc);
}

static void fence_cases() {
	n33vm::State vm; n33bo::State bo; make_state(vm, bo, 0x100000ull, 0x4000ull);
	CHECK(fence_check(bo, 2, 0) == kOk);
	CHECK(fence_check(bo, 2, 16376) == kOk);                           // last 8-byte slot in a 16384 B BO
	CHECK(fence_check(bo, 2, 16384) == kBadArg);                       // past the end
	CHECK(fence_check(bo, 2, 4) == kBadArg);                           // not 8-aligned
	CHECK(fence_check(bo, 1, 0) == kBadArg);                           // handle 1 not live
	CHECK(fence_check(bo, 9, 0) == kBadArg);                           // handle out of range
}

// Build a submit blob: header + num_ibs IBs, each a compute IB at va/bytes.
static uint32_t build_submit(uint8_t *buf, uint32_t numIbs, uint32_t ctxId, uint32_t flags, uint32_t fh, uint32_t fo,
                             const SubmitIb *ibs, const uint32_t *ipTypes, const uint32_t *ibFlags) {
	n48n_cs_in hdr{}; hdr.abi = N48N_ABI_VERSION; hdr.ctx_id = ctxId; hdr.num_ibs = numIbs; hdr.flags = flags;
	hdr.fence_handle = fh; hdr.fence_offset = fo;
	memcpy(buf, &hdr, sizeof hdr);
	for (uint32_t i = 0; i < numIbs; i++) {
		n48n_cs_ib ib{}; ib.va_start = ibs[i].va; ib.ib_bytes = ibs[i].dw * 4u;
		ib.ip_type = ipTypes ? ipTypes[i] : kIpCompute; ib.flags = ibFlags ? ibFlags[i] : 0u;
		memcpy(buf + 32u + (size_t)i * 32u, &ib, sizeof ib);
	}
	return 32u + 32u * numIbs;
}

static void submit_cases() {
	const uint64_t mapVa = 0x200000ull, mapBytes = 0x10000ull;         // 64 KiB mapping, R|W|X
	n33vm::State vm; n33bo::State bo; make_state(vm, bo, mapVa, mapBytes);
	Client c{}; c.hello = true;
	uint8_t buf[32 + 32 * N48N_MAX_IBS]; SubmitReq req{};
	SubmitIb one = { mapVa, 64u };                                     // 256-byte IB inside the mapping

	uint32_t sz = build_submit(buf, 1, /*ctx*/1, 0, 0, 0, &one, nullptr, nullptr);
	CHECK(submit_check(buf, sz, vm, bo, true, c, &req) == kBadArg);   // ctx 1 not allocated
	c.ctxUsed[1] = 1;
	CHECK(submit_check(buf, sz, vm, bo, true, c, &req) == kOk);
	CHECK(req.numIbs == 1 && req.ib[0].va == mapVa && req.ib[0].dw == 64u && !req.hasFence && req.ctxId == 1u);

	// not Hello / hung / no VM
	Client c0{}; CHECK(submit_check(buf, sz, vm, bo, true, c0, &req) == kNotReady);
	Client ch = c; ch.hung = true; CHECK(submit_check(buf, sz, vm, bo, true, ch, &req) == kNotReady);
	CHECK(submit_check(buf, sz, vm, bo, false, c, &req) == kNotReady);   // vmLive false

	// a fence
	sz = build_submit(buf, 1, 1, N48N_CS_HAS_FENCE, /*fh*/2, /*fo*/0, &one, nullptr, nullptr);
	CHECK(submit_check(buf, sz, vm, bo, true, c, &req) == kOk && req.hasFence && req.fenceHandle == 2u);
	sz = build_submit(buf, 1, 1, N48N_CS_HAS_FENCE, /*fh*/2, /*fo*/5, &one, nullptr, nullptr);   // unaligned fence
	CHECK(submit_check(buf, sz, vm, bo, true, c, &req) == kBadArg);
	sz = build_submit(buf, 1, 1, 0, /*fh*/2, 0, &one, nullptr, nullptr);   // fence fields set without the flag
	CHECK(submit_check(buf, sz, vm, bo, true, c, &req) == kBadArg);

	// gfx ip_type refused
	uint32_t gfx = kIpGfx; sz = build_submit(buf, 1, 1, 0, 0, 0, &one, &gfx, nullptr);
	CHECK(submit_check(buf, sz, vm, bo, true, c, &req) == kBadArg);

	// an IB straddling the end of the mapping
	SubmitIb over = { mapVa + mapBytes - 128u, 64u };                 // 256 bytes from 128 before the end -> overruns
	sz = build_submit(buf, 1, 1, 0, 0, 0, &over, nullptr, nullptr);
	CHECK(submit_check(buf, sz, vm, bo, true, c, &req) == kBadArg);

	// an IB entirely outside any mapping
	SubmitIb out = { 0x900000ull, 64u };
	sz = build_submit(buf, 1, 1, 0, 0, 0, &out, nullptr, nullptr);
	CHECK(submit_check(buf, sz, vm, bo, true, c, &req) == kBadArg);

	// an IB that needs EXECUTE: a read-only mapping is refused
	n33vm::State vmro; n33bo::State boro; make_state(vmro, boro, mapVa, mapBytes);
	vmro.map[0].vmFlags = n33vm::kVmRead;                             // R only
	sz = build_submit(buf, 1, 1, 0, 0, 0, &one, nullptr, nullptr);
	CHECK(submit_check(buf, sz, vmro, boro, true, c, &req) == kBadArg);

	// zero-byte and misaligned IBs
	SubmitIb zero = { mapVa, 0u }; sz = build_submit(buf, 1, 1, 0, 0, 0, &zero, nullptr, nullptr);
	CHECK(submit_check(buf, sz, vm, bo, true, c, &req) == kBadArg);
	SubmitIb odd = { mapVa + 2u, 64u }; sz = build_submit(buf, 1, 1, 0, 0, 0, &odd, nullptr, nullptr);
	CHECK(submit_check(buf, sz, vm, bo, true, c, &req) == kBadArg);    // va not 4-aligned

	// disallowed IB flag
	uint32_t badf = N48N_IB_FLAG_SECURE; sz = build_submit(buf, 1, 1, 0, 0, 0, &one, nullptr, &badf);
	CHECK(submit_check(buf, sz, vm, bo, true, c, &req) == kBadArg);
	uint32_t okf = N48N_IB_FLAG_PREAMBLE; sz = build_submit(buf, 1, 1, 0, 0, 0, &one, nullptr, &okf);
	CHECK(submit_check(buf, sz, vm, bo, true, c, &req) == kOk);

	// two IBs, the second bad -> whole submit refused
	SubmitIb two[2] = { one, out }; sz = build_submit(buf, 2, 1, 0, 0, 0, two, nullptr, nullptr);
	CHECK(submit_check(buf, sz, vm, bo, true, c, &req) == kBadArg);

	// size inconsistent with num_ibs
	sz = build_submit(buf, 1, 1, 0, 0, 0, &one, nullptr, nullptr);
	CHECK(submit_check(buf, sz + 32u, vm, bo, true, c, &req) == kBadArg);   // trailing extra IB bytes, header says 1

	// a high-half VA, as Mesa's W3 test uses (0xffff8001_00000000), with no fence BO (the common path)
	const uint64_t hiVa = 0xFFFF800100000000ull;
	n33vm::State vmhi; n33bo::State bohi; make_state(vmhi, bohi, hiVa, mapBytes);
	SubmitIb hiIb = { hiVa, 64u };
	sz = build_submit(buf, 1, 1, 0, 0, 0, &hiIb, nullptr, nullptr);
	CHECK(submit_check(buf, sz, vmhi, bohi, true, c, &req) == kOk && req.ib[0].va == hiVa && !req.hasFence);
}

static void gate_cases() {
	CHECK(may_open(/*cpReady*/true, /*vmLive*/true, /*already*/false) == kOk);
	CHECK(may_open(false, true, false) == kNotReady);         // K1-K3 not run
	CHECK(may_open(true, false, false) == kNotReady);         // no W2V VM
	CHECK(may_open(true, true, true) == kExclusive);          // a client is already open
}

static void domain_cases() {
	uint32_t k = 0xFF;
	CHECK(domain_to_kind(N48N_GEM_DOMAIN_VRAM, &k) == kOk && k == n33bo::kVram);
	CHECK(domain_to_kind(N48N_GEM_DOMAIN_GTT, &k) == kOk && k == n33bo::kGtt);
	CHECK(domain_to_kind(N48N_GEM_DOMAIN_VRAM | N48N_GEM_DOMAIN_GTT, &k) == kOk && k == n33bo::kVram);   // VRAM preferred
	CHECK(domain_to_kind(N48N_GEM_DOMAIN_CPU, &k) == kBadArg);
	CHECK(domain_to_kind(0, &k) == kBadArg);
}

static void ring_cases() {
	// two IBs at high-half VAs, a caller fence MC: ACQUIRE_MEM + IB per IB, one RELEASE_MEM at the end.
	SubmitReq req{}; req.numIbs = 2;
	req.ib[0] = { 0xFFFF800100000000ull, 64u };
	req.ib[1] = { 0xFFFF800100010000ull, 40u };
	const uint64_t fenceMc = 0xC000000ull + 0x40;
	const uint32_t seq = 7;
	uint32_t p[n33cp::kSubmitDw]; memset(p, 0, sizeof p);
	const uint32_t n = ring_build(p, req, fenceMc, seq);
	CHECK(n == 2u * 12u + 8u);                                 // 2*(acquire 8 + ib 4) + release 8 = 32
	for (uint32_t i = 0; i < 2; i++) {
		const uint32_t a = i * 12u;                            // ACQUIRE_MEM
		CHECK(p[a] == n33cp::pkt3(n33cp::kOpAcquireMem, 6));
		const uint32_t b = a + 8u;                             // INDIRECT_BUFFER
		CHECK(p[b] == n33cp::pkt3(n33cp::kOpIndirectBuffer, 2));
		CHECK(p[b + 1] == (n33cp::lo32(req.ib[i].va) & ~3u) && p[b + 2] == n33cp::hi32(req.ib[i].va));
		CHECK(((p[b + 3] >> 24) & 0xFu) == kVmid);             // INDIRECT_BUFFER runs in VMID 8
		CHECK((p[b + 3] & 0xFFFFFu) == req.ib[i].dw);          // the caller's IB size
	}
	const uint32_t r = 2u * 12u;                               // RELEASE_MEM
	CHECK(p[r] == n33cp::pkt3(n33cp::kOpReleaseMem, 6));
	CHECK((p[r + 1] & n33cp::kGl2Wb) != 0u);                   // GL2_WB before the fence
	CHECK(p[r + 3] == (n33cp::lo32(fenceMc) & ~3u) && p[r + 4] == n33cp::hi32(fenceMc) && p[r + 5] == seq);
	// the ring never overflows the submit buffer, even at the cap
	CHECK(kMaxRingIbs * 12u + 8u <= n33cp::kSubmitDw);
}

static void submit_cap_case() {
	const uint64_t mapVa = 0x200000ull;
	n33vm::State vm; n33bo::State bo; make_state(vm, bo, mapVa, 0x10000ull);
	Client c{}; c.hello = true; c.ctxUsed[1] = 1;
	uint8_t buf[32 + 32 * N48N_MAX_IBS]; SubmitReq req{};
	SubmitIb many[N48N_MAX_IBS];
	for (uint32_t i = 0; i < N48N_MAX_IBS; i++) many[i] = { mapVa, 4u };
	n48n_cs_in hdr{}; hdr.abi = N48N_ABI_VERSION; hdr.ctx_id = 1; hdr.num_ibs = kMaxRingIbs + 1u;
	memcpy(buf, &hdr, sizeof hdr);
	for (uint32_t i = 0; i < kMaxRingIbs + 1u; i++) { n48n_cs_ib ib{}; ib.va_start = mapVa; ib.ib_bytes = 16u; ib.ip_type = kIpCompute; memcpy(buf + 32u + (size_t)i * 32u, &ib, sizeof ib); }
	CHECK(submit_check(buf, 32u + 32u * (kMaxRingIbs + 1u), vm, bo, true, c, &req) == kBadArg);   // over the ring cap
	(void)many;
}

// Part 2: pin the IP types to the Linux uapi header.
static int header_check(const char *root) {
	int f = 0;
	std::string p = std::string(root) + "/include/uapi/drm/amdgpu_drm.h";
	std::ifstream in(p);
	if (!in) { printf("header check skipped: %s not found\n", p.c_str()); return 0; }
	std::string line; int gfx = -1, comp = -1;
	while (std::getline(in, line)) {
		std::istringstream ss(line); std::string a, b; int v;
		if (line.find("AMDGPU_HW_IP_GFX") != std::string::npos && (ss >> a >> b >> v) && b == "AMDGPU_HW_IP_GFX") gfx = v;
		std::istringstream ss2(line); if (line.find("AMDGPU_HW_IP_COMPUTE") != std::string::npos && (ss2 >> a >> b >> v) && b == "AMDGPU_HW_IP_COMPUTE") comp = v;
	}
	if (gfx != (int)kIpGfx) { printf("FAIL AMDGPU_HW_IP_GFX %d != %u\n", gfx, kIpGfx); f++; }
	if (comp != (int)kIpCompute) { printf("FAIL AMDGPU_HW_IP_COMPUTE %d != %u\n", comp, kIpCompute); f++; }
	return f;
}

// Pin kKextBuild to Info.plist (CFBundleVersion), tried at a couple of relative paths; skipped if not found.
static void version_check() {
	const char *paths[] = { "src/navi48-bringup/Info.plist", "Info.plist", "../Info.plist" };
	for (const char *p : paths) {
		std::ifstream in(p);
		if (!in) continue;
		std::stringstream ss; ss << in.rdbuf(); std::string s = ss.str();
		CHECK(s.find("0.0.674") != std::string::npos);                // CFBundleVersion / ShortVersion
		return;
	}
	printf("version check skipped: Info.plist not found from the working directory\n");
}

int main(int argc, char **argv) {
	hello_cases(); info_cases(); read_regs_cases(); ctx_cases(); gemva_cases(); fence_cases(); submit_cases();
	gate_cases(); domain_cases(); ring_cases(); submit_cap_case();
	version_check();
	if (argc > 1) fails += header_check(argv[1]);
	else printf("header check skipped: pass the Linux tree root (3857c2fe) as the first argument\n");
	if (fails) { printf("%d FAILED\n", fails); return 1; }
	printf("n33_native_test: all passed\n");
	return 0;
}
