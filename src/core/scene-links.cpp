#include "scene-links.hpp"

#include <algorithm>

namespace vertical::core {

bool SceneLinks::link(const std::string &mainScene, const std::string &verticalScene)
{
	if (mainScene.empty() || verticalScene.empty())
		return false;
	for (auto &l : links_)
		if (l.first == mainScene) {
			if (l.second == verticalScene)
				return false;
			l.second = verticalScene;
			return true;
		}
	links_.emplace_back(mainScene, verticalScene);
	return true;
}

bool SceneLinks::unlink(const std::string &mainScene)
{
	const auto before = links_.size();
	links_.erase(std::remove_if(links_.begin(), links_.end(), [&](const auto &l) { return l.first == mainScene; }),
		     links_.end());
	return links_.size() != before;
}

std::optional<std::string> SceneLinks::verticalFor(const std::string &mainScene) const
{
	for (const auto &l : links_)
		if (l.first == mainScene)
			return l.second;
	return std::nullopt;
}

std::vector<std::string> SceneLinks::mainScenesFor(const std::string &verticalScene) const
{
	std::vector<std::string> out;
	for (const auto &l : links_)
		if (l.second == verticalScene)
			out.push_back(l.first);
	return out;
}

void SceneLinks::renameMain(const std::string &from, const std::string &to)
{
	if (from == to || to.empty())
		return;
	/* A link already on the new name would be a duplicate: the renamed one wins. */
	if (std::any_of(links_.begin(), links_.end(), [&](const auto &l) { return l.first == from; }))
		unlink(to);
	for (auto &l : links_)
		if (l.first == from)
			l.first = to;
}

void SceneLinks::renameVertical(const std::string &from, const std::string &to)
{
	if (from == to || to.empty())
		return;
	for (auto &l : links_)
		if (l.second == from)
			l.second = to;
}

void SceneLinks::removeMain(const std::string &name)
{
	unlink(name);
}

void SceneLinks::removeVertical(const std::string &name)
{
	links_.erase(std::remove_if(links_.begin(), links_.end(), [&](const auto &l) { return l.second == name; }),
		     links_.end());
}

Json SceneLinks::toJson() const
{
	Json arr = Json::array();
	for (const auto &l : links_) {
		Json o = Json::object();
		o.set("main", l.first);
		o.set("vertical", l.second);
		arr.push(std::move(o));
	}
	return arr;
}

SceneLinks SceneLinks::fromJson(const Json &j)
{
	SceneLinks s;
	for (const Json &item : j.items())
		s.link(item.get("main").asString(), item.get("vertical").asString());
	return s;
}

SwitchDecision decideSwitch(const SceneLinks &links, bool linkedMode, const std::string &mainScene,
			    const std::string &currentVertical, const std::vector<std::string> &verticalScenes)
{
	SwitchDecision d;
	if (!linkedMode)
		return d;
	const auto target = links.verticalFor(mainScene);
	if (!target || *target == currentVertical)
		return d;
	if (std::find(verticalScenes.begin(), verticalScenes.end(), *target) == verticalScenes.end())
		return d;
	d.doSwitch = true;
	d.target = *target;
	return d;
}

std::vector<std::string> applyOrder(const std::vector<std::string> &existing, const std::vector<std::string> &saved)
{
	std::vector<std::string> out;
	const auto has = [](const std::vector<std::string> &v, const std::string &s) {
		return std::find(v.begin(), v.end(), s) != v.end();
	};
	for (const std::string &s : saved)
		if (has(existing, s) && !has(out, s))
			out.push_back(s);
	for (const std::string &s : existing)
		if (!has(out, s))
			out.push_back(s);
	return out;
}

bool moveItem(std::vector<std::string> &order, int from, int to)
{
	const int n = (int)order.size();
	if (from < 0 || from >= n)
		return false;
	to = std::clamp(to, 0, n - 1);
	if (from == to)
		return false;
	std::string item = order[(size_t)from];
	order.erase(order.begin() + from);
	order.insert(order.begin() + to, std::move(item));
	return true;
}

std::string uniqueName(const std::string &base, const std::vector<std::string> &taken)
{
	const auto has = [&](const std::string &s) {
		return std::find(taken.begin(), taken.end(), s) != taken.end();
	};
	if (!has(base))
		return base;
	for (int i = 2; i < 10000; i++) {
		std::string candidate = base + " " + std::to_string(i);
		if (!has(candidate))
			return candidate;
	}
	return base + " *";
}

} // namespace vertical::core
