#pragma once

/* Identitatea kernelului DevOS. KERNEL_BUILD_ID e injectat de Makefile
 * (data/ora build-ului) prin -DKERNEL_BUILD_ID; are un fallback aici. */

#define KERNEL_NAME    "DevOS"
#define KERNEL_VERSION "0.62"
#define KERNEL_ARCH    "x86_64"

#ifndef KERNEL_BUILD_ID
#define KERNEL_BUILD_ID "dev"
#endif
