// Arch-agnostic forward-callback orchestration. Translates a jitdata_t into
// a sequence of IEmitter primitive calls that produce the equivalent of the
// hand-written x86 callback in the original jitasm-based design.
//
// The instruction-level emission decisions (which physical instructions to
// use, how immediates are materialized, etc.) live in callback_jit_x86.cpp
// and callback_jit_a64.cpp; this file is identical on every target.

#include "precompiled.h"

#include "callback_jit_emitter.h"

#include <asmjit/core.h>

namespace cb_jit {

namespace {

using namespace asmjit;

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

// Allocate a new vreg to hold the i-th incoming arg.
Reg new_arg_reg(IEmitter& em, argtype_t t)
{
	switch (t) {
	case at_float:	return em.new_vec_f32();
	case at_double:	return em.new_vec_f64();
	default:	return em.new_gp_ptr();
	}
}

// Capture an invoke's return value into a fresh vreg matching the rettype.
// Returns an empty Reg (default-constructed) for void returns.
Reg capture_return(IEmitter& em, InvokeNode* inv, rettype_t rt)
{
	if (rt == rt_void)
		return Reg();

	if (rt == rt_integer) {
		Reg r = em.new_gp_ptr();
		inv->set_ret(0, r);
		return r;
	}

	Reg r = em.new_vec_f32();
	inv->set_ret(0, r);
	return r;
}

void emit_passthrough_only(IEmitter& em, const jitdata_t& jd, const std::vector<Reg>& args)
{
	if (!jd.pfn_original) {
		// No original to forward to; return zero/void.
		if (jd.rettype == rt_integer) {
			Reg z = em.new_gp_ptr();
			em.zero_gp(z);
			em.ret_gp(z);
		} else if (jd.rettype == rt_float) {
			Reg z = em.new_vec_f32();
			em.zero_vec(z);
			em.ret_vec(z);
		} else {
			em.ret_void();
		}
		return;
	}

	InvokeNode* inv = em.invoke_imm(jd.pfn_original, engine_signature(jd));
	for (size_t i = 0; i < args.size(); ++i)
		inv->set_arg(i, args[i]);

	if (jd.rettype == rt_integer) {
		Reg r = em.new_gp_ptr();
		inv->set_ret(0, r);
		em.ret_gp(r);
	} else if (jd.rettype == rt_float) {
		Reg r = em.new_vec_f32();
		inv->set_ret(0, r);
		em.ret_vec(r);
	} else {
		em.ret_void();
	}
}

// Updates g_metaGlobals.status = max(status, mres). Same on pre and post loops.
void emit_status_update(IEmitter& em, const Reg& globals)
{
	Reg mres_reg = em.new_gp32();
	Reg status_reg = em.new_gp32();
	em.load_dword(mres_reg, em.ptr(globals, int32_t(offsetof(meta_globals_t, mres))));
	em.load_dword(status_reg, em.ptr(globals, int32_t(offsetof(meta_globals_t, status))));
	em.max_signed(status_reg, mres_reg);
	em.store_dword(em.ptr(globals, int32_t(offsetof(meta_globals_t, status))), status_reg);
}

// if (mres >= MRES_OVERRIDE) over_ret = ret_reg
void emit_save_override(IEmitter& em, const Reg& globals, const BaseMem& over_ret_mem,
			rettype_t rt, const Reg& ret_reg)
{
	Reg mres_reg = em.new_gp32();
	Label skip = em.new_label();
	em.load_dword(mres_reg, em.ptr(globals, int32_t(offsetof(meta_globals_t, mres))));
	em.cmp_imm(mres_reg, int32_t(MRES_OVERRIDE));
	em.branch_below_unsigned(skip);

	if (rt == rt_integer)
		em.store_word(over_ret_mem, ret_reg);
	else
		em.store_float(over_ret_mem, ret_reg);

	em.bind_label(skip);
}

// One iteration of the per-plugin loop body. Emits the status check, handler
// load, prev_mres bump, invoke, status max, override save, then go_next.
void emit_plugin_call(IEmitter& em, const jitdata_t& jd, const Reg& globals,
		      const std::vector<Reg>& args, const BaseMem& over_ret_mem,
		      bool need_ret_slots, uintptr_t handler_slot, uintptr_t status_ptr,
		      emit_result_t& result)
{
	Label go_next = em.new_label();

	// Status check: skip if plugin is no longer running.
	Reg status_addr_reg = em.new_gp_ptr();
	em.mov_imm(status_addr_reg, status_ptr);
	em.cmp_byte_at(status_addr_reg, int32_t(PL_RUNNING));
	em.branch_ne(go_next);

	// Load handler fn-ptr from slot. Skip if null.
	Reg slot_reg = em.new_gp_ptr();
	em.mov_imm(slot_reg, handler_slot);
	Reg handler_reg = em.new_gp_ptr();
	em.load_word(handler_reg, em.ptr(slot_reg, 0));
	em.branch_if_zero(handler_reg, go_next);

	// prev_mres = mres; mres = MRES_IGNORED
	Reg old_mres = em.new_gp32();
	em.load_dword(old_mres, em.ptr(globals, int32_t(offsetof(meta_globals_t, mres))));
	em.store_dword(em.ptr(globals, int32_t(offsetof(meta_globals_t, prev_mres))), old_mres);
	em.store_imm32(em.ptr(globals, int32_t(offsetof(meta_globals_t, mres))), int32_t(MRES_IGNORED));

	// Invoke handler, capture return.
	InvokeNode* inv = em.invoke_reg(handler_reg, engine_signature(jd));
	for (size_t i = 0; i < args.size(); ++i)
		inv->set_arg(i, args[i]);
	Reg ret_reg = capture_return(em, inv, jd.rettype);

	// Bind a label immediately after the invoke. Its resolved offset becomes
	// the post-call return address that registry lookups key on.
	Label after_call = em.new_label();
	em.bind_label(after_call);
	result.sites.push_back({after_call, handler_slot});

	emit_status_update(em, globals);

	if (need_ret_slots && jd.rettype != rt_void)
		emit_save_override(em, globals, over_ret_mem, jd.rettype, ret_reg);

	em.bind_label(go_next);
}

void emit_orchestration(IEmitter& em, const jitdata_t& jd,
			const std::vector<Reg>& args, emit_result_t& result)
{
	const size_t mg_size = sizeof(meta_globals_t);
	const size_t ret_slot_size = sizeof(intptr_t);
	const bool need_ret_slots = (jd.rettype != rt_void);
	const size_t locals_size = mg_size + (need_ret_slots ? 2 * ret_slot_size : 0);

	// One combined stack region: [mg_backup | orig_ret | over_ret].
	// mg_backup at offset 0 so its address can be used as g_metaGlobals.esp_save.
	BaseMem locals_base = em.new_stack(uint32_t(locals_size), 16);

	auto mg_at = [&](size_t off) {
		return em.stack_at(locals_base, int32_t(off));
	};

	BaseMem orig_ret_mem;
	BaseMem over_ret_mem;
	if (need_ret_slots) {
		orig_ret_mem = em.stack_at(locals_base, int32_t(mg_size));
		over_ret_mem = em.stack_at(locals_base, int32_t(mg_size + ret_slot_size));
	}

	// Pointer to g_metaGlobals (kept across the body).
	Reg globals = em.new_gp_ptr();
	em.mov_imm(globals, uintptr_t(&g_metaGlobals));

	// Backup g_metaGlobals into the local mg slot, intptr-sized words.
	for (size_t off = 0; off < mg_size; off += sizeof(intptr_t)) {
		Reg tmp = em.new_gp_ptr();
		em.load_word(tmp, em.ptr(globals, int32_t(off)));
		em.store_word(em.stack_at(locals_base, int32_t(off)), tmp);
	}

	// Pre-hook: metamod's mm_hook
	if (jd.mm_hook && jd.mm_hook_time == P_PRE) {
		InvokeNode* inv = em.invoke_imm(jd.mm_hook, engine_signature(jd));
		for (size_t i = 0; i < args.size(); ++i)
			inv->set_arg(i, args[i]);
		Label after_call = em.new_label();
		em.bind_label(after_call);
		result.sites.push_back({after_call, jd.mm_hook});
	}

	// Initialize meta_globals fields for our own dispatch.
	em.store_imm32(em.ptr(globals, int32_t(offsetof(meta_globals_t, mres))), int32_t(MRES_UNSET));
	em.store_imm32(em.ptr(globals, int32_t(offsetof(meta_globals_t, status))), int32_t(MRES_UNSET));

	if (need_ret_slots) {
		Reg ret_ptr = em.new_gp_ptr();
		em.lea_stack(ret_ptr, orig_ret_mem);
		em.store_word(em.ptr(globals, int32_t(offsetof(meta_globals_t, orig_ret))), ret_ptr);

		em.lea_stack(ret_ptr, over_ret_mem);
		em.store_word(em.ptr(globals, int32_t(offsetof(meta_globals_t, override_ret))), ret_ptr);
	}

	// esp_save = address of meta_globals backup region. meta_collect_fix_data
	// uses this address to walk paused callbacks across rebuild.
	{
		Reg mg_addr = em.new_gp_ptr();
		em.lea_stack(mg_addr, mg_at(0));
		em.store_word(em.ptr(globals, int32_t(offsetof(meta_globals_t, esp_save))), mg_addr);
	}

	auto emit_plugin_loop = [&](size_t table_offset) {
		if (!jd.plugins) return;

		for (auto plug : *jd.plugins) {
			if (plug->status() < PL_RUNNING)
				continue;

			uintptr_t fn_table = *(uintptr_t *)(uintptr_t(plug) + table_offset);
			if (!fn_table)
				continue;

			uintptr_t handler_slot = fn_table + jd.pfn_offset;
			uintptr_t status_ptr = uintptr_t(plug->status_ptr());

			emit_plugin_call(em, jd, globals, args, over_ret_mem,
					 need_ret_slots, handler_slot, status_ptr, result);
		}
	};

	emit_plugin_loop(jd.table_offset);

	// Original call, with supercede short-circuit.
	Label skip_supercede = em.new_label();
	{
		Reg status_reg = em.new_gp32();
		em.load_dword(status_reg, em.ptr(globals, int32_t(offsetof(meta_globals_t, status))));
		em.cmp_imm(status_reg, int32_t(MRES_SUPERCEDE));
		Label do_original = em.new_label();
		em.branch_ne(do_original);

		// Superceded: orig_ret = over_ret
		if (need_ret_slots) {
			Reg tmp = em.new_gp_ptr();
			em.load_word(tmp, over_ret_mem);
			em.store_word(orig_ret_mem, tmp);
		}
		em.jump_unconditional(skip_supercede);

		em.bind_label(do_original);

		if (jd.pfn_original) {
			InvokeNode* inv = em.invoke_imm(jd.pfn_original, engine_signature(jd));
			for (size_t i = 0; i < args.size(); ++i)
				inv->set_arg(i, args[i]);
			Reg ret_reg = capture_return(em, inv, jd.rettype);

			if (jd.rettype == rt_integer) {
				em.store_word(orig_ret_mem, ret_reg);
			} else if (jd.rettype == rt_float) {
				em.store_float(orig_ret_mem, ret_reg);
			}
		} else if (jd.rettype == rt_integer) {
			// Quirk: pfnShouldCollide etc. default to TRUE when no original.
			em.store_imm32(orig_ret_mem, int32_t(1));
		} else if (jd.rettype == rt_float) {
			em.store_imm32(orig_ret_mem, 0);
		}
	}
	em.bind_label(skip_supercede);

	emit_plugin_loop(jd.post_table_offset);

	if (jd.mm_hook && jd.mm_hook_time == P_POST) {
		InvokeNode* inv = em.invoke_imm(jd.mm_hook, engine_signature(jd));
		for (size_t i = 0; i < args.size(); ++i)
			inv->set_arg(i, args[i]);
		Label after_call = em.new_label();
		em.bind_label(after_call);
		result.sites.push_back({after_call, jd.mm_hook});
	}

	// Restore g_metaGlobals from backup.
	for (size_t off = 0; off < mg_size; off += sizeof(intptr_t)) {
		Reg tmp = em.new_gp_ptr();
		em.load_word(tmp, em.stack_at(locals_base, int32_t(off)));
		em.store_word(em.ptr(globals, int32_t(off)), tmp);
	}

	// Final return value: status >= MRES_OVERRIDE picks over_ret, else orig_ret.
	if (jd.rettype == rt_integer) {
		Reg ret = em.new_gp_ptr();
		em.load_word(ret, orig_ret_mem);
		Reg status = em.new_gp32();
		em.load_dword(status, em.ptr(globals, int32_t(offsetof(meta_globals_t, status))));
		em.cmp_imm(status, int32_t(MRES_OVERRIDE));
		Label done = em.new_label();
		em.branch_below_unsigned(done);
		em.load_word(ret, over_ret_mem);
		em.bind_label(done);
		em.ret_gp(ret);
	} else if (jd.rettype == rt_float) {
		Reg ret = em.new_vec_f32();
		em.load_float(ret, orig_ret_mem);
		Reg status = em.new_gp32();
		em.load_dword(status, em.ptr(globals, int32_t(offsetof(meta_globals_t, status))));
		em.cmp_imm(status, int32_t(MRES_OVERRIDE));
		Label done = em.new_label();
		em.branch_below_unsigned(done);
		em.load_float(ret, over_ret_mem);
		em.bind_label(done);
		em.ret_vec(ret);
	} else {
		em.ret_void();
	}
}

}	// namespace

emit_result_t emit_callback(IEmitter& em, const jitdata_t& jd)
{
	emit_result_t result;

	FuncSignature entry_sig = engine_signature(jd);
	FuncNode* func = em.add_func(entry_sig);

	std::vector<Reg> arg_regs;
	arg_regs.reserve(jd.args_count);
	for (size_t i = 0; i < jd.args_count; ++i) {
		Reg r = new_arg_reg(em, jd.arg_types.types[i]);
		em.func_set_arg(func, i, r);
		arg_regs.push_back(r);
	}

	if (jd.has_varargs)
		emit_passthrough_only(em, jd, arg_regs);
	else
		emit_orchestration(em, jd, arg_regs, result);

	em.end_func();
	return result;
}

}	// namespace cb_jit
