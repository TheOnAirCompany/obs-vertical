#include "test.hpp"

namespace t {
std::vector<Case> &registry()
{
	static std::vector<Case> r;
	return r;
}
int failures = 0;
const char *current = "";
} // namespace t

int main()
{
	int run = 0;
	for (const t::Case &c : t::registry()) {
		t::current = c.name;
		const int before = t::failures;
		c.fn();
		run++;
		if (t::failures == before)
			std::printf("ok   %s\n", c.name);
	}
	std::printf("\n%d tests, %d failed checks\n", run, t::failures);
	return t::failures == 0 ? 0 : 1;
}
