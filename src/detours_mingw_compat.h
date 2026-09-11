// Copyright (c) 2026 AydoganCan60- MIT License
#pragma once

#if defined(__GNUC__) && !defined(_MSC_VER)
#define _MSC_VER 1900
#ifdef __try
#undef __try
#endif
#define __try
#define __except(expression) if (false)
#endif
