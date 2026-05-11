#----------------------------------------
# 1. Preparing build:
#    rm -rf build
#    mkdir build && cd build
#
# 2. Select compiler and build it
# - Compile with Clang:
#    CC="clang" CXX="clang++" cmake ..
#    make
#
# - Compile with Intel C++ Compiler:
#    CC="icc" CXX="icpc" cmake ..
#    make
#
# - Compile with GCC Compiler:
#    cmake ..
#    make
#
# Use -DCMAKE_BUILD_TYPE=COMPAT_GLIBC
# to backward compatibility with oldest version of glibc 2.11
#
# Use -DBUILD_64BIT=ON to compile 64-bit x86_64 version (default is 32-bit x86)
# Use -DBUILD_AARCH64=ON to compile 64-bit AArch64 (ARM64) version
# (auto-enabled when configuring on an aarch64 host)
#
#----------------------------------------

cmake_minimum_required(VERSION 3.24)
project(metamod CXX)

option(DEBUG "Build with debug information." OFF)
option(USE_STATIC_LIBSTDC "Enables static linking libstdc++." OFF)
option(BUILD_64BIT "Build 64-bit x86_64 version instead of 32-bit x86." OFF)
option(BUILD_AARCH64 "Build for AArch64 (ARM64) target." OFF)

# On MSVC the target arch is selected by the generator (`cmake -A x64`
# vs `cmake -A Win32`), not by a flag — derive BUILD_64BIT from
# CMAKE_SIZEOF_VOID_P so the rest of the arch logic doesn't need a
# parallel MSVC code path.
if (MSVC AND NOT BUILD_64BIT AND CMAKE_SIZEOF_VOID_P EQUAL 8)
	set(BUILD_64BIT ON)
endif()

# Auto-detect AArch64 host when neither toggle is specified explicitly.
if (NOT BUILD_AARCH64 AND NOT BUILD_64BIT
		AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
	set(BUILD_AARCH64 ON)
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Avoid -rdynamic -fPIC options
set(CMAKE_SHARED_LIBRARY_CXX_FLAGS "")
set(CMAKE_SHARED_LIBRARY_LINK_CXX_FLAGS "")

if (BUILD_AARCH64)
	# Use the toolchain's native target arch; no -m32/-m64 selector applies.
	set(ARCH_FLAG "")
	set(METAMOD_OUTPUT_NAME "metamod_aarch64")
	set(COMPAT_GLIBC_LIB_DIR "linuxaarch64")
	set(METAMOD_ARCH_X86 OFF)
	set(METAMOD_ARCH_AARCH64 ON)
elseif (BUILD_64BIT)
	# GCC needs an explicit -m64; MSVC picks 64-bit via the generator.
	if (MSVC)
		set(ARCH_FLAG "")
	else()
		set(ARCH_FLAG "-m64")
	endif()
	set(METAMOD_OUTPUT_NAME "metamod_amd64")
	set(COMPAT_GLIBC_LIB_DIR "linux64")
	set(METAMOD_ARCH_X86 ON)
	set(METAMOD_ARCH_AARCH64 OFF)
else()
	if (MSVC)
		set(ARCH_FLAG "")
	else()
		set(ARCH_FLAG "-m32")
	endif()
	set(METAMOD_OUTPUT_NAME "metamod_i386")
	set(COMPAT_GLIBC_LIB_DIR "linux32")
	set(METAMOD_ARCH_X86 ON)
	set(METAMOD_ARCH_AARCH64 OFF)
endif()

# Apply arch flag globally so the AsmJit submodule build matches.
# On MSVC the flag expands to empty (generator handles platform).
if (NOT MSVC)
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${ARCH_FLAG}")
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${ARCH_FLAG}")
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${ARCH_FLAG}")
	set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${ARCH_FLAG}")
	set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} ${ARCH_FLAG}")
	set(COMPILE_FLAGS "${ARCH_FLAG} -U_FORTIFY_SOURCE")
	set(LINK_FLAGS "${ARCH_FLAG}")
else()
	set(COMPILE_FLAGS "")
	set(LINK_FLAGS "")
endif()

set(ASMJIT_STATIC ON CACHE BOOL "Build AsmJit as a static library" FORCE)
if (METAMOD_ARCH_AARCH64)
	set(ASMJIT_NO_X86 ON CACHE BOOL "Disable X86 backend in AsmJit" FORCE)
else()
	set(ASMJIT_NO_AARCH64 ON CACHE BOOL "Disable AArch64 backend in AsmJit" FORCE)
endif()
# AsmJit links into a shared library, so its objects must be PIC.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory(extern/asmjit EXCLUDE_FROM_ALL)

if (NOT MSVC)
	set(COMPILE_FLAGS "${COMPILE_FLAGS} -Wall -fno-exceptions -fno-builtin -Wno-unknown-pragmas")

	# Remove noxref code and data
	set(COMPILE_FLAGS "${COMPILE_FLAGS} -ffunction-sections -fdata-sections")

	if (DEBUG)
		set(COMPILE_FLAGS "${COMPILE_FLAGS} -g3 -O0 -ggdb")
	else()
		set(COMPILE_FLAGS "${COMPILE_FLAGS} -g0 -O3 -fno-stack-protector")
		set(LINK_FLAGS "${LINK_FLAGS} -s")
	endif()

	# Check Intel C++ compiler
	if ("$ENV{CXX}" MATCHES "icpc")
		set(COMPILE_FLAGS "${COMPILE_FLAGS} \
			-fasm-blocks -Qoption,cpp,--treat_func_as_string_literal_cpp\
			-inline-forceinline -no-ansi-alias")

		set(LINK_FLAGS "${LINK_FLAGS} -static-intel -no-intel-extensions")

		if (NOT DEBUG)
			set(COMPILE_FLAGS "${COMPILE_FLAGS} -ipo")
			set(LINK_FLAGS "${LINK_FLAGS} -ipo -diag-disable=11003")
		endif()
	else()
		if (METAMOD_ARCH_X86)
			# Produce code optimized for the most common IA32/AMD64/EM64T processors.
			# As new processors are deployed in the marketplace, the behavior of this option will change.
			set(COMPILE_FLAGS "${COMPILE_FLAGS} \
				-mtune=generic -msse3\
				-fno-sized-deallocation -Wno-invalid-offsetof -Wno-ignored-attributes\
				-Wno-write-strings -Wno-strict-aliasing -fno-plt")
		else()
			set(COMPILE_FLAGS "${COMPILE_FLAGS} \
				-fno-sized-deallocation -Wno-invalid-offsetof -Wno-ignored-attributes\
				-Wno-write-strings -Wno-strict-aliasing -fno-plt")
		endif()

		# Check Clang compiler
		if (NOT "$ENV{CXX}" MATCHES "clang")
			set(COMPILE_FLAGS "${COMPILE_FLAGS} -Wno-maybe-uninitialized -Wno-unused-but-set-variable")

			# GCC >= 8.3
			if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 8.0)
				set(COMPILE_FLAGS "${COMPILE_FLAGS} -Wno-stringop-truncation -Wno-format-truncation -Wno-class-memaccess")
			endif()
		endif()
	endif()

	# -fcf-protection is x86-only (control-flow protection / IBT/SHSTK).
	if (METAMOD_ARCH_X86 AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 8.0)
		set(COMPILE_FLAGS "${COMPILE_FLAGS} -fcf-protection=none")
	endif()

	if (NOT DEBUG)
		set(LINK_FLAGS "${LINK_FLAGS} \
			-Wl,-gc-sections -Wl,--version-script=\"${PROJECT_SOURCE_DIR}/../version_script.lds\"")
	endif()

	if (CMAKE_BUILD_TYPE MATCHES COMPAT_GLIBC)
		set(LINK_FLAGS	"${LINK_FLAGS} -Wl,--wrap=stat64")
	endif()
else()
	# MSVC flags. /MP = multi-process compile; /W3 = standard warning level;
	# /EHsc = standard C++ exception model; /Zc:__cplusplus so the
	# __cplusplus macro reflects the actual standard.
	set(COMPILE_FLAGS "/MP /W3 /EHsc /Zc:__cplusplus")
	if (DEBUG)
		set(COMPILE_FLAGS "${COMPILE_FLAGS} /Od /Zi")
	else()
		set(COMPILE_FLAGS "${COMPILE_FLAGS} /O2 /Oi /GR-")
	endif()
endif()

set(PROJECT_SRC_DIR
	"${PROJECT_SOURCE_DIR}"
	"${PROJECT_SOURCE_DIR}/src"
	"${PROJECT_SOURCE_DIR}/include/engine"
	"${PROJECT_SOURCE_DIR}/include/common"
	"${PROJECT_SOURCE_DIR}/include/dlls"
	"${PROJECT_SOURCE_DIR}/include/game_shared"
	"${PROJECT_SOURCE_DIR}/include/pm_shared"
	"${PROJECT_SOURCE_DIR}/include/public"
)

set(PROJECT_PUBLIC_DIR
	"${PROJECT_SOURCE_DIR}/include/public"
	"${PROJECT_SOURCE_DIR}/include/public/metamod"
)

add_library(metamod SHARED ${appversion.sh})

if (NOT WIN32)
	if (NOT TARGET appversion)
		add_custom_target(appversion DEPENDS COMMAND "${PROJECT_SOURCE_DIR}/version/appversion.sh" "${PROJECT_SOURCE_DIR}/..")
	endif()
	add_dependencies(metamod appversion)
endif()

target_include_directories(metamod PRIVATE
	${PROJECT_SRC_DIR}
	${PROJECT_PUBLIC_DIR}
)

target_compile_definitions(metamod PRIVATE
	NDEBUG
	METAMOD_CORE
)
if (NOT WIN32)
	target_compile_definitions(metamod PRIVATE
		_LINUX
		LINUX
		_GLIBCXX_USE_CXX11_ABI=0
		_stricmp=strcasecmp
		_strnicmp=strncasecmp
		_strdup=strdup
		_unlink=unlink
		_write=write
		_close=close
		_getcwd=getcwd
		_vsnprintf=vsnprintf
		_vsnwprintf=vswprintf
		_snprintf=snprintf
	)
else()
	target_compile_definitions(metamod PRIVATE
		WIN32
		_WINDOWS
		_CRT_SECURE_NO_DEPRECATE
		_CRT_SECURE_NO_WARNINGS
		_CRT_NONSTDC_NO_DEPRECATE
		NOMINMAX
	)
endif()

target_sources(metamod PRIVATE
	"src/api_info.cpp"
	"src/callback_jit.cpp"
	"src/callback_jit_orchestration.cpp"
	"src/commands_meta.cpp"
	"src/conf_meta.cpp"
	"src/dllapi.cpp"
	"src/engine_api.cpp"
	"src/game_support.cpp"
	"src/h_export.cpp"
	"src/log_meta.cpp"
	"src/mdebug.cpp"
	"src/mem_utils.cpp"
	"src/meta_rehlds_api.cpp"
	"src/metamod.cpp"
	"src/mextdll.cpp"
	"src/mlist.cpp"
	"src/mplayer.cpp"
	"src/mplugin.cpp"
	"src/mreg.cpp"
	"src/mutil.cpp"
	"src/precompiled.cpp"
	"src/public_amalgamation.cpp"
	"src/reg_support.cpp"
	"src/sdk_util.cpp"
	"src/studioapi.cpp"
	"src/sys_module.cpp"
	"src/utils.cpp"
)

if (WIN32)
	target_sources(metamod PRIVATE "src/osdep_linkent_win32.cpp")
else()
	target_sources(metamod PRIVATE
		"src/osdep_linkent_linux.cpp"
		"src/glibc_compat.cpp"
	)
endif()

if (METAMOD_ARCH_AARCH64)
	target_sources(metamod PRIVATE "src/callback_jit_a64.cpp")
else()
	target_sources(metamod PRIVATE "src/callback_jit_x86.cpp")
endif()

target_link_libraries(metamod PRIVATE
	asmjit::asmjit
)
if (NOT WIN32)
	target_link_libraries(metamod PRIVATE
		dl
		pthread
		$<$<CONFIG:COMPAT_GLIBC>:libc-2.11.1.so>
	)
endif()

if (USE_STATIC_LIBSTDC AND NOT MSVC)
	target_compile_definitions(metamod PRIVATE BUILD_STATIC_LIBSTDC)
	set(LINK_FLAGS "${LINK_FLAGS} -static-libgcc -static-libstdc++")
endif()

if (CMAKE_BUILD_TYPE MATCHES COMPAT_GLIBC AND NOT MSVC)
	set(LINK_FLAGS "${LINK_FLAGS} \
		-Wl,-rpath,'$ORIGIN/.' \
		-L${PROJECT_SOURCE_DIR}/lib/${COMPAT_GLIBC_LIB_DIR}")
endif()

set_target_properties(metamod PROPERTIES
	OUTPUT_NAME ${METAMOD_OUTPUT_NAME}
	PREFIX ""
	COMPILE_FLAGS ${COMPILE_FLAGS}
	LINK_FLAGS ${LINK_FLAGS}
	POSITION_INDEPENDENT_CODE ON
)
