#include "precompiled.h"

#include <asmjit/x86.h>
#include <cstdarg>

CJit g_jit;

namespace {

using namespace asmjit;

// Process-wide JIT runtime owns code allocations made by AsmJit during
// assembly. We re-copy emitted bytes into our own RWX allocator afterwards
// so static_allocator-based pattern queries continue to work; the runtime
// here is just a host for CodeHolder configuration.
JitRuntime& jit_runtime()
{
	static JitRuntime rt;
	return rt;
}

TypeId arg_type_id(argtype_t t)
{
	switch (t) {
	case at_float:	return TypeId::kFloat32;
	case at_double:	return TypeId::kFloat64;
	default:	return TypeId::kIntPtr;
	}
}

TypeId ret_type_id(rettype_t r)
{
	switch (r) {
	case rt_float:		return TypeId::kFloat32;
	case rt_integer:	return TypeId::kIntPtr;
	default:		return TypeId::kVoid;
	}
}

FuncSignature engine_signature(const jitdata_t& jd)
{
	FuncSignature sig(CallConvId::kCDecl);
	sig.set_ret(ret_type_id(jd.rettype));
	for (size_t i = 0; i < jd.args_count; ++i)
		sig.add_arg(arg_type_id(jd.arg_types.types[i]));
	if (jd.has_varargs)
		sig.set_va_index(uint32_t(jd.args_count));
	return sig;
}

// Allocates a fresh virtual register matching the given argtype.
Reg new_arg_reg(x86::Compiler& cc, argtype_t t)
{
	switch (t) {
	case at_float:
	case at_double:
		return cc.new_xmm();
	default:
		return cc.new_gp_ptr();
	}
}

// Emit an integer-load of the given size from `mem` into a fresh GP and
// store it at `dst_mem` (size matches). Used for byte-by-byte struct copy.
void emit_word_copy(x86::Compiler& cc, const x86::Mem& dst, const x86::Mem& src)
{
	x86::Gp tmp = cc.new_gp_ptr();
	cc.mov(tmp, src);
	cc.mov(dst, tmp);
}

// Records the (handler_slot, post-call label) pair for a single registered
// invocation site. Resolved into absolute addresses post-relocation.
struct emit_call_site_t
{
	Label		after_call;
	uintptr_t	handler_slot;
};

struct emit_result_t
{
	std::vector<emit_call_site_t> sites;
};

class CForwardCallbackJIT
{
public:
	CForwardCallbackJIT(const jitdata_t& jd) : m_jd(jd) {}
	emit_result_t emit(x86::Compiler& cc);

private:
	void emit_passthrough_only(x86::Compiler& cc, FuncNode* func, std::vector<Reg>& args);
	void emit_orchestration(x86::Compiler& cc, FuncNode* func, std::vector<Reg>& args, emit_result_t& result);
	void emit_invoke_handler(
		x86::Compiler& cc,
		const x86::Gp& target_reg,
		const std::vector<Reg>& args,
		bool capture_return,
		Reg& out_ret,
		Label& out_after_call);
	void emit_invoke_imm(
		x86::Compiler& cc,
		uintptr_t target,
		const std::vector<Reg>& args,
		bool capture_return,
		Reg& out_ret,
		Label& out_after_call);
	void emit_status_update(x86::Compiler& cc, const x86::Gp& globals, bool is_post);
	void emit_save_override(
		x86::Compiler& cc,
		const x86::Gp& globals,
		const x86::Mem& over_ret_mem,
		const Reg& ret_reg);

	const jitdata_t& m_jd;
};

void CForwardCallbackJIT::emit_passthrough_only(x86::Compiler& cc, FuncNode*, std::vector<Reg>& args)
{
	// For varargs callbacks (or when no plugins/mm_hook actually want it) we
	// emit a thin wrapper that calls the original directly with no plugin
	// notification. Returning the original's result preserves engine semantics.
	if (!m_jd.pfn_original) {
		// No original to forward to; just return zero/void.
		if (m_jd.rettype == rt_integer) {
			x86::Gp z = cc.new_gp_ptr();
			cc.xor_(z, z);
			cc.ret(z);
		} else if (m_jd.rettype == rt_float) {
			x86::Vec z = cc.new_xmm();
			cc.pxor(z, z);
			cc.ret(z);
		} else {
			cc.ret();
		}
		return;
	}

	InvokeNode* inv;
	cc.invoke(Out(inv), imm(m_jd.pfn_original), engine_signature(m_jd));
	for (size_t i = 0; i < args.size(); ++i)
		inv->set_arg(i, args[i]);

	if (m_jd.rettype == rt_integer) {
		x86::Gp r = cc.new_gp_ptr();
		inv->set_ret(0, r);
		cc.ret(r);
	} else if (m_jd.rettype == rt_float) {
		x86::Vec r = cc.new_xmm();
		inv->set_ret(0, r);
		cc.ret(r);
	} else {
		cc.ret();
	}
}

void CForwardCallbackJIT::emit_invoke_imm(
	x86::Compiler& cc,
	uintptr_t target,
	const std::vector<Reg>& args,
	bool capture_return,
	Reg& out_ret,
	Label& out_after_call)
{
	InvokeNode* inv;
	cc.invoke(Out(inv), imm(target), engine_signature(m_jd));
	for (size_t i = 0; i < args.size(); ++i)
		inv->set_arg(i, args[i]);

	if (capture_return && m_jd.rettype != rt_void) {
		if (m_jd.rettype == rt_integer) {
			x86::Gp r = cc.new_gp_ptr();
			inv->set_ret(0, r);
			out_ret = r;
		} else {
			x86::Vec r = cc.new_xmm();
			inv->set_ret(0, r);
			out_ret = r;
		}
	}

	out_after_call = cc.new_label();
	cc.bind(out_after_call);
}

void CForwardCallbackJIT::emit_invoke_handler(
	x86::Compiler& cc,
	const x86::Gp& target_reg,
	const std::vector<Reg>& args,
	bool capture_return,
	Reg& out_ret,
	Label& out_after_call)
{
	InvokeNode* inv;
	cc.invoke(Out(inv), target_reg, engine_signature(m_jd));
	for (size_t i = 0; i < args.size(); ++i)
		inv->set_arg(i, args[i]);

	if (capture_return && m_jd.rettype != rt_void) {
		if (m_jd.rettype == rt_integer) {
			x86::Gp r = cc.new_gp_ptr();
			inv->set_ret(0, r);
			out_ret = r;
		} else {
			x86::Vec r = cc.new_xmm();
			inv->set_ret(0, r);
			out_ret = r;
		}
	}

	out_after_call = cc.new_label();
	cc.bind(out_after_call);
}

void CForwardCallbackJIT::emit_save_override(
	x86::Compiler& cc,
	const x86::Gp& globals,
	const x86::Mem& over_ret_mem,
	const Reg& ret_reg)
{
	// if (mres >= MRES_OVERRIDE) over_ret = ret_reg
	x86::Gp mres_reg = cc.new_gp32();
	Label skip = cc.new_label();
	cc.mov(mres_reg, x86::dword_ptr(globals, int32_t(offsetof(meta_globals_t, mres))));
	cc.cmp(mres_reg, int32_t(MRES_OVERRIDE));
	cc.jb(skip);

	if (m_jd.rettype == rt_integer) {
		cc.mov(over_ret_mem.clone_resized(sizeof(intptr_t)), ret_reg.as<x86::Gp>());
	} else {
		// Float/double: spill xmm into the slot. Slot is sized intptr_t but
		// only the low 4 bytes (single) or 8 bytes (double) matter.
		cc.movss(over_ret_mem.clone_resized(4), ret_reg.as<x86::Vec>());
	}
	cc.bind(skip);
}

void CForwardCallbackJIT::emit_status_update(x86::Compiler& cc, const x86::Gp& globals, bool /*is_post*/)
{
	// status = max(status, mres) — same on pre and post loops.
	x86::Gp mres_reg = cc.new_gp32();
	x86::Gp status_reg = cc.new_gp32();
	cc.mov(mres_reg, x86::dword_ptr(globals, int32_t(offsetof(meta_globals_t, mres))));
	cc.mov(status_reg, x86::dword_ptr(globals, int32_t(offsetof(meta_globals_t, status))));
	cc.cmp(status_reg, mres_reg);
	cc.cmovl(status_reg, mres_reg);
	cc.mov(x86::dword_ptr(globals, int32_t(offsetof(meta_globals_t, status))), status_reg);
}

void CForwardCallbackJIT::emit_orchestration(x86::Compiler& cc, FuncNode*, std::vector<Reg>& args, emit_result_t& result)
{
	const size_t mg_size = sizeof(meta_globals_t);
	const size_t ret_slot_size = sizeof(intptr_t);
	const bool need_ret_slots = (m_jd.rettype != rt_void);
	const size_t locals_size = mg_size + (need_ret_slots ? 2 * ret_slot_size : 0);

	// One combined stack region: [mg_backup | orig_ret | over_ret].
	// mg_backup at offset 0 so its address can be used as g_metaGlobals.esp_save.
	x86::Mem locals_base = cc.new_stack(uint32_t(locals_size), 16);

	auto mg_at = [&](size_t off) {
		return locals_base.clone_adjusted(int32_t(off));
	};

	x86::Mem orig_ret_mem;
	x86::Mem over_ret_mem;
	if (need_ret_slots) {
		orig_ret_mem = locals_base.clone_adjusted(int32_t(mg_size));
		over_ret_mem = locals_base.clone_adjusted(int32_t(mg_size + ret_slot_size));
	}

	// Pointer to g_metaGlobals (kept across the body).
	x86::Gp globals = cc.new_gp_ptr("globals");
	cc.mov(globals, imm(uintptr_t(&g_metaGlobals)));

	// Backup g_metaGlobals into the local mg slot, intptr-sized words.
	for (size_t off = 0; off < mg_size; off += sizeof(intptr_t)) {
		emit_word_copy(
			cc,
			mg_at(off).clone_resized(sizeof(intptr_t)),
			x86::ptr(globals, int32_t(off), sizeof(intptr_t)));
	}

	// Pre-hook: metamod's mm_hook
	if (m_jd.mm_hook && m_jd.mm_hook_time == P_PRE) {
		Reg ignored;
		Label after_call;
		emit_invoke_imm(cc, m_jd.mm_hook, args, /*capture_return=*/false, ignored, after_call);
		result.sites.push_back({after_call, m_jd.mm_hook});
	}

	// Initialize meta_globals fields for our own dispatch.
	cc.mov(x86::dword_ptr(globals, int32_t(offsetof(meta_globals_t, mres))), int32_t(MRES_UNSET));
	cc.mov(x86::dword_ptr(globals, int32_t(offsetof(meta_globals_t, status))), int32_t(MRES_UNSET));

	if (need_ret_slots) {
		// orig_ret pointer = address of orig_ret_mem
		x86::Gp ret_ptr = cc.new_gp_ptr();
		cc.lea(ret_ptr, orig_ret_mem);
		cc.mov(x86::ptr(globals, int32_t(offsetof(meta_globals_t, orig_ret)), sizeof(uintptr_t)), ret_ptr);

		cc.lea(ret_ptr, over_ret_mem);
		cc.mov(x86::ptr(globals, int32_t(offsetof(meta_globals_t, override_ret)), sizeof(uintptr_t)), ret_ptr);
	}

	// esp_save points at the meta_globals backup region (stable address inside
	// our frame). meta_collect_fix_data uses it to walk paused callbacks.
	{
		x86::Gp mg_addr = cc.new_gp_ptr();
		cc.lea(mg_addr, mg_at(0));
		cc.mov(x86::ptr(globals, int32_t(offsetof(meta_globals_t, esp_save)), sizeof(uintptr_t)), mg_addr);
	}

	auto emit_plugin_loop = [&](size_t table_offset) {
		if (!m_jd.plugins) return;

		for (auto plug : *m_jd.plugins) {
			if (plug->status() < PL_RUNNING)
				continue;

			uintptr_t fn_table = *(uintptr_t *)(uintptr_t(plug) + table_offset);
			if (!fn_table)
				continue;

			uintptr_t handler_slot = fn_table + m_jd.pfn_offset;
			uintptr_t status_ptr = uintptr_t(plug->status_ptr());

			Label go_next = cc.new_label();

			// Status check: skip if plugin is no longer running.
			x86::Gp status_addr_reg = cc.new_gp_ptr();
			x86::Gp status_byte = cc.new_gp8();
			cc.mov(status_addr_reg, imm(status_ptr));
			cc.movzx(cc.new_gp32(), x86::byte_ptr(status_addr_reg));
			// Cleaner form using cmp on memory:
			cc.cmp(x86::byte_ptr(status_addr_reg), int32_t(PL_RUNNING));
			cc.jne(go_next);

			// Load handler fn-ptr from slot. Skip if null.
			x86::Gp slot_reg = cc.new_gp_ptr();
			x86::Gp handler_reg = cc.new_gp_ptr();
			cc.mov(slot_reg, imm(handler_slot));
			cc.mov(handler_reg, x86::ptr(slot_reg, 0, sizeof(uintptr_t)));
			cc.test(handler_reg, handler_reg);
			cc.jz(go_next);

			// prev_mres = mres; mres = MRES_IGNORED
			x86::Gp old_mres = cc.new_gp32();
			cc.mov(old_mres, x86::dword_ptr(globals, int32_t(offsetof(meta_globals_t, mres))));
			cc.mov(x86::dword_ptr(globals, int32_t(offsetof(meta_globals_t, prev_mres))), old_mres);
			cc.mov(x86::dword_ptr(globals, int32_t(offsetof(meta_globals_t, mres))), int32_t(MRES_IGNORED));

			// Invoke handler.
			Reg ret_reg;
			Label after_call;
			emit_invoke_handler(cc, handler_reg, args, need_ret_slots, ret_reg, after_call);
			result.sites.push_back({after_call, handler_slot});

			// status = max(status, mres)
			emit_status_update(cc, globals, /*is_post=*/false);

			// override_ret = ret if mres >= MRES_OVERRIDE
			if (need_ret_slots)
				emit_save_override(cc, globals, over_ret_mem, ret_reg);

			cc.bind(go_next);
		}
	};

	emit_plugin_loop(m_jd.table_offset);

	// Original call.
	Label skip_supercede = cc.new_label();
	{
		x86::Gp status_reg = cc.new_gp32();
		cc.mov(status_reg, x86::dword_ptr(globals, int32_t(offsetof(meta_globals_t, status))));
		cc.cmp(status_reg, int32_t(MRES_SUPERCEDE));
		Label do_original = cc.new_label();
		cc.jne(do_original);

		// Superceded: orig_ret = over_ret
		if (need_ret_slots) {
			x86::Gp tmp = cc.new_gp_ptr();
			cc.mov(tmp, over_ret_mem.clone_resized(sizeof(uintptr_t)));
			cc.mov(orig_ret_mem.clone_resized(sizeof(uintptr_t)), tmp);
		}
		cc.jmp(skip_supercede);

		cc.bind(do_original);

		if (m_jd.pfn_original) {
			Reg ret_reg;
			Label after_call;
			emit_invoke_imm(cc, m_jd.pfn_original, args, need_ret_slots, ret_reg, after_call);
			(void)after_call;	// not registering original-call retaddrs (no fixup needed; original is stable)

			if (m_jd.rettype == rt_integer) {
				cc.mov(orig_ret_mem.clone_resized(sizeof(uintptr_t)), ret_reg.as<x86::Gp>());
			} else if (m_jd.rettype == rt_float) {
				cc.movss(orig_ret_mem.clone_resized(4), ret_reg.as<x86::Vec>());
			}
		} else if (m_jd.rettype == rt_integer) {
			// Quirk: pfnShouldCollide etc. default to TRUE when no original.
			cc.mov(orig_ret_mem.clone_resized(sizeof(uintptr_t)), int32_t(1));
		} else if (m_jd.rettype == rt_float) {
			x86::Gp z = cc.new_gp32();
			cc.xor_(z, z);
			cc.mov(orig_ret_mem.clone_resized(4), z);
		}
	}
	cc.bind(skip_supercede);

	// Post-hook plugin loop.
	emit_plugin_loop(m_jd.post_table_offset);

	// Post-hook: metamod's mm_hook
	if (m_jd.mm_hook && m_jd.mm_hook_time == P_POST) {
		Reg ignored;
		Label after_call;
		emit_invoke_imm(cc, m_jd.mm_hook, args, /*capture_return=*/false, ignored, after_call);
		result.sites.push_back({after_call, m_jd.mm_hook});
	}

	// Restore g_metaGlobals from backup.
	for (size_t off = 0; off < mg_size; off += sizeof(intptr_t)) {
		emit_word_copy(
			cc,
			x86::ptr(globals, int32_t(off), sizeof(intptr_t)),
			mg_at(off).clone_resized(sizeof(intptr_t)));
	}

	// Compute final return value.
	if (m_jd.rettype == rt_integer) {
		x86::Gp ret = cc.new_gp_ptr();
		cc.mov(ret, orig_ret_mem.clone_resized(sizeof(uintptr_t)));
		x86::Gp status = cc.new_gp32();
		cc.mov(status, x86::dword_ptr(globals, int32_t(offsetof(meta_globals_t, status))));
		cc.cmp(status, int32_t(MRES_OVERRIDE));
		x86::Gp over = cc.new_gp_ptr();
		cc.mov(over, over_ret_mem.clone_resized(sizeof(uintptr_t)));
		cc.cmovae(ret, over);
		cc.ret(ret);
	} else if (m_jd.rettype == rt_float) {
		x86::Vec ret = cc.new_xmm();
		cc.movss(ret, orig_ret_mem.clone_resized(4));
		x86::Gp status = cc.new_gp32();
		cc.mov(status, x86::dword_ptr(globals, int32_t(offsetof(meta_globals_t, status))));
		cc.cmp(status, int32_t(MRES_OVERRIDE));
		Label use_orig = cc.new_label();
		cc.jb(use_orig);
		cc.movss(ret, over_ret_mem.clone_resized(4));
		cc.bind(use_orig);
		cc.ret(ret);
	} else {
		cc.ret();
	}
}

emit_result_t CForwardCallbackJIT::emit(x86::Compiler& cc)
{
	emit_result_t result;

	FuncSignature entry_sig = engine_signature(m_jd);
	FuncNode* func = cc.add_func(entry_sig);

	// Materialize incoming args into virtual regs we can re-pass.
	std::vector<Reg> arg_regs;
	arg_regs.reserve(m_jd.args_count);
	for (size_t i = 0; i < m_jd.args_count; ++i) {
		Reg r = new_arg_reg(cc, m_jd.arg_types.types[i]);
		func->set_arg(i, r);
		arg_regs.push_back(r);
	}

	if (m_jd.has_varargs) {
		// AsmJit cannot synthesize forwarding of variadic args portably.
		// Emit a passthrough that calls the original directly without
		// notifying plugins. (Plugin hooks of variadic engine functions
		// are unsupported in the AsmJit JIT path.)
		emit_passthrough_only(cc, func, arg_regs);
	} else {
		emit_orchestration(cc, func, arg_regs, result);
	}

	cc.end_func();
	return result;
}

} // namespace

CJit::CJit() : m_callback_allocator(static_allocator::mp_rwx), m_tramp_allocator(static_allocator::mp_rwx)
{
}

CJit::~CJit()
{
}

void CJit::register_call_site(uintptr_t retaddr, uintptr_t handler_slot)
{
	m_retaddr_to_handler[retaddr] = handler_slot;
	// First emitted retaddr for a given handler_slot wins; later ones are
	// duplicates (shouldn't happen within one rebuild, but be defensive).
	m_handler_to_retaddr.emplace(handler_slot, retaddr);
}

size_t CJit::compile_callback(jitdata_t* jitdata)
{
	if (!is_hook_needed(jitdata))
		return jitdata->pfn_original;

	if (jitdata->args_count > MAX_CALLBACK_ARGS) {
		// Signature exceeds our captured arg-type capacity. Fall back to
		// passthrough; plugins won't see the call, but the engine still works.
		return jitdata->pfn_original;
	}

	CodeHolder code;
	code.init(jit_runtime().environment(), jit_runtime().cpu_features());

	x86::Compiler cc(&code);

	CForwardCallbackJIT emitter(*jitdata);
	emit_result_t emitted = emitter.emit(cc);

	if (cc.finalize() != kErrorOk)
		return jitdata->pfn_original;

	size_t code_size = code.code_size();
	auto buf = (uint8_t *)m_callback_allocator.allocate(code_size);

	// Relocate to the actual destination, then copy the flattened bytes.
	if (code.relocate_to_base(uintptr_t(buf)) != kErrorOk)
		return jitdata->pfn_original;

	code.copy_flattened_data(buf, code_size, CopySectionFlags::kPadSectionBuffer);

	// Resolve label offsets to absolute addresses now that base is fixed.
	for (auto& site : emitted.sites) {
		uint64_t off;
		if (code.label_offset_from_base(site.after_call) == Globals::kInvalidId)
			continue;
		off = code.label_offset_from_base(site.after_call);
		register_call_site(uintptr_t(buf) + uintptr_t(off), site.handler_slot);
	}

	return uintptr_t(buf);
}

size_t CJit::compile_tramp(size_t ptr_to_func)
{
#if defined(__x86_64__) || defined(_M_X64)
	// movabs rax, ptr_to_func ; jmp qword ptr [rax]
	auto code = (uint8_t *)m_tramp_allocator.allocate(12);
	code[0] = 0x48;
	code[1] = 0xB8;
	*(uint64_t *)&code[2] = uint64_t(ptr_to_func);
	code[10] = 0xFF;
	code[11] = 0x20;
#else
	// jmp dword ptr [ptr_to_func]
	auto code = (uint8_t *)m_tramp_allocator.allocate(2 + sizeof(uint32_t));
	code[0] = 0xFFu;
	code[1] = 0x25u;
	*(uint32_t *)&code[2] = uint32_t(ptr_to_func);
#endif
	return uintptr_t(code);
}

void CJit::clear_callbacks()
{
	m_callback_allocator.deallocate_all();
	m_retaddr_to_handler.clear();
	m_handler_to_retaddr.clear();
}

void CJit::clear_tramps()
{
	m_tramp_allocator.deallocate_all();
}

bool CJit::is_callback_retaddr(uintptr_t addr)
{
	return m_retaddr_to_handler.find(addr) != m_retaddr_to_handler.end();
}

uintptr_t CJit::handler_slot_at_retaddr(uintptr_t retaddr) const
{
	auto it = m_retaddr_to_handler.find(retaddr);
	return it == m_retaddr_to_handler.end() ? 0 : it->second;
}

uintptr_t CJit::retaddr_for_handler_slot(uintptr_t handler_slot) const
{
	auto it = m_handler_to_retaddr.find(handler_slot);
	return it == m_handler_to_retaddr.end() ? 0 : it->second;
}

bool CJit::is_hook_needed(jitdata_t* jitdata)
{
	if (!jitdata->pfn_original)
		return true;

	if (jitdata->mm_hook)
		return true;

	if (!jitdata->plugins)
		return false;

	for (auto& plug : *jitdata->plugins) {
		const uintptr_t fn_table		= *(uintptr_t *)(uintptr_t(plug) + jitdata->table_offset);
		const uintptr_t fn_table_post	= *(uintptr_t *)(uintptr_t(plug) + jitdata->post_table_offset);

		if (fn_table || fn_table_post)
			return true;
	}

	return false;
}
