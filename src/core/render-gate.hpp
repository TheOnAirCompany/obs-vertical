#pragma once

/* The vertical canvas renders its scene only while someone needs the picture:
 * an output (stream, recording, replay buffer), the dock preview, or the
 * virtual camera. Each reason is reference-counted. Pure C++17, not thread-safe
 * (the OBS layer uses it from the UI thread only). */

namespace vertical::core {

enum class RenderReason { Output = 0, Preview = 1, VirtualCam = 2 };

class RenderGate {
public:
	/* Returns true when the gate goes from idle to active. */
	bool acquire(RenderReason r)
	{
		const bool was = active();
		counts_[(int)r]++;
		return !was;
	}

	/* Returns true when the gate goes from active to idle. Releasing a reason
	 * that is not held does nothing. */
	bool release(RenderReason r)
	{
		if (counts_[(int)r] == 0)
			return false;
		counts_[(int)r]--;
		return !active();
	}

	/* Drops every holder of one reason (e.g. all outputs stopped at once). */
	bool reset(RenderReason r)
	{
		const bool was = active();
		counts_[(int)r] = 0;
		return was && !active();
	}

	bool active() const { return counts_[0] + counts_[1] + counts_[2] > 0; }
	int count(RenderReason r) const { return counts_[(int)r]; }

private:
	int counts_[3] = {0, 0, 0};
};

} // namespace vertical::core
