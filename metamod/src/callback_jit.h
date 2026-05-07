#pragma once

#include <unordered_map>

constexpr size_t MAX_CALLBACK_ARGS = 16;

#define CDATA_ENTRY(s, x, p, h)	{#x, offsetof(s, x), (uint8)getArgsCount(decltype(s::x)()), getRetType(decltype(s::x)()), is_varargs(decltype(s::x)()), getArgTypes(decltype(s::x)()), p, h}

enum rettype_t : uint8_t
{
	rt_void,
	rt_integer,
	rt_float
};

enum argtype_t : uint8_t
{
	at_int = 0,	// integer, pointer, struct-by-value (treat as pointer-sized)
	at_float,
	at_double
};

struct arg_types_array_t
{
	argtype_t types[MAX_CALLBACK_ARGS];
};

struct jitdata_t
{
	size_t			pfn_original;
	size_t			pfn_offset;	// from fn table
	uint8			args_count;
	rettype_t		rettype;
	bool			has_varargs;
	uint8			mm_hook_time;
	size_t			mm_hook;
	arg_types_array_t	arg_types;

	plugins_t*		plugins;
	size_t			table_offset;		// from MPlugin
	size_t			post_table_offset;	// from MPlugin

#ifdef JIT_DEBUG
	const char*		name;
#endif
};

struct compile_data_t
{
	const char*		name;
	size_t			offset;
	uint8			args_count;
	rettype_t		rettype;
	bool			has_varargs;
	arg_types_array_t	arg_types;
	uint8			mm_hook_time;
	size_t			mm_hook;
};

template<typename ret_t, typename ...t_args>
constexpr size_t getArgsCount(ret_t (*)(t_args...))
{
	return sizeof...(t_args);
}

template<typename ret_t, typename ...t_args>
constexpr size_t getArgsCount(ret_t (*)(t_args..., ...))
{
	return sizeof...(t_args);
}

template<typename ...t_args>
constexpr rettype_t getRetType(void (*)(t_args..., ...))
{
	return rt_void;
}

template<typename ...t_args>
constexpr rettype_t getRetType(void(*)(t_args...))
{
	return rt_void;
}

template<typename ...t_args>
constexpr rettype_t getRetType(float (*)(t_args...))
{
	return rt_float;
}

template<typename ...t_args>
constexpr rettype_t getRetType(double (*)(t_args...))
{
	return rt_float;
}

template<typename ...t_args>
constexpr rettype_t getRetType(long double (*)(t_args...))
{
	return rt_float;
}

template<typename ret_t, typename ...t_args>
constexpr rettype_t getRetType(ret_t (*)(t_args...))
{
	return rt_integer;
}

template<typename ret_t, typename ...t_args>
constexpr bool is_varargs(ret_t (*)(t_args...))
{
	return false;
}

template<typename ret_t, typename ...t_args>
constexpr bool is_varargs(ret_t (*)(t_args..., ...))
{
	return true;
}

template<typename T>
constexpr argtype_t getArgType()
{
	return std::is_floating_point<T>::value
		? (sizeof(T) == sizeof(float) ? at_float : at_double)
		: at_int;
}

template<typename ret_t, typename ...t_args>
constexpr arg_types_array_t getArgTypes(ret_t (*)(t_args...))
{
	return arg_types_array_t{ { getArgType<t_args>()... } };
}

template<typename ret_t, typename ...t_args>
constexpr arg_types_array_t getArgTypes(ret_t (*)(t_args..., ...))
{
	return arg_types_array_t{ { getArgType<t_args>()... } };
}

class CJit
{
public:
	CJit();
	~CJit();

	size_t compile_callback(jitdata_t* jitdata);
	size_t compile_tramp(size_t ptr_to_func);
	void clear_callbacks();
	void clear_tramps();

	// Returns true if `addr` is a known post-call return address inside any
	// emitted callback. Used by meta_collect_fix_data to identify paused
	// callback frames on a live stack.
	bool is_callback_retaddr(uintptr_t addr);

	// Returns the plugin-handler slot (the address of the slot in a plugin's
	// fn-table from which the function pointer was loaded immediately before
	// the indirect call ending at `retaddr`). Zero if `retaddr` is not a
	// registered callback retaddr.
	uintptr_t handler_slot_at_retaddr(uintptr_t retaddr) const;

	// After a rebuild, find the new return address whose call site loads from
	// the same plugin-handler slot. Used by meta_apply_fix_data to re-bind
	// paused frames into the regenerated code.
	uintptr_t retaddr_for_handler_slot(uintptr_t handler_slot) const;

private:
	static bool is_hook_needed(jitdata_t* jitdata);

	void register_call_site(uintptr_t retaddr, uintptr_t handler_slot);

	static_allocator	m_callback_allocator;
	static_allocator	m_tramp_allocator;

	std::unordered_map<uintptr_t, uintptr_t>	m_retaddr_to_handler;
	std::unordered_map<uintptr_t, uintptr_t>	m_handler_to_retaddr;
};

extern CJit g_jit;
