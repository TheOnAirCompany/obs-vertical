#pragma once

/* Minimal test harness: no third-party dependency. */

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace t {

struct Case {
	const char *name;
	std::function<void()> fn;
};

std::vector<Case> &registry();
extern int failures;
extern const char *current;

struct Register {
	Register(const char *name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline void fail(const char *file, int line, const std::string &msg)
{
	failures++;
	std::fprintf(stderr, "FAIL %s (%s:%d): %s\n", current, file, line, msg.c_str());
}

} // namespace t

#define T_CAT2(a, b) a##b
#define T_CAT(a, b) T_CAT2(a, b)
#define TEST(name)                                                         \
	static void T_CAT(test_fn_, __LINE__)();                           \
	static t::Register T_CAT(test_reg_, __LINE__)(name, T_CAT(test_fn_, __LINE__)); \
	static void T_CAT(test_fn_, __LINE__)()

#define CHECK(cond)                                         \
	do {                                                \
		if (!(cond))                                \
			t::fail(__FILE__, __LINE__, #cond); \
	} while (0)

#define CHECK_EQ(a, b)                                                                     \
	do {                                                                                       \
		if (!((a) == (b)))                                                                 \
			t::fail(__FILE__, __LINE__, #a " == " #b);                                 \
	} while (0)

#define CHECK_NEAR(a, b, eps)                                                                              \
	do {                                                                                               \
		const double _a = (a), _b = (b);                                                           \
		if (!(std::fabs(_a - _b) <= (eps)))                                                        \
			t::fail(__FILE__, __LINE__,                                                        \
				std::string(#a " = ") + std::to_string(_a) + ", expected " + std::to_string(_b)); \
	} while (0)
