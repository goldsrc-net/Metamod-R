cmake_minimum_required(VERSION 3.24)
project(metamod CXX)

# Linux uses the appversion.sh helper; on Windows the equivalent is
# pre-baked into the source tree (or generated via the .sln's
# pre-build script) — skip the custom target.
if (NOT WIN32)
	add_custom_target(appversion DEPENDS
		COMMAND "${PROJECT_SOURCE_DIR}/metamod/version/appversion.sh" "${PROJECT_SOURCE_DIR}"
	)
endif()

add_subdirectory(metamod)
