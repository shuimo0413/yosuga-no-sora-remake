/*

	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000-2009 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"

*/

#include "tjsCommHead.h"
#include "tvpgl.h"

#if defined(__aarch64__) || defined(__arm64__)

#include <arm_neon.h>

#include <array>
#include <cstring>

#include "DebugIntf.h"
#include "SysInitIntf.h"

extern "C" {
extern unsigned char TVPOpacityOnOpacityTable[256 * 256];
extern unsigned char TVPNegativeMulTable[256 * 256];
}

namespace {

inline tjs_uint32 TVPAlphaBlendDestAlphaPixel(tjs_uint32 d, tjs_uint32 s,
											  tjs_uint32 addr) {
	const tjs_uint32 sopa = TVPOpacityOnOpacityTable[addr];
	tjs_uint32 d1 = d & 0x00ff00ffu;
	d1 = (d1 + (((s & 0x00ff00ffu) - d1) * sopa >> 8)) & 0x00ff00ffu;
	d &= 0x0000ff00u;
	s &= 0x0000ff00u;
	return d1 + ((d + ((s - d) * sopa >> 8)) & 0x0000ff00u) +
		(static_cast<tjs_uint32>(TVPNegativeMulTable[addr]) << 24);
}

inline uint32x4_t TVPAlphaBlendPackedNEON(uint32x4_t d, uint32x4_t s,
										  uint32x4_t sopa) {
	const uint32x4_t red_blue_mask = vdupq_n_u32(0x00ff00ffu);
	const uint32x4_t green_mask = vdupq_n_u32(0x0000ff00u);
	const uint32x4_t d_red_blue = vandq_u32(d, red_blue_mask);
	const uint32x4_t s_red_blue = vandq_u32(s, red_blue_mask);
	const uint32x4_t red_blue = vandq_u32(
		vaddq_u32(d_red_blue,
			vshrq_n_u32(vmulq_u32(vsubq_u32(s_red_blue, d_red_blue), sopa), 8)),
		red_blue_mask);
	const uint32x4_t d_green = vandq_u32(d, green_mask);
	const uint32x4_t s_green = vandq_u32(s, green_mask);
	const uint32x4_t green = vandq_u32(
		vaddq_u32(d_green,
			vshrq_n_u32(vmulq_u32(vsubq_u32(s_green, d_green), sopa), 8)),
		green_mask);
	return vaddq_u32(red_blue, green);
}

template <bool HasOpacity>
void TVPAlphaBlendDestAlphaNEON(tjs_uint32 *dest, const tjs_uint32 *src,
								 tjs_int len, tjs_int opacity) {
	while (len >= 8) {
		alignas(16) tjs_uint32 blend_opacity[8];
		alignas(16) tjs_uint32 destination_alpha[8];

		for (tjs_int i = 0; i < 8; ++i) {
			const tjs_uint32 d = dest[i];
			const tjs_uint32 s = src[i];
			const tjs_uint32 addr = HasOpacity
				? ((((s >> 24) * static_cast<tjs_uint32>(opacity)) & 0x0000ff00u) +
				   (d >> 24))
				: (((s >> 16) & 0x0000ff00u) + (d >> 24));
			blend_opacity[i] = TVPOpacityOnOpacityTable[addr];
			destination_alpha[i] =
				static_cast<tjs_uint32>(TVPNegativeMulTable[addr]) << 24;
		}

		const uint32x4_t destination0 = vld1q_u32(dest);
		const uint32x4_t destination1 = vld1q_u32(dest + 4);
		const uint32x4_t source0 = vld1q_u32(src);
		const uint32x4_t source1 = vld1q_u32(src + 4);
		const uint32x4_t result0 = TVPAlphaBlendPackedNEON(
			destination0, source0, vld1q_u32(blend_opacity));
		const uint32x4_t result1 = TVPAlphaBlendPackedNEON(
			destination1, source1, vld1q_u32(blend_opacity + 4));
		vst1q_u32(dest, vaddq_u32(result0, vld1q_u32(destination_alpha)));
		vst1q_u32(dest + 4,
			vaddq_u32(result1, vld1q_u32(destination_alpha + 4)));

		dest += 8;
		src += 8;
		len -= 8;
	}

	while (len-- > 0) {
		const tjs_uint32 d = *dest;
		const tjs_uint32 s = *src;
		const tjs_uint32 addr = HasOpacity
			? ((((s >> 24) * static_cast<tjs_uint32>(opacity)) & 0x0000ff00u) +
			   (d >> 24))
			: (((s >> 16) & 0x0000ff00u) + (d >> 24));
		*dest++ = TVPAlphaBlendDestAlphaPixel(d, s, addr);
		++src;
	}
}

void TVPAlphaBlend_d_neon(tjs_uint32 *dest, const tjs_uint32 *src, tjs_int len) {
	TVPAlphaBlendDestAlphaNEON<false>(dest, src, len, 255);
}

void TVPAlphaBlend_do_neon(tjs_uint32 *dest, const tjs_uint32 *src,
							 tjs_int len, tjs_int opacity) {
	TVPAlphaBlendDestAlphaNEON<true>(dest, src, len, opacity);
}

bool TVPNEONBlendRequested() {
	tTJSVariant value;
	if (!TVPGetCommandLine(TJS_W("-cpuneon"), &value)) return true;
	const ttstr option(value);
	return option != TJS_W("no") && option != TJS_W("false") &&
		option != TJS_W("0");
}

bool TVPVerifyNEONBlend() {
	if (!TVPAlphaBlend_d || !TVPAlphaBlend_do) return false;

	constexpr tjs_int TestPixels = 521;
	std::array<tjs_uint32, TestPixels> source{};
	std::array<tjs_uint32, TestPixels> initial{};
	std::array<tjs_uint32, TestPixels> expected{};
	std::array<tjs_uint32, TestPixels> actual{};
	tjs_uint32 state = 0x4b525a32u;
	for (tjs_int i = 0; i < TestPixels; ++i) {
		state = state * 1664525u + 1013904223u;
		source[i] = state;
		state = state * 1664525u + 1013904223u;
		initial[i] = state;

		// Exercise fully transparent/opaque pixels and every alpha byte value.
		source[i] = (source[i] & 0x00ffffffu) |
			(static_cast<tjs_uint32>(i & 0xff) << 24);
		initial[i] = (initial[i] & 0x00ffffffu) |
			(static_cast<tjs_uint32>((i * 73) & 0xff) << 24);
	}

	expected = initial;
	actual = initial;
	TVPAlphaBlend_d(expected.data(), source.data(), TestPixels);
	TVPAlphaBlend_d_neon(actual.data(), source.data(), TestPixels);
	if (std::memcmp(expected.data(), actual.data(), sizeof(expected)) != 0)
		return false;

	// The opacity parameter is only one byte in the table address, so verifying
	// all 256 values is cheap and makes the runtime fallback deterministic.
	for (tjs_int opacity = 0; opacity < 256; ++opacity) {
		expected = initial;
		actual = initial;
		TVPAlphaBlend_do(expected.data(), source.data(), TestPixels, opacity);
		TVPAlphaBlend_do_neon(actual.data(), source.data(), TestPixels, opacity);
		if (std::memcmp(expected.data(), actual.data(), sizeof(expected)) != 0)
			return false;
	}
	return true;
}

} // namespace

#endif // defined(__aarch64__) || defined(__arm64__)

void TVPGL_NEON_Init() {
#if defined(__aarch64__) || defined(__arm64__)
	// Advanced SIMD is mandatory in AArch64. Other architectures compile this
	// initializer as a no-op and keep the already selected C/SSE implementation.
	if (!TVPNEONBlendRequested()) {
		TVPAddImportantLog(TJS_W("NEON alpha blend disabled; using fallback"));
		return;
	}
	if (!TVPVerifyNEONBlend()) {
		TVPAddImportantLog(
			TJS_W("NEON alpha blend verification failed; using fallback"));
		return;
	}

	TVPAlphaBlend_d = TVPAlphaBlend_d_neon;
	TVPAlphaBlend_do = TVPAlphaBlend_do_neon;
	TVPAddImportantLog(TJS_W("NEON alpha blend enabled"));
#endif
}
