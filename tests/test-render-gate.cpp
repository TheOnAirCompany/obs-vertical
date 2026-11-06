#include "test.hpp"

#include "render-gate.hpp"

using namespace vertical::core;

TEST("gate: idle until a reason is held")
{
	RenderGate g;
	CHECK(!g.active());
	CHECK(g.acquire(RenderReason::Preview));
	CHECK(g.active());
	CHECK(!g.acquire(RenderReason::Output)); /* already active */
	CHECK(!g.release(RenderReason::Preview)); /* output still holds it */
	CHECK(g.release(RenderReason::Output));
	CHECK(!g.active());
}

TEST("gate: counts per reason, extra releases are ignored")
{
	RenderGate g;
	g.acquire(RenderReason::Output);
	g.acquire(RenderReason::Output);
	CHECK_EQ(g.count(RenderReason::Output), 2);
	CHECK(!g.release(RenderReason::Output));
	CHECK(g.release(RenderReason::Output));
	CHECK(!g.release(RenderReason::Output));
	CHECK_EQ(g.count(RenderReason::Output), 0);
	CHECK(!g.release(RenderReason::VirtualCam));
}

TEST("gate: reset drops a reason")
{
	RenderGate g;
	g.acquire(RenderReason::Output);
	g.acquire(RenderReason::Output);
	g.acquire(RenderReason::VirtualCam);
	CHECK(!g.reset(RenderReason::Output));
	CHECK(g.reset(RenderReason::VirtualCam));
	CHECK(!g.active());
	CHECK(!g.reset(RenderReason::Preview));
}
