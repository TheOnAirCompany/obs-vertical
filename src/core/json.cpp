#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace vertical::core {

namespace {

const Json kNull;
const std::string kEmpty;
const Json::Array kEmptyArray;
const Json::Object kEmptyObject;

void escapeTo(std::string &out, const std::string &s)
{
	out += '"';
	for (unsigned char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		case '\b':
			out += "\\b";
			break;
		case '\f':
			out += "\\f";
			break;
		default:
			if (c < 0x20) {
				char buf[8];
				std::snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else {
				out += static_cast<char>(c);
			}
		}
	}
	out += '"';
}

void appendUtf8(std::string &out, uint32_t cp)
{
	if (cp < 0x80) {
		out += static_cast<char>(cp);
	} else if (cp < 0x800) {
		out += static_cast<char>(0xC0 | (cp >> 6));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		out += static_cast<char>(0xE0 | (cp >> 12));
		out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	} else {
		out += static_cast<char>(0xF0 | (cp >> 18));
		out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
		out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	}
}

class Parser {
public:
	explicit Parser(const std::string &t) : text_(t) {}

	bool run(Json &out, std::string &error)
	{
		skipWs();
		if (!value(out, 0)) {
			error = error_.empty() ? "invalid JSON" : error_;
			return false;
		}
		skipWs();
		if (pos_ != text_.size()) {
			error = "trailing characters at " + std::to_string(pos_);
			return false;
		}
		return true;
	}

private:
	bool fail(const char *what)
	{
		if (error_.empty())
			error_ = std::string(what) + " at " + std::to_string(pos_);
		return false;
	}

	void skipWs()
	{
		while (pos_ < text_.size() &&
		       (text_[pos_] == ' ' || text_[pos_] == '\n' || text_[pos_] == '\r' || text_[pos_] == '\t'))
			pos_++;
	}

	bool literal(const char *word)
	{
		const std::string w(word);
		if (text_.compare(pos_, w.size(), w) != 0)
			return fail("unexpected token");
		pos_ += w.size();
		return true;
	}

	bool hex4(uint32_t &v)
	{
		if (pos_ + 4 > text_.size())
			return fail("short unicode escape");
		v = 0;
		for (int i = 0; i < 4; i++) {
			const char c = text_[pos_++];
			v <<= 4;
			if (c >= '0' && c <= '9')
				v |= static_cast<uint32_t>(c - '0');
			else if (c >= 'a' && c <= 'f')
				v |= static_cast<uint32_t>(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F')
				v |= static_cast<uint32_t>(c - 'A' + 10);
			else
				return fail("bad unicode escape");
		}
		return true;
	}

	bool string(std::string &out)
	{
		pos_++; /* opening quote */
		while (pos_ < text_.size()) {
			const char c = text_[pos_++];
			if (c == '"')
				return true;
			if (static_cast<unsigned char>(c) < 0x20)
				return fail("control character in string");
			if (c != '\\') {
				out += c;
				continue;
			}
			if (pos_ >= text_.size())
				return fail("bad escape");
			const char e = text_[pos_++];
			switch (e) {
			case '"':
				out += '"';
				break;
			case '\\':
				out += '\\';
				break;
			case '/':
				out += '/';
				break;
			case 'b':
				out += '\b';
				break;
			case 'f':
				out += '\f';
				break;
			case 'n':
				out += '\n';
				break;
			case 'r':
				out += '\r';
				break;
			case 't':
				out += '\t';
				break;
			case 'u': {
				uint32_t cp = 0;
				if (!hex4(cp))
					return false;
				if (cp >= 0xD800 && cp <= 0xDBFF) {
					uint32_t lo = 0;
					if (pos_ + 2 > text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u')
						return fail("lone surrogate");
					pos_ += 2;
					if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF)
						return fail("bad surrogate pair");
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				}
				appendUtf8(out, cp);
				break;
			}
			default:
				return fail("bad escape");
			}
		}
		return fail("unterminated string");
	}

	bool number(Json &out)
	{
		const size_t start = pos_;
		if (text_[pos_] == '-')
			pos_++;
		while (pos_ < text_.size() &&
		       ((text_[pos_] >= '0' && text_[pos_] <= '9') || text_[pos_] == '.' || text_[pos_] == 'e' ||
			text_[pos_] == 'E' || text_[pos_] == '+' || text_[pos_] == '-'))
			pos_++;
		const std::string s = text_.substr(start, pos_ - start);
		char *end = nullptr;
		const double v = std::strtod(s.c_str(), &end);
		if (s.empty() || end != s.c_str() + s.size())
			return fail("bad number");
		out = Json(v);
		return true;
	}

	bool value(Json &out, int depth)
	{
		if (depth > 64)
			return fail("nesting too deep");
		if (pos_ >= text_.size())
			return fail("unexpected end");
		const char c = text_[pos_];
		if (c == 'n')
			return literal("null") && (out = Json(), true);
		if (c == 't')
			return literal("true") && (out = Json(true), true);
		if (c == 'f')
			return literal("false") && (out = Json(false), true);
		if (c == '"') {
			std::string s;
			if (!string(s))
				return false;
			out = Json(std::move(s));
			return true;
		}
		if (c == '-' || (c >= '0' && c <= '9'))
			return number(out);
		if (c == '[') {
			pos_++;
			out = Json::array();
			skipWs();
			if (pos_ < text_.size() && text_[pos_] == ']') {
				pos_++;
				return true;
			}
			for (;;) {
				Json item;
				skipWs();
				if (!value(item, depth + 1))
					return false;
				out.push(std::move(item));
				skipWs();
				if (pos_ < text_.size() && text_[pos_] == ',') {
					pos_++;
					continue;
				}
				if (pos_ < text_.size() && text_[pos_] == ']') {
					pos_++;
					return true;
				}
				return fail("expected , or ]");
			}
		}
		if (c == '{') {
			pos_++;
			out = Json::object();
			skipWs();
			if (pos_ < text_.size() && text_[pos_] == '}') {
				pos_++;
				return true;
			}
			for (;;) {
				skipWs();
				if (pos_ >= text_.size() || text_[pos_] != '"')
					return fail("expected key");
				std::string key;
				if (!string(key))
					return false;
				skipWs();
				if (pos_ >= text_.size() || text_[pos_] != ':')
					return fail("expected :");
				pos_++;
				skipWs();
				Json v;
				if (!value(v, depth + 1))
					return false;
				out.set(key, std::move(v));
				skipWs();
				if (pos_ < text_.size() && text_[pos_] == ',') {
					pos_++;
					continue;
				}
				if (pos_ < text_.size() && text_[pos_] == '}') {
					pos_++;
					return true;
				}
				return fail("expected , or }");
			}
		}
		return fail("unexpected character");
	}

	const std::string &text_;
	size_t pos_ = 0;
	std::string error_;
};

} // namespace

int Json::asInt(int fallback) const
{
	if (!isNumber() || !std::isfinite(num_))
		return fallback;
	if (num_ > 2147483647.0 || num_ < -2147483648.0)
		return fallback;
	return static_cast<int>(std::lround(num_));
}

const std::string &Json::asString() const
{
	return isString() ? str_ : kEmpty;
}

const Json::Array &Json::items() const
{
	return isArray() ? arr_ : kEmptyArray;
}

void Json::push(Json v)
{
	if (!isArray()) {
		*this = array();
	}
	arr_.push_back(std::move(v));
}

size_t Json::size() const
{
	if (isArray())
		return arr_.size();
	if (isObject())
		return obj_.size();
	return 0;
}

const Json &Json::get(const std::string &key) const
{
	if (!isObject())
		return kNull;
	for (const Member &m : obj_)
		if (m.first == key)
			return m.second;
	return kNull;
}

bool Json::has(const std::string &key) const
{
	if (!isObject())
		return false;
	for (const Member &m : obj_)
		if (m.first == key)
			return true;
	return false;
}

Json &Json::set(const std::string &key, Json v)
{
	if (!isObject())
		*this = object();
	for (Member &m : obj_)
		if (m.first == key) {
			m.second = std::move(v);
			return m.second;
		}
	obj_.emplace_back(key, std::move(v));
	return obj_.back().second;
}

const Json::Object &Json::members() const
{
	return isObject() ? obj_ : kEmptyObject;
}

bool Json::operator==(const Json &o) const
{
	if (type_ != o.type_)
		return false;
	switch (type_) {
	case Type::Null:
		return true;
	case Type::Bool:
		return bool_ == o.bool_;
	case Type::Number:
		return num_ == o.num_;
	case Type::String:
		return str_ == o.str_;
	case Type::Array:
		return arr_ == o.arr_;
	case Type::Object:
		return obj_ == o.obj_;
	}
	return false;
}

void Json::dumpTo(std::string &out, bool pretty, int depth) const
{
	const auto indent = [&](int d) {
		if (pretty) {
			out += '\n';
			out.append(static_cast<size_t>(d) * 2, ' ');
		}
	};
	switch (type_) {
	case Type::Null:
		out += "null";
		break;
	case Type::Bool:
		out += bool_ ? "true" : "false";
		break;
	case Type::Number: {
		if (!std::isfinite(num_)) {
			out += "null";
		} else if (num_ == std::floor(num_) && std::fabs(num_) < 1e15) {
			out += std::to_string(static_cast<long long>(num_));
		} else {
			char buf[32];
			std::snprintf(buf, sizeof(buf), "%.17g", num_);
			out += buf;
		}
		break;
	}
	case Type::String:
		escapeTo(out, str_);
		break;
	case Type::Array:
		out += '[';
		for (size_t i = 0; i < arr_.size(); i++) {
			if (i)
				out += ',';
			indent(depth + 1);
			arr_[i].dumpTo(out, pretty, depth + 1);
		}
		if (!arr_.empty())
			indent(depth);
		out += ']';
		break;
	case Type::Object:
		out += '{';
		for (size_t i = 0; i < obj_.size(); i++) {
			if (i)
				out += ',';
			indent(depth + 1);
			escapeTo(out, obj_[i].first);
			out += pretty ? ": " : ":";
			obj_[i].second.dumpTo(out, pretty, depth + 1);
		}
		if (!obj_.empty())
			indent(depth);
		out += '}';
		break;
	}
}

std::string Json::dump(bool pretty) const
{
	std::string out;
	dumpTo(out, pretty, 0);
	return out;
}

Json Json::parse(const std::string &text, std::string *error)
{
	Json out;
	std::string err;
	Parser p(text);
	if (!p.run(out, err)) {
		if (error)
			*error = err;
		return Json();
	}
	if (error)
		error->clear();
	return out;
}

} // namespace vertical::core
