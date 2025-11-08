cmake_minimum_required(VERSION 3.18)

# Version information
set(F4SEVR_DLSS_VERSION_MAJOR 1)
set(F4SEVR_DLSS_VERSION_MINOR 0)
set(F4SEVR_DLSS_VERSION_PATCH 0)

project(
	F4SEVR_DLSS
	VERSION ${F4SEVR_DLSS_VERSION_MAJOR}.${F4SEVR_DLSS_VERSION_MINOR}.${F4SEVR_DLSS_VERSION_PATCH}
	LANGUAGES CXX
)

# ---- Include guards ----

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
	message(
		FATAL_ERROR
			"In-source builds not allowed. Please make a new directory (called a build directory) and run CMake from there."
)
endif()

# ---- Dependencies ---- 
include(cmake/versioning.cmake)

set(NGX_SDK_PATH "" CACHE PATH "Path to the NVIDIA NGX SDK root directory")
set(USE_STREAMLINE ON CACHE BOOL "Enable NVIDIA Streamline backend for DLSS")
set(SL_SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/streamline-sdk-v2.9.0" CACHE PATH "Path to NVIDIA Streamline SDK root")

if(NOT NGX_SDK_PATH)
	set(_default_ngx "${CMAKE_CURRENT_SOURCE_DIR}/DLSS-310.4.0")
	if(EXISTS "${_default_ngx}/include/nvsdk_ngx.h")
		set(NGX_SDK_PATH "${_default_ngx}")
		message(STATUS "Defaulting NGX_SDK_PATH to ${NGX_SDK_PATH}")
	else()
		message(WARNING "NGX_SDK_PATH is not set; ensure NVIDIA NGX SDK headers are available.")
	endif()
endif()

# ---- Plugin source files include ----

include(cmake/headerlist.cmake)
include(cmake/sourcelist.cmake)

add_library(${PROJECT_NAME} SHARED "${sources}")

set_target_properties(
	${PROJECT_NAME}
	PROPERTIES
		OUTPUT_NAME "${PROJECT_NAME}_${F4_VERSION_MAJOR}_${F4_VERSION_MINOR}_${F4_VERSION_PATCH}"
)

target_compile_definitions(
	${PROJECT_NAME}
	PRIVATE
		RUNTIME
		RUNTIME_VERSION=${F4_VERSION_PACKED}
		NOMINMAX
)

target_compile_features(
    ${PROJECT_NAME}
    PUBLIC
        cxx_std_17
)

target_include_directories(
    ${PROJECT_NAME}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        include
        third_party/imgui
        third_party/imgui/backends
)

if(NGX_SDK_PATH)
    target_include_directories(
        ${PROJECT_NAME}
        PUBLIC
            "${NGX_SDK_PATH}/include"
    )
endif()

if(USE_STREAMLINE)
    if(EXISTS "${SL_SDK_PATH}/include/sl.h")
        target_compile_definitions(${PROJECT_NAME} PRIVATE USE_STREAMLINE=1)
        target_include_directories(${PROJECT_NAME} PUBLIC "${SL_SDK_PATH}/include")
        message(STATUS "Streamline SDK enabled: ${SL_SDK_PATH}")
    else()
        message(WARNING "USE_STREAMLINE is ON but SL headers not found under ${SL_SDK_PATH}; disabling")
        target_compile_definitions(${PROJECT_NAME} PRIVATE USE_STREAMLINE=0)
    endif()
endif()

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /EHsc)
endif()

target_link_libraries(
    ${PROJECT_NAME}
    PUBLIC
        d3d11
        dxgi
        d3dcompiler
        dwmapi
        gdi32
        user32
        kernel32
        uuid
        advapi32
        shell32
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/f4sevr_1_2_72.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/f4se_common.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/common_vc11.lib
)

# Link NGX SDK static stub so missing entry points are resolved on recent runtimes
if(MSVC AND NGX_SDK_PATH)
    # NGX libs live under lib/Windows_x86_64/x64 for desktop x64
    set(_NGX_LIB_DIR "${NGX_SDK_PATH}/lib/Windows_x86_64/x64")
    if(EXISTS "${_NGX_LIB_DIR}/nvsdk_ngx_d.lib")
        target_link_directories(${PROJECT_NAME} PRIVATE "${_NGX_LIB_DIR}")
        target_link_libraries(${PROJECT_NAME} PUBLIC nvsdk_ngx_d.lib)
        message(STATUS "Linking against NGX stub (dynamic CRT): ${_NGX_LIB_DIR}/nvsdk_ngx_d.lib")
    elseif(EXISTS "${_NGX_LIB_DIR}/nvsdk_ngx_s.lib")
        target_link_directories(${PROJECT_NAME} PRIVATE "${_NGX_LIB_DIR}")
        target_link_libraries(${PROJECT_NAME} PUBLIC nvsdk_ngx_s.lib)
        message(STATUS "Linking against NGX static stub: ${_NGX_LIB_DIR}/nvsdk_ngx_s.lib")
    else()
        message(WARNING "nvsdk_ngx_*.lib not found under ${_NGX_LIB_DIR}. Ensure NGX_SDK_PATH is set correctly.")
    endif()
endif()

if(MSVC)
    # Export F4SEPlugin_* symbols via .def
    target_link_options(${PROJECT_NAME} PRIVATE "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/exports.def")
endif()





