// X86 / X86_64 implementation of the IEmitter primitive layer.
// Compiled only on x86-family targets (see metamod/CMakeLists.txt).

#include "precompiled.h"

#include "callback_jit_emitter.h"

#include <asmjit/x86.h>

namespace cb_jit {

namespace {

using namespace asmjit;

class X86Emitter : public IEmitter
{
public:
	Error init(CodeHolder& code) override
	{
		m_cc.reset(new x86::Compiler(&code));
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
	Reg new_vec_ss() override	{ return m_cc->new_xmm_ss(); }
	Reg new_vec_sd() override	{ return m_cc->new_xmm_sd(); }

	BaseMem new_stack(uint32_t size, uint32_t alignment) override
	{
		return m_cc->new_stack(size, alignment);
	}
	BaseMem stack_at(const BaseMem& base, int32_t off) override
	{
		return base.clone_adjusted(off);
	}
	BaseMem ptr(const Reg& base, int32_t off, uint32_t size) override
	{
		return x86::ptr(base.as<x86::Gp>(), off, size);
	}

	void mov_imm(const Reg& dst, uintptr_t val) override
	{
		m_cc->mov(dst.as<x86::Gp>(), imm(val));
	}
	void mov_reg(const Reg& dst, const Reg& src) override
	{
		m_cc->mov(dst.as<x86::Gp>(), src.as<x86::Gp>());
	}
	void load_word(const Reg& dst, const BaseMem& src) override
	{
		m_cc->mov(dst.as<x86::Gp>(), src.as<x86::Mem>().clone_resized(sizeof(uintptr_t)));
	}
	void store_word(const BaseMem& dst, const Reg& src) override
	{
		m_cc->mov(dst.as<x86::Mem>().clone_resized(sizeof(uintptr_t)), src.as<x86::Gp>());
	}
	void load_dword(const Reg& dst, const BaseMem& src) override
	{
		m_cc->mov(dst.as<x86::Gp>().r32(), src.as<x86::Mem>().clone_resized(4));
	}
	void store_dword(const BaseMem& dst, const Reg& src) override
	{
		m_cc->mov(dst.as<x86::Mem>().clone_resized(4), src.as<x86::Gp>().r32());
	}
	void store_imm32(const BaseMem& dst, int32_t val) override
	{
		m_cc->mov(dst.as<x86::Mem>().clone_resized(4), imm(val));
	}

	void load_float(const Reg& dst_vec, const BaseMem& src) override
	{
		m_cc->movss(dst_vec.as<x86::Vec>(), src.as<x86::Mem>().clone_resized(4));
	}
	void store_float(const BaseMem& dst, const Reg& src_vec) override
	{
		m_cc->movss(dst.as<x86::Mem>().clone_resized(4), src_vec.as<x86::Vec>());
	}
	void zero_vec(const Reg& vec) override
	{
		x86::Vec v = vec.as<x86::Vec>();
		m_cc->pxor(v, v);
	}

	void lea(const Reg& dst, const BaseMem& mem) override
	{
		m_cc->lea(dst.as<x86::Gp>(), mem.as<x86::Mem>());
	}

	void cmp_imm(const Reg& r, int32_t val) override
	{
		m_cc->cmp(r.as<x86::Gp>(), imm(val));
	}
	void cmp_reg(const Reg& a, const Reg& b) override
	{
		m_cc->cmp(a.as<x86::Gp>(), b.as<x86::Gp>());
	}
	void cmp_byte_at(const Reg& addr_reg, int32_t val) override
	{
		m_cc->cmp(x86::byte_ptr(addr_reg.as<x86::Gp>()), imm(val));
	}

	void jump_unconditional(Label l) override		{ m_cc->jmp(l); }
	void branch_eq(Label l) override			{ m_cc->je(l); }
	void branch_ne(Label l) override			{ m_cc->jne(l); }
	void branch_below_unsigned(Label l) override		{ m_cc->jb(l); }
	void branch_above_equal_unsigned(Label l) override	{ m_cc->jae(l); }

	void branch_if_zero(const Reg& r, Label l) override
	{
		x86::Gp g = r.as<x86::Gp>();
		m_cc->test(g, g);
		m_cc->jz(l);
	}
	void branch_if_nonzero(const Reg& r, Label l) override
	{
		x86::Gp g = r.as<x86::Gp>();
		m_cc->test(g, g);
		m_cc->jnz(l);
	}

	void max_signed(const Reg& target, const Reg& other) override
	{
		x86::Gp t = target.as<x86::Gp>();
		x86::Gp o = other.as<x86::Gp>();
		m_cc->cmp(t, o);
		m_cc->cmovl(t, o);
	}

	void zero_gp(const Reg& r) override
	{
		x86::Gp g = r.as<x86::Gp>();
		m_cc->xor_(g, g);
	}

	InvokeNode* invoke_imm(uintptr_t target, const FuncSignature& sig) override
	{
		InvokeNode* inv;
		m_cc->invoke(Out(inv), imm(target), sig);
		return inv;
	}
	InvokeNode* invoke_reg(const Reg& target, const FuncSignature& sig) override
	{
		InvokeNode* inv;
		m_cc->invoke(Out(inv), target.as<x86::Gp>(), sig);
		return inv;
	}

	void ret_void() override			{ m_cc->ret(); }
	void ret_gp(const Reg& r) override		{ m_cc->ret(r.as<x86::Gp>()); }
	void ret_vec(const Reg& r) override		{ m_cc->ret(r.as<x86::Vec>()); }

private:
	std::unique_ptr<x86::Compiler> m_cc;
};

}	// namespace

std::unique_ptr<IEmitter> create_native_emitter()
{
	return std::unique_ptr<IEmitter>(new X86Emitter());
}

}	// namespace cb_jit
