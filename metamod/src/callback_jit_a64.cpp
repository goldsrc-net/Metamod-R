// AArch64 implementation of the IEmitter primitive layer.
// Compiled only on aarch64 targets (see metamod/CMakeLists.txt).
//
// The orchestration layer in callback_jit_orchestration.cpp drives this via
// IEmitter primitives identically to the x86 build. Each primitive maps to
// the closest AArch64 sequence:
//
//   x86 idiom                       AArch64 equivalent
//   --------------------------------------------------------
//   cmp byte ptr [addr], imm        ldrb w_tmp, [x_addr]; cmp w_tmp, imm
//   test r, r ; jz l                cbz r, l
//   cmp a, b ; cmovl a, b           cmp a, b ; csel a, b, a, lt
//   xor r, r                        mov r, xzr
//   pxor v, v                       fmov v.s, wzr
//   lea r, [mem]                    load_address_of (AsmJit pseudo)
//   movss [mem], xmm                str s_vec, [mem]
//   ret reg                         ret reg

#include "precompiled.h"

#include "callback_jit_emitter.h"

#include <asmjit/a64.h>

namespace cb_jit {

namespace {

using namespace asmjit;

class A64Emitter : public IEmitter
{
public:
	Error init(CodeHolder& code) override
	{
		m_cc.reset(new a64::Compiler(&code));
		return kErrorOk;
	}

	Error finalize() override
	{
		return m_cc->finalize();
	}

	FuncNode* add_func(const FuncSignature& sig) override	{ return m_cc->add_func(sig); }
	void end_func() override				{ m_cc->end_func(); }
	void func_set_arg(FuncNode* fn, size_t i, const Reg& r) override
	{
		fn->set_arg(i, r);
	}
	Label new_label() override				{ return m_cc->new_label(); }
	void bind_label(Label l) override			{ m_cc->bind(l); }

	Reg new_gp_ptr() override	{ return m_cc->new_gp_ptr(); }
	Reg new_gp32() override		{ return m_cc->new_gp32(); }
	Reg new_vec_ss() override	{ return m_cc->new_vec_s(); }
	Reg new_vec_sd() override	{ return m_cc->new_vec_d(); }

	BaseMem new_stack(uint32_t size, uint32_t alignment) override
	{
		return m_cc->new_stack(size, alignment);
	}
	BaseMem stack_at(const BaseMem& base, int32_t off) override
	{
		return base.clone_adjusted(off);
	}
	BaseMem ptr(const Reg& base, int32_t off, uint32_t /*size*/) override
	{
		return a64::ptr(base.as<a64::Gp>(), off);
	}

	void mov_imm(const Reg& dst, uintptr_t val) override
	{
		// a64::Compiler::mov(Gp, Imm) materializes large immediates as a
		// movz/movk sequence; we don't have to manage that here.
		m_cc->mov(dst.as<a64::Gp>().x(), Imm(val));
	}
	void mov_reg(const Reg& dst, const Reg& src) override
	{
		m_cc->mov(dst.as<a64::Gp>(), src.as<a64::Gp>());
	}
	void load_word(const Reg& dst, const BaseMem& src) override
	{
		m_cc->ldr(dst.as<a64::Gp>().x(), src.as<a64::Mem>());
	}
	void store_word(const BaseMem& dst, const Reg& src) override
	{
		m_cc->str(src.as<a64::Gp>().x(), dst.as<a64::Mem>());
	}
	void load_dword(const Reg& dst, const BaseMem& src) override
	{
		m_cc->ldr(dst.as<a64::Gp>().w(), src.as<a64::Mem>());
	}
	void store_dword(const BaseMem& dst, const Reg& src) override
	{
		m_cc->str(src.as<a64::Gp>().w(), dst.as<a64::Mem>());
	}
	void store_imm32(const BaseMem& dst, int32_t val) override
	{
		// AArch64 has no store-immediate; materialize then store.
		if (val == 0) {
			m_cc->str(a64::wzr, dst.as<a64::Mem>());
			return;
		}
		a64::Gp tmp = m_cc->new_gp32();
		m_cc->mov(tmp, Imm(uint32_t(val)));
		m_cc->str(tmp, dst.as<a64::Mem>());
	}

	void load_float(const Reg& dst_vec, const BaseMem& src) override
	{
		m_cc->ldr(dst_vec.as<a64::Vec>().s(), src.as<a64::Mem>());
	}
	void store_float(const BaseMem& dst, const Reg& src_vec) override
	{
		m_cc->str(src_vec.as<a64::Vec>().s(), dst.as<a64::Mem>());
	}
	void zero_vec(const Reg& vec) override
	{
		// fmov s_vec, wzr — copies the 32-bit zero register into the
		// scalar vector register, yielding 0.0f.
		m_cc->fmov(vec.as<a64::Vec>().s(), a64::wzr);
	}

	void lea(const Reg& dst, const BaseMem& mem) override
	{
		// AsmJit's a64::Compiler exposes lea-equivalent specifically for
		// stack-allocated addresses. AArch64 has no LEA; this lowers to
		// `add dst, sp, #offset` once frame layout is finalized.
		m_cc->load_address_of(dst.as<a64::Gp>(), mem.as<a64::Mem>());
	}

	void cmp_imm(const Reg& r, int32_t val) override
	{
		m_cc->cmp(r.as<a64::Gp>(), Imm(val));
	}
	void cmp_reg(const Reg& a, const Reg& b) override
	{
		m_cc->cmp(a.as<a64::Gp>(), b.as<a64::Gp>());
	}
	void cmp_byte_at(const Reg& addr_reg, int32_t val) override
	{
		// ldrb writes a 32-bit zero-extended byte into the destination.
		a64::Gp tmp = m_cc->new_gp32();
		m_cc->ldrb(tmp, a64::ptr(addr_reg.as<a64::Gp>()));
		m_cc->cmp(tmp, Imm(val));
	}

	void jump_unconditional(Label l) override		{ m_cc->b(l); }
	void branch_eq(Label l) override			{ m_cc->b_eq(l); }
	void branch_ne(Label l) override			{ m_cc->b_ne(l); }
	void branch_below_unsigned(Label l) override		{ m_cc->b_lo(l); }	// unsigned <
	void branch_above_equal_unsigned(Label l) override	{ m_cc->b_hs(l); }	// unsigned >=

	void branch_if_zero(const Reg& r, Label l) override
	{
		m_cc->cbz(r.as<a64::Gp>(), l);
	}
	void branch_if_nonzero(const Reg& r, Label l) override
	{
		m_cc->cbnz(r.as<a64::Gp>(), l);
	}

	void max_signed(const Reg& target, const Reg& other) override
	{
		// target = max_signed(target, other):
		//   cmp target, other
		//   csel target, other, target, lt   ; if target < other, target = other
		a64::Gp t = target.as<a64::Gp>();
		a64::Gp o = other.as<a64::Gp>();
		m_cc->cmp(t, o);
		m_cc->csel(t, o, t, Imm(uint32_t(asmjit::arm::CondCode::kLT)));
	}

	void zero_gp(const Reg& r) override
	{
		// `mov Wd, wzr` clears the 32-bit form (and zeroes upper 32 of Xd).
		m_cc->mov(r.as<a64::Gp>().w(), a64::wzr);
	}

	InvokeNode* invoke_imm(uintptr_t target, const FuncSignature& sig) override
	{
		// AsmJit needs a Gp holding the call target on AArch64; load the
		// immediate first, then invoke through the register form.
		a64::Gp target_reg = m_cc->new_gp_ptr();
		m_cc->mov(target_reg, Imm(target));
		InvokeNode* inv;
		m_cc->invoke(Out(inv), target_reg, sig);
		return inv;
	}
	InvokeNode* invoke_reg(const Reg& target, const FuncSignature& sig) override
	{
		InvokeNode* inv;
		m_cc->invoke(Out(inv), target.as<a64::Gp>(), sig);
		return inv;
	}

	void ret_void() override			{ m_cc->ret(); }
	void ret_gp(const Reg& r) override		{ m_cc->ret(r.as<a64::Gp>()); }
	void ret_vec(const Reg& r) override		{ m_cc->ret(r.as<a64::Vec>()); }

private:
	std::unique_ptr<a64::Compiler> m_cc;
};

}	// namespace

std::unique_ptr<IEmitter> create_native_emitter()
{
	return std::unique_ptr<IEmitter>(new A64Emitter());
}

}	// namespace cb_jit
