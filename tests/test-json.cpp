#include "test.hpp"

#include "json.hpp"

using vertical::core::Json;

TEST("json: parse scalars and containers")
{
	std::string err;
	const Json j = Json::parse(R"({"a": 1, "b": [true, false, null], "c": "x\"y", "d": -2.5e1})", &err);
	CHECK(err.empty());
	CHECK(j.isObject());
	CHECK_EQ(j.get("a").asInt(), 1);
	CHECK_EQ(j.get("b").size(), 3u);
	CHECK(j.get("b").items()[0].asBool());
	CHECK(j.get("b").items()[2].isNull());
	CHECK_EQ(j.get("c").asString(), std::string("x\"y"));
	CHECK_NEAR(j.get("d").asNumber(), -25.0, 1e-9);
	CHECK(j.get("missing").isNull());
}

TEST("json: unicode escapes become UTF-8")
{
	const Json j = Json::parse(R"(["\u00e9", "\ud83d\ude00", "\u00d7"])");
	CHECK_EQ(j.items()[0].asString(), std::string("\xC3\xA9"));
	CHECK_EQ(j.items()[1].asString(), std::string("\xF0\x9F\x98\x80"));
	CHECK_EQ(j.items()[2].asString(), std::string("\xC3\x97"));
}

TEST("json: errors give null and a message")
{
	std::string err;
	CHECK(Json::parse("{\"a\": }", &err).isNull());
	CHECK(!err.empty());
	CHECK(Json::parse("[1, 2", &err).isNull());
	CHECK(Json::parse("{} x", &err).isNull());
	CHECK(Json::parse("\"bad \\q\"", &err).isNull());
	CHECK(Json::parse("", &err).isNull());
	/* Deep nesting is refused instead of overflowing the stack. */
	CHECK(Json::parse(std::string(200, '[') + std::string(200, ']'), &err).isNull());
}

TEST("json: dump round trip keeps order and escapes")
{
	Json o = Json::object();
	o.set("z", 1);
	o.set("a", "line\nbreak \x01");
	Json arr = Json::array();
	arr.push(1.5);
	arr.push(Json());
	o.set("arr", arr);
	const std::string text = o.dump();
	CHECK_EQ(text, std::string(R"({"z":1,"a":"line\nbreak \u0001","arr":[1.5,null]})"));
	CHECK(Json::parse(text) == o);
	CHECK(Json::parse(o.dump(true)) == o);
}

TEST("json: set replaces an existing key, typed fallbacks")
{
	Json o = Json::object();
	o.set("k", 1);
	o.set("k", "two");
	CHECK_EQ(o.size(), 1u);
	CHECK_EQ(o.get("k").asString(), std::string("two"));
	CHECK_EQ(o.get("k").asInt(7), 7);
	CHECK_EQ(Json(1e12).asInt(3), 3);
	CHECK_EQ(Json("s").asString("f"), std::string("s"));
	CHECK_EQ(Json(2).asString("f"), std::string("f"));
}
