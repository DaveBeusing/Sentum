#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace sentum::ui {

struct TerminalFrameDiff {
	std::string payload;
	std::vector<std::string> lines;
	std::size_t changed_rows = 0;
	bool full_redraw = false;
};

inline std::vector<std::string> split_terminal_lines(const std::string& frame) {
	std::vector<std::string> lines;
	std::size_t start = 0;
	while (start < frame.size()) {
		const auto end = frame.find('\n', start);
		if (end == std::string::npos) {
			lines.emplace_back(frame.substr(start));
			break;
		}
		lines.emplace_back(frame.substr(start, end - start));
		start = end + 1;
	}
	if (!frame.empty() && frame.back() == '\n') lines.emplace_back();
	return lines;
}

inline TerminalFrameDiff build_terminal_frame_diff(
	const std::vector<std::string>& previous_lines,
	const std::string& frame,
	bool force_full) {
	TerminalFrameDiff result;
	result.lines = split_terminal_lines(frame);
	result.full_redraw = force_full || previous_lines.empty();

	std::ostringstream terminal;
	if (result.full_redraw) {
		terminal << "\x1b[H\x1b[2J" << frame;
		result.changed_rows = std::max(previous_lines.size(), result.lines.size());
		if (result.changed_rows == 0 && !frame.empty()) result.changed_rows = 1;
	} else {
		const std::size_t rows = std::max(previous_lines.size(), result.lines.size());
		for (std::size_t row = 0; row < rows; ++row) {
			const std::string current = row < result.lines.size() ? result.lines[row] : std::string{};
			const std::string previous = row < previous_lines.size() ? previous_lines[row] : std::string{};
			if (current == previous) continue;
			terminal << "\x1b[" << (row + 1) << ";1H\x1b[2K" << current << "\x1b[0m";
			++result.changed_rows;
		}
	}

	result.payload = terminal.str();
	return result;
}

class TerminalFramePacer {
public:
	explicit TerminalFramePacer(std::chrono::steady_clock::duration interval) noexcept
		: interval_(interval) {}

	std::chrono::steady_clock::time_point next_deadline(
		std::chrono::steady_clock::time_point now) noexcept {
		if (!initialized_) {
			next_ = now + interval_;
			initialized_ = true;
			return next_;
		}

		next_ += interval_;
		if (next_ <= now) next_ = now + interval_;
		return next_;
	}

	void reset() noexcept {
		initialized_ = false;
		next_ = {};
	}

private:
	std::chrono::steady_clock::duration interval_;
	std::chrono::steady_clock::time_point next_{};
	bool initialized_ = false;
};

} // namespace sentum::ui
