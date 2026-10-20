// Internal renderer call surface: the native Mac subset has no Windows SDK dependency.
#pragma once
#ifdef __APPLE__
#include "platform/macos/metal_compat.h"
#else
#include <d3d11.h>
#include <d3dcompiler.h>
#endif
