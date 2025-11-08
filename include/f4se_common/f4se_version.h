#pragma once

#include <cstdint>

#ifndef F4SE_TYPES_UINT32_DEFINED
#define F4SE_TYPES_UINT32_DEFINED
using UInt32 = std::uint32_t;
#endif

#define MAKE_EXE_VERSION_EX(major, minor, build, sub) \
	((((major) & 0xFF) << 24) | (((minor) & 0xFF) << 16) | (((build) & 0xFFF) << 4) | ((sub) & 0xF))

#define MAKE_EXE_VERSION(major, minor, build) \
	MAKE_EXE_VERSION_EX(major, minor, build, 0)

#define GET_EXE_VERSION_MAJOR(a) (((a) & 0xFF000000) >> 24)
#define GET_EXE_VERSION_MINOR(a) (((a) & 0x00FF0000) >> 16)
#define GET_EXE_VERSION_BUILD(a) (((a) & 0x0000FFF0) >> 4)
#define GET_EXE_VERSION_SUB(a)   (((a) & 0x0000000F) >> 0)

constexpr UInt32 RUNTIME_VR_VERSION_1_2_72 = MAKE_EXE_VERSION(1, 2, 72);
