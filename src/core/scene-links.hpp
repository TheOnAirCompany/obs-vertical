#pragma once

/* "Linked scenes": main (16:9) scene name -> vertical scene name, plus the
 * order of the vertical scenes in the dock. Pure C++17. */

#include "json.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vertical::core {

class SceneLinks {
public:
	/* Links a main scene to a vertical scene (replaces an existing link of
	 * that main scene). Empty names are ignored. Returns true when changed. */
	bool link(const std::string &mainScene, const std::string &verticalScene);
	bool unlink(const std::string &mainScene);

	std::optional<std::string> verticalFor(const std::string &mainScene) const;
	std::vector<std::string> mainScenesFor(const std::string &verticalScene) const;

	/* Keep links valid when scenes are renamed or removed. */
	void renameMain(const std::string &from, const std::string &to);
	void renameVertical(const std::string &from, const std::string &to);
	void removeMain(const std::string &name);
	void removeVertical(const std::string &name);

	/* Pairs (main, vertical) in insertion order. */
	const std::vector<std::pair<std::string, std::string>> &entries() const { return links_; }
	size_t size() const { return links_.size(); }
	bool empty() const { return links_.empty(); }
	void clear() { links_.clear(); }

	Json toJson() const;
	static SceneLinks fromJson(const Json &j);

	bool operator==(const SceneLinks &o) const { return links_ == o.links_; }

private:
	std::vector<std::pair<std::string, std::string>> links_;
};

/* What to do when the main program scene changes. */
struct SwitchDecision {
	bool doSwitch = false;
	std::string target;
};

/* Switches only in linked mode, to an existing vertical scene that is not
 * already on air. Without a link the vertical output keeps its scene. */
SwitchDecision decideSwitch(const SceneLinks &links, bool linkedMode, const std::string &mainScene,
			    const std::string &currentVertical, const std::vector<std::string> &verticalScenes);

/* Dock order: names of `saved` that still exist, in saved order, then the
 * remaining names of `existing` in their given order. */
std::vector<std::string> applyOrder(const std::vector<std::string> &existing, const std::vector<std::string> &saved);

/* Moves the item at `from` to `to` (clamped). Returns false when nothing moved. */
bool moveItem(std::vector<std::string> &order, int from, int to);

/* A free scene name: base, "base 2", "base 3", ... not in `taken`. */
std::string uniqueName(const std::string &base, const std::vector<std::string> &taken);

} // namespace vertical::core
