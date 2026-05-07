# CMake toolchain file for aarch64-linux-gnu cross-compilation.
#
# Usage:
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-linux.cmake -B build_aarch64

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Pick whichever versioned compiler is present (the unversioned alias is in
# gcc-aarch64-linux-gnu, which isn't always installed).
if (NOT CMAKE_C_COMPILER)
	find_program(_AARCH64_CC NAMES
		aarch64-linux-gnu-gcc
		aarch64-linux-gnu-gcc-14
		aarch64-linux-gnu-gcc-13
		aarch64-linux-gnu-gcc-12
		aarch64-linux-gnu-gcc-11
	)
	if (_AARCH64_CC)
		set(CMAKE_C_COMPILER ${_AARCH64_CC})
	else()
		message(FATAL_ERROR "No aarch64-linux-gnu-gcc found in PATH")
	endif()
endif()

if (NOT CMAKE_CXX_COMPILER)
	find_program(_AARCH64_CXX NAMES
		aarch64-linux-gnu-g++
		aarch64-linux-gnu-g++-14
		aarch64-linux-gnu-g++-13
		aarch64-linux-gnu-g++-12
		aarch64-linux-gnu-g++-11
	)
	if (_AARCH64_CXX)
		set(CMAKE_CXX_COMPILER ${_AARCH64_CXX})
	else()
		message(FATAL_ERROR "No aarch64-linux-gnu-g++ found in PATH")
	endif()
endif()

# Don't probe the host system for libraries; only the cross sysroot.
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
