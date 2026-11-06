#include "test.hpp"

#include "scene-links.hpp"

using namespace vertical::core;

TEST("links: link, relink, unlink")
{
	SceneLinks l;
	CHECK(l.link("Main Cam", "V Cam"));
	CHECK(!l.link("Main Cam", "V Cam"));
	CHECK(l.link("Main Cam", "V Wide"));
	CHECK_EQ(l.size(), 1u);
	CHECK_EQ(*l.verticalFor("Main Cam"), std::string("V Wide"));
	CHECK(!l.verticalFor("Other"));
	CHECK(!l.link("", "V"));
	CHECK(!l.link("M", ""));
	CHECK(l.unlink("Main Cam"));
	CHECK(!l.unlink("Main Cam"));
	CHECK(l.empty());
}

TEST("links: several main scenes to one vertical scene")
{
	SceneLinks l;
	l.link("Intro", "V Logo");
	l.link("Outro", "V Logo");
	l.link("Game", "V Game");
	const auto mains = l.mainScenesFor("V Logo");
	CHECK_EQ(mains.size(), 2u);
	l.removeVertical("V Logo");
	CHECK_EQ(l.size(), 1u);
	CHECK(l.verticalFor("Game"));
}

TEST("links: renames and removals")
{
	SceneLinks l;
	l.link("A", "VA");
	l.link("B", "VB");
	l.renameMain("A", "A2");
	CHECK(!l.verticalFor("A"));
	CHECK_EQ(*l.verticalFor("A2"), std::string("VA"));
	l.renameVertical("VB", "VB2");
	CHECK_EQ(*l.verticalFor("B"), std::string("VB2"));
	/* Renaming onto a linked name: the renamed scene keeps its link. */
	l.renameMain("A2", "B");
	CHECK_EQ(l.size(), 1u);
	CHECK_EQ(*l.verticalFor("B"), std::string("VA"));
	l.removeMain("B");
	CHECK(l.empty());
}

TEST("links: json round trip")
{
	SceneLinks l;
	l.link("Main \xC3\xA9", "Vertical \"1\"");
	l.link("Second", "V2");
	const SceneLinks back = SceneLinks::fromJson(Json::parse(l.toJson().dump()));
	CHECK(back == l);
	CHECK(SceneLinks::fromJson(Json::parse("[{\"main\":\"\"},{\"x\":1},3]")).empty());
}

TEST("links: switch decision")
{
	SceneLinks l;
	l.link("Main", "V1");
	l.link("Gone", "Deleted");
	const std::vector<std::string> scenes{"V1", "V2"};
	CHECK(!decideSwitch(l, false, "Main", "V2", scenes).doSwitch);
	SwitchDecision d = decideSwitch(l, true, "Main", "V2", scenes);
	CHECK(d.doSwitch);
	CHECK_EQ(d.target, std::string("V1"));
	CHECK(!decideSwitch(l, true, "Main", "V1", scenes).doSwitch);   /* already on air */
	CHECK(!decideSwitch(l, true, "Unlinked", "V2", scenes).doSwitch); /* keeps its scene */
	CHECK(!decideSwitch(l, true, "Gone", "V2", scenes).doSwitch);     /* target missing */
}

TEST("order: saved order first, new scenes after")
{
	const auto o = applyOrder({"C", "A", "B", "D"}, {"B", "X", "A", "B"});
	CHECK_EQ(o.size(), 4u);
	CHECK_EQ(o[0], std::string("B"));
	CHECK_EQ(o[1], std::string("A"));
	CHECK_EQ(o[2], std::string("C"));
	CHECK_EQ(o[3], std::string("D"));
	CHECK(applyOrder({}, {"A"}).empty());
}

TEST("order: move items")
{
	std::vector<std::string> v{"A", "B", "C"};
	CHECK(moveItem(v, 0, 2));
	CHECK_EQ(v[2], std::string("A"));
	CHECK(moveItem(v, 2, -5));
	CHECK_EQ(v[0], std::string("A"));
	CHECK(!moveItem(v, 1, 1));
	CHECK(!moveItem(v, 5, 0));
}

TEST("names: unique scene names")
{
	CHECK_EQ(uniqueName("Vertical Scene", {}), std::string("Vertical Scene"));
	CHECK_EQ(uniqueName("Vertical Scene", {"Vertical Scene", "Vertical Scene 2"}), std::string("Vertical Scene 3"));
}
