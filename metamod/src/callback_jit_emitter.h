#pragma once

#include <memory>
#include <vector>

#include <asmjit/core.h>

namespace cb_jit {

struct emit_call_site_t
{
	asmjit::Label	after_call;
	uintptr_t	handler_slot;
};

struct emit_result_t
{
	std::vector<emit_call_site_t> sites;
};

// Arch-agnostic primitive emitter. Implementations wrap either x86::Compiler
// or a64::Compiler; the orchestration layer in callback_jit_orchestration.cpp
// uses only this interface so its source is identical on all targets.
class IEmitter
{
public:
	virtual ~IEmitter() = default;

	// --- setup ---
	virtual asmjit::Error	init(asmjit::CodeHolder& code) = 0;
	virtual asmjit::Error	finalize() = 0;

	// --- function management ---
	virtual asmjit::FuncNode*	add_func(const asmjit::FuncSignature& sig) = 0;
	virtual void			end_func() = 0;
	virtual void			func_set_arg(asmjit::FuncNode* fn, size_t i, const asmjit::Reg& r) = 0;
	virtual asmjit::Label		new_label() = 0;
	virtual void			bind_label(asmjit::Label l) = 0;

	// --- register allocation ---
	virtual asmjit::Reg	new_gp_ptr() = 0;
	virtual asmjit::Reg	new_gp32() = 0;
	virtual asmjit::Reg	new_vec_f32() = 0;	// scalar single-precision float
	virtual asmjit::Reg	new_vec_f64() = 0;	// scalar double-precision float

	// --- stack / memory operands ---
	virtual asmjit::BaseMem	new_stack(uint32_t size, uint32_t alignment) = 0;
	virtual asmjit::BaseMem	stack_at(const asmjit::BaseMem& base, int32_t off) = 0;
	// Memory operand at base+off; access size is set by the load/store helper
	// that consumes this BaseMem, not here.
	virtual asmjit::BaseMem	ptr(const asmjit::Reg& base, int32_t off) = 0;

	// --- loads / stores ---
	virtual void	mov_imm(const asmjit::Reg& dst, uintptr_t imm) = 0;
	virtual void	mov_reg(const asmjit::Reg& dst, const asmjit::Reg& src) = 0;
	virtual void	load_word(const asmjit::Reg& dst, const asmjit::BaseMem& src) = 0;	// ptr-sized
	virtual void	store_word(const asmjit::BaseMem& dst, const asmjit::Reg& src) = 0;
	virtual void	load_dword(const asmjit::Reg& dst, const asmjit::BaseMem& src) = 0;
	virtual void	store_dword(const asmjit::BaseMem& dst, const asmjit::Reg& src) = 0;
	virtual void	store_imm32(const asmjit::BaseMem& dst, int32_t imm) = 0;

	// --- float scalar ---
	virtual void	load_float(const asmjit::Reg& dst_vec, const asmjit::BaseMem& src) = 0;
	virtual void	store_float(const asmjit::BaseMem& dst, const asmjit::Reg& src_vec) = 0;
	virtual void	zero_vec(const asmjit::Reg& vec) = 0;

	// --- address-of for a stack-allocated mem (from new_stack / stack_at) ---
	// On a64 this lowers to add dst, sp, #off after frame-layout finalize, so
	// only stack-relative BaseMems are valid. General-mem lea would need a
	// separate primitive.
	virtual void	lea_stack(const asmjit::Reg& dst, const asmjit::BaseMem& stack_mem) = 0;

	// --- compare + branch ---
	virtual void	cmp_imm(const asmjit::Reg& r, int32_t imm) = 0;
	virtual void	cmp_reg(const asmjit::Reg& a, const asmjit::Reg& b) = 0;
	// Combined "ldrb tmp,[addr]; cmp tmp,imm" or x86 "cmp byte ptr [addr_reg], imm".
	virtual void	cmp_byte_at(const asmjit::Reg& addr_reg, int32_t imm) = 0;

	virtual void	jump_unconditional(asmjit::Label l) = 0;
	virtual void	branch_eq(asmjit::Label l) = 0;
	virtual void	branch_ne(asmjit::Label l) = 0;
	virtual void	branch_below_unsigned(asmjit::Label l) = 0;		// jb / b.lo
	virtual void	branch_above_equal_unsigned(asmjit::Label l) = 0;	// jae / b.hs

	// Combined test-and-branch. cbz/cbnz on ARM, test+jz/jnz on x86.
	virtual void	branch_if_zero(const asmjit::Reg& r, asmjit::Label l) = 0;
	virtual void	branch_if_nonzero(const asmjit::Reg& r, asmjit::Label l) = 0;

	// target = max(target, other), signed.
	virtual void	max_signed(const asmjit::Reg& target, const asmjit::Reg& other) = 0;

	virtual void	zero_gp(const asmjit::Reg& r) = 0;

	// --- function invocation ---
	virtual asmjit::InvokeNode*	invoke_imm(uintptr_t target, const asmjit::FuncSignature& sig) = 0;
	virtual asmjit::InvokeNode*	invoke_reg(const asmjit::Reg& target, const asmjit::FuncSignature& sig) = 0;

	// --- return ---
	virtual void	ret_void() = 0;
	virtual void	ret_gp(const asmjit::Reg& r) = 0;
	virtual void	ret_vec(const asmjit::Reg& r) = 0;
};

// Factory: returns an emitter matching the build target arch (x86 or a64).
// Definition lives in the arch-specific TU (callback_jit_x86.cpp / callback_jit_a64.cpp).
std::unique_ptr<IEmitter> create_native_emitter();

// The orchestration entry point. Builds a callback per `jd` using the given emitter.
// `code` must already be init()'d on the emitter.
emit_result_t emit_callback(IEmitter& em, const jitdata_t& jd);

}	// namespace cb_jit
