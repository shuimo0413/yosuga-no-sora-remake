/* SPDX-License-Identifier: MIT */
/* Copyright (c) Kirikiri SDL2 Developers */

#pragma once
#include "tjsCommHead.h"

/* The OpenHarmony NDK builds with -fvisibility=hidden; the platform hooks
 * below are called from the NAPI entry module in libentry.so and must be
 * exported from libkrkrsdl2.so. */
#if defined(__OHOS__)
#if defined(__GNUC__) || defined(__clang__)
#define KRKRSDL2_OHOS_EXPORT __attribute__((visibility("default")))
#else
#define KRKRSDL2_OHOS_EXPORT
#endif
#else
#define KRKRSDL2_OHOS_EXPORT
#endif

KRKRSDL2_OHOS_EXPORT extern void krkrsdl2_pre_init_platform(void);
KRKRSDL2_OHOS_EXPORT extern void krkrsdl2_set_args(int argc, tjs_char **argv);
KRKRSDL2_OHOS_EXPORT extern void krkrsdl2_convert_set_args(int argc, char **argv);
KRKRSDL2_OHOS_EXPORT extern bool krkrsdl2_init_platform(void);
KRKRSDL2_OHOS_EXPORT extern void krkrsdl2_run_main_loop(void);
KRKRSDL2_OHOS_EXPORT extern void krkrsdl2_cleanup(void);
