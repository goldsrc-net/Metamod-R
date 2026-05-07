#include "precompiled.h"

#include "callback_jit_emitter.h"

#include <asmjit/core.h>

CJit g_jit;

namespace {

using namespace asmjit;

JitRuntime& jit_runtime()
{
	static JitRuntime rt;
	return rt;
}

// Flush instruction cache after writing JIT code to a buffer. No-op on x86;
// required on AArch64 (separate I/D caches), and harmless to call.
void flush_icache(void* start, size_t size)
{
#if defined(__GNUC__) || defined(__clang__)
	__builtin___clear_cache(reinterpret_cast<char *>(start),
				reinterpret_cast<char *>(start) + size);
#endif
}

}	// namespace

CJit::CJit() : m_callback_allocator(static_allocator::mp_rwx), m_tramp_allocator(static_allocator::mp_rwx)
{
}

CJit::~CJit()
{
}

void CJit::register_call_site(uintptr_t retaddr, uintptr_t handler_slot)
{
	m_retaddr_to_handler[retaddr] = handler_slot;
	m_handler_to_retaddr.emplace(handler_slot, retaddr);
}

size_t CJit::compile_callback(jitdata_t* jitdata)
{
	if (!is_hook_needed(jitdata))
		return jitdata->pfn_original;

	if (jitdata->args_count > MAX_CALLBACK_ARGS)
		return jitdata->pfn_original;

	CodeHolder code;
	code.init(jit_runtime().environment(), jit_runtime().cpu_features());

	auto em = cb_jit::create_native_emitter();
	if (em->init(code) != kErrorOk)
		return jitdata->pfn_original;

	cb_jit::emit_result_t emitted = cb_jit::emit_callback(*em, *jitdata);

	if (em->finalize() != kErrorOk)
		return jitdata->pfn_original;

	size_t code_size = code.code_size();
	auto buf = (uint8_t *)m_callback_allocator.allocate(code_size);

	if (code.relocate_to_base(uintptr_t(buf)) != kErrorOk)
		return jitdata->pfn_original;

	code.copy_flattened_data(buf, code_size, CopySectionFlags::kPadSectionBuffer);
	flush_icache(buf, code_size);

	for (auto& site : emitted.sites) {
		uint64_t off = code.label_offset_from_base(site.after_call);
		register_call_site(uintptr_t(buf) + uintptr_t(off), site.handler_slot);
	}

	return uintptr_t(buf);
}

size_t CJit::compile_tramp(size_t ptr_to_func)
{
#if defined(__aarch64__) || defined(_M_ARM64)
	// AArch64: load *ptr_to_func into x16 and branch to it.
	//   LDR  x16, [PC + 12]   ; 0x58000070  -> bytes 70 00 00 58
	//   LDR  x16, [x16]       ; 0xF9400210  -> bytes 10 02 40 F9
	//   BR   x16              ; 0xD61F0200  -> bytes 00 02 1F D6
	//   .quad ptr_to_func
	auto code = (uint8_t *)m_tramp_allocator.allocate(20);
	*(uint32_t *)&code[0]	= 0x58000070u;
	*(uint32_t *)&code[4]	= 0xF9400210u;
	*(uint32_t *)&code[8]	= 0xD61F0200u;
	*(uint64_t *)&code[12]	= uint64_t(ptr_to_func);
	flush_icache(code, 20);
	return uintptr_t(code);
#elif defined(__x86_64__) || defined(_M_X64)
	// movabs rax, ptr_to_func ; jmp qword ptr [rax]
	auto code = (uint8_t *)m_tramp_allocator.allocate(12);
	code[0] = 0x48;
	code[1] = 0xB8;
	*(uint64_t *)&code[2] = uint64_t(ptr_to_func);
	code[10] = 0xFF;
	code[11] = 0x20;
	return uintptr_t(code);
#else
	// jmp dword ptr [ptr_to_func]
	auto code = (uint8_t *)m_tramp_allocator.allocate(2 + sizeof(uint32_t));
	code[0] = 0xFFu;
	code[1] = 0x25u;
	*(uint32_t *)&code[2] = uint32_t(ptr_to_func);
	return uintptr_t(code);
#endif
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
