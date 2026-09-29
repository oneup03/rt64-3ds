// Host stand-in for citro3d: the PICA enums and no-op texture-environment
// calls, enough to compile the combiner planner (rt64_3ds_tev.cpp) for
// tev_test. Found before the real citro3d.h on the include path.
#pragma once
#include <cstdint>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#ifndef BIT
#define BIT(n) (1U << (n))
#endif
#include "3ds/gpu/enums.h"

enum { C3D_RGB = BIT(0), C3D_Alpha = BIT(1), C3D_Both = C3D_RGB | C3D_Alpha };
struct C3D_TexEnv { int unused; };
inline C3D_TexEnv* C3D_GetTexEnv(int) { static C3D_TexEnv env; return &env; }
inline void C3D_TexEnvInit(C3D_TexEnv*) {}
inline void C3D_TexEnvSrc(C3D_TexEnv*, int, GPU_TEVSRC, GPU_TEVSRC = GPU_PRIMARY_COLOR, GPU_TEVSRC = GPU_PRIMARY_COLOR) {}
inline void C3D_TexEnvOpRgb(C3D_TexEnv*, GPU_TEVOP_RGB, GPU_TEVOP_RGB = GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB = GPU_TEVOP_RGB_SRC_COLOR) {}
inline void C3D_TexEnvOpAlpha(C3D_TexEnv*, GPU_TEVOP_A, GPU_TEVOP_A = GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A = GPU_TEVOP_A_SRC_ALPHA) {}
inline void C3D_TexEnvFunc(C3D_TexEnv*, int, GPU_COMBINEFUNC) {}
inline void C3D_TexEnvColor(C3D_TexEnv*, u32) {}
inline void C3D_TexEnvBufUpdate(int, int) {}
inline void C3D_TexEnvBufColor(u32) {}
