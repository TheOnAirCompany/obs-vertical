#pragma once

/* Minimal JSON value, parser and writer for the settings of the plugin.
 * Pure C++17: no OBS, no Qt, no third-party code. Objects keep insertion
 * order so saved files stay stable and diff-friendly. */

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace vertical::core {

class Json {
public:
	enum class Type { Null, Bool, Number, String, Array, Object };

	using Array = std::vector<Json>;
	using Member = std::pair<std::string, Json>;
	using Object = std::vector<Member>;

	Json() = default;
	Json(std::nullptr_t) {}
	Json(bool b) : type_(Type::Bool), bool_(b) {}
	Json(int n) : type_(Type::Number), num_(n) {}
	Json(int64_t n) : type_(Type::Number), num_(static_cast<double>(n)) {}
	Json(double n) : type_(Type::Number), num_(n) {}
	Json(const char *s) : type_(Type::String), str_(s ? s : "") {}
	Json(std::string s) : type_(Type::String), str_(std::move(s)) {}

	static Json array()
	{
		Json j;
		j.type_ = Type::Array;
		return j;
	}
	static Json object()
	{
		Json j;
		j.type_ = Type::Object;
		return j;
	}

	Type type() const { return type_; }
	bool isNull() const { return type_ == Type::Null; }
	bool isBool() const { return type_ == Type::Bool; }
	bool isNumber() const { return type_ == Type::Number; }
	bool isString() const { return type_ == Type::String; }
	bool isArray() const { return type_ == Type::Array; }
	bool isObject() const { return type_ == Type::Object; }

	/* Typed reads with a fallback when the value has another type. */
	bool asBool(bool fallback = false) const { return isBool() ? bool_ : fallback; }
	double asNumber(double fallback = 0.0) const { return isNumber() ? num_ : fallback; }
	int asInt(int fallback = 0) const;
	const std::string &asString() const;
	std::string asString(const std::string &fallback) const { return isString() ? str_ : fallback; }

	/* Arrays. */
	const Array &items() const;
	void push(Json v);
	size_t size() const;

	/* Objects. get() returns a shared null value when the key is missing. */
	const Json &get(const std::string &key) const;
	bool has(const std::string &key) const;
	Json &set(const std::string &key, Json v);
	const Object &members() const;

	std::string dump(bool pretty = false) const;

	/* Parses text. On error returns a null value and fills *error (if given). */
	static Json parse(const std::string &text, std::string *error = nullptr);

	bool operator==(const Json &o) const;
	bool operator!=(const Json &o) const { return !(*this == o); }

private:
	void dumpTo(std::string &out, bool pretty, int depth) const;

	Type type_ = Type::Null;
	bool bool_ = false;
	double num_ = 0.0;
	std::string str_;
	Array arr_;
	Object obj_;
};

} // namespace vertical::core
