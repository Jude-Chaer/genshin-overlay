#include "item_patch.hpp"
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ItemPatch {
	static const std::string HEADER = "items patch 1\n";
	// shorter lines ("[", "]") only extend a copy, they don't start one
	static constexpr size_t MIN_LINE = 8;

	// The pack cut into lines, each file's header on its own and a file's last
	// line ending with the file, so a change in one file never reaches into the
	// next. What doesn't read as a pack is cut at its lines only.
	static std::vector<std::string_view> pieces(std::string_view all) {
		std::vector<std::string_view> out;
		auto lines = [&](std::string_view part) {
			for (size_t at = 0; at < part.size();) {
				size_t end = part.find('\n', at);
				end = end == std::string_view::npos ? part.size() : end + 1;
				out.push_back(part.substr(at, end - at));
				at = end;
			}
		};
		for (size_t at = 0; at < all.size();) {
			size_t tab = all.find('\t', at), end = all.find('\n', at);
			bool header = tab != std::string_view::npos && end != std::string_view::npos && tab < end && end - tab > 1 && end - tab <= 13;
			size_t size = 0;
			for (size_t i = tab + 1; header && i < end; i++) {
				if (all[i] < '0' || all[i] > '9') header = false;
				else size = size * 10 + (all[i] - '0');
			}
			if (!header || size > all.size() - end - 1) {
				lines(all.substr(at));
				break;
			}
			out.push_back(all.substr(at, end + 1 - at));
			lines(all.substr(end + 1, size));
			at = end + 1 + size;
		}
		return out;
	}

	std::string make(const std::string& from, const std::string& to) {
		std::string_view old(from);
		std::unordered_map<std::string_view, size_t> lines;
		for (std::string_view piece : pieces(old)) lines.emplace(piece, piece.data() - old.data());

		std::string patch = HEADER;
		size_t copyAt = 0, copyLength = 0;
		std::string added;
		auto endCopy = [&]() {
			if (copyLength > 0) patch += "c " + std::to_string(copyAt) + " " + std::to_string(copyLength) + "\n";
			copyLength = 0;
		};
		auto endAdded = [&]() {
			if (!added.empty()) patch += "a " + std::to_string(added.size()) + "\n" + added;
			added.clear();
		};
		for (std::string_view piece : pieces(to)) {
			if (copyLength > 0 && old.substr(copyAt + copyLength, piece.size()) == piece) {
				copyLength += piece.size();
				continue;
			}
			auto found = piece.size() >= MIN_LINE ? lines.find(piece) : lines.end();
			endCopy();
			if (found == lines.end()) {
				added += piece;
				continue;
			}
			endAdded();
			copyAt = found->second;
			copyLength = piece.size();
		}
		endCopy();
		endAdded();
		return patch;
	}

	// a number up to the next char, false for anything that isn't one
	static bool number(std::string_view text, size_t& at, char end, size_t& value) {
		size_t stop = text.find(end, at);
		if (stop == std::string_view::npos || stop == at || stop - at > 12) return false;
		value = 0;
		for (size_t i = at; i < stop; i++) {
			if (text[i] < '0' || text[i] > '9') return false;
			value = value * 10 + (text[i] - '0');
		}
		at = stop + 1;
		return true;
	}

	bool apply(const std::string& from, const std::string& patch, size_t maxSize, std::string& out) {
		std::string_view steps(patch);
		if (steps.substr(0, HEADER.size()) != HEADER) return false;
		out.clear();
		for (size_t at = HEADER.size(); at < steps.size();) {
			char kind = steps[at];
			if (at + 1 >= steps.size() || steps[at + 1] != ' ') return false;
			at += 2;
			if (kind == 'c') {
				size_t offset, length;
				if (!number(steps, at, ' ', offset) || !number(steps, at, '\n', length)) return false;
				if (offset > from.size() || length > from.size() - offset || length > maxSize - out.size()) return false;
				out.append(from, offset, length);
			}
			else if (kind == 'a') {
				size_t length;
				if (!number(steps, at, '\n', length)) return false;
				if (length > steps.size() - at || length > maxSize - out.size()) return false;
				out.append(steps.substr(at, length));
				at += length;
			}
			else return false;
		}
		return true;
	}
}
