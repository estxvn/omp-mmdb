#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
#define MMDB_TEST_X86
#if defined(_MSC_VER)
#include <immintrin.h>
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace mmdb::test {

struct FloatingPointState {
	std::uint32_t mxcsr{};
	std::uint16_t x87_control{};
	std::uint16_t x87_exceptions{};
	bool operator==(const FloatingPointState&) const = default;
};

class FloatingPointEnvironment {
public:
	static constexpr std::uint32_t daz = 1U << 6;
	static constexpr std::uint32_t ftz = 1U << 15;
	static constexpr std::uint32_t sse_rounding = 3U << 13;
	static constexpr std::uint32_t x87_rounding_and_precision = 0x0f00U;

	FloatingPointEnvironment() noexcept : mxcsr_mask_(supported_mxcsr()), saved_(state()) {}
	~FloatingPointEnvironment() { write(saved_); }
	FloatingPointEnvironment(const FloatingPointEnvironment&) = delete;
	FloatingPointEnvironment& operator=(const FloatingPointEnvironment&) = delete;

	FloatingPointState state() const noexcept {
		FloatingPointState current;
#if defined(MMDB_TEST_X86)
#if defined(_MSC_VER)
		if (mxcsr_mask_ != 0) current.mxcsr = _mm_getcsr();
#if defined(_M_X64)
		// The Windows x64 ABI uses SSE for floating-point operations.
		const auto saved = fxsave();
		std::memcpy(&current.x87_control, saved.bytes, sizeof(current.x87_control));
		std::memcpy(&current.x87_exceptions, saved.bytes + 2, sizeof(current.x87_exceptions));
#else
		std::uint16_t control, status;
		__asm fnstcw control
		__asm fnstsw status
		current.x87_control = control;
		current.x87_exceptions = status;
#endif
#else
		if (mxcsr_mask_ != 0) __asm__ volatile("stmxcsr %0" : "=m"(current.mxcsr));
		__asm__ volatile("fnstcw %0" : "=m"(current.x87_control));
		__asm__ volatile("fnstsw %0" : "=m"(current.x87_exceptions));
#endif
		current.x87_exceptions &= 0x003f;
#endif
		return current;
	}

	template <typename Check>
	std::size_t exercise(Check check) const {
		std::size_t combinations = 0;
		for (const auto flush : {0U, ftz, daz, ftz | daz}) {
			for (auto sse = 0U; sse <= sse_rounding; sse += 1U << 13) {
				const auto mxcsr = flush | sse;
				if ((mxcsr & ~mxcsr_mask_) != 0) continue;
				const auto modes = can_write_x87 ? 4U : 1U;
				for (auto x87 = 0U; x87 < modes; ++x87) {
					for (const auto precision : x87_precisions) {
						auto next = saved_;
						next.mxcsr = (next.mxcsr & ~(ftz | daz | sse_rounding)) | mxcsr;
						if (can_write_x87) {
						next.x87_control = static_cast<std::uint16_t>((next.x87_control & ~x87_rounding_and_precision)
								| (x87 << 10) | precision);
						}
						write(next);
						check(next);
						++combinations;
					}
				}
			}
		}
		return combinations;
	}

private:
#if defined(MMDB_TEST_X86) && !(defined(_MSC_VER) && defined(_M_X64))
	static constexpr bool can_write_x87 = true;
	static constexpr std::array x87_precisions{0U, 0x0200U, 0x0300U}; // 24, 53, 64 bits.
#else
	static constexpr bool can_write_x87 = false;
	static constexpr std::array x87_precisions{0U};
#endif

#if defined(MMDB_TEST_X86)
	struct alignas(16) FxState {
		unsigned char bytes[512];
	};

	static FxState fxsave() noexcept {
		FxState state{};
#if defined(_MSC_VER)
		_fxsave(state.bytes);
#else
		__asm__ volatile("fxsave %0" : "=m"(state));
#endif
		return state;
	}
#endif

	static std::uint32_t supported_mxcsr() noexcept {
#if defined(MMDB_TEST_X86)
#if defined(_M_IX86) || defined(__i386__)
		// A 32-bit build can run on a host without SSE or FXSAVE.
#if defined(_MSC_VER)
		int registers[4]{};
		__cpuid(registers, 1);
		const auto edx = static_cast<unsigned>(registers[3]);
#else
		unsigned eax, ebx, ecx, edx;
		if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) == 0) return 0;
#endif
		constexpr auto required = (1U << 24) | (1U << 25);
		if ((edx & required) != required) return 0;
#endif
		std::uint32_t mask;
		std::memcpy(&mask, fxsave().bytes + 28, sizeof(mask));
		return mask == 0 ? 0xffbfU : mask;
#else
		return 0;
#endif
	}

	void write(FloatingPointState value) const noexcept {
#if defined(MMDB_TEST_X86)
#if defined(_MSC_VER)
		if (mxcsr_mask_ != 0) _mm_setcsr(value.mxcsr);
#if defined(_M_IX86)
		const auto x87 = value.x87_control;
		__asm fldcw x87
#endif
#else
		if (mxcsr_mask_ != 0) __asm__ volatile("ldmxcsr %0" : : "m"(value.mxcsr) : "memory");
		__asm__ volatile("fldcw %0" : : "m"(value.x87_control) : "memory");
#endif
#else
		(void)value;
#endif
	}

	std::uint32_t mxcsr_mask_;
	FloatingPointState saved_;
};

} // namespace mmdb::test

#undef MMDB_TEST_X86
