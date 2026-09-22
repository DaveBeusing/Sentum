#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace sentum::cli {

enum class Mode { Paper, Replay, Research, Testnet, Dashboard, Incident, Help, Version };

struct Options {
    Mode mode = Mode::Paper;
    std::string input;
    std::string symbol;
    std::string incident_command;
    std::string target_id;
    std::string actor;
    std::string reason;
    std::string evidence_id;
    bool tui = true;
    std::optional<std::uint16_t> dashboard_port;
};

inline std::uint16_t parse_port(const std::string& value) {
    const int parsed = std::stoi(value);
    if (parsed < 1 || parsed > 65535) throw std::runtime_error("dashboard port must be between 1 and 65535");
    return static_cast<std::uint16_t>(parsed);
}

inline Options parse(int argc, char** argv) {
    Options out;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    if (args.empty()) return out;

    std::vector<std::string> positional;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--no-tui") { out.tui = false; continue; }
        if (arg == "--tui") { out.tui = true; continue; }
        if (arg == "--dashboard-port") {
            if (++i >= args.size()) throw std::runtime_error("--dashboard-port requires a value");
            out.dashboard_port = parse_port(args[i]);
            continue;
        }
        positional.push_back(arg);
    }
    if (positional.empty()) return out;

    const std::string command = positional.front();
    if (command == "--help" || command == "-h" || command == "help") { out.mode = Mode::Help; return out; }
    if (command == "--version" || command == "-V" || command == "version") { out.mode = Mode::Version; return out; }

    auto require = [&](std::size_t count, const char* usage) {
        if (positional.size() != count + 1) throw std::runtime_error(std::string("invalid arguments; expected: ") + usage);
    };

    if (command == "paper" || command == "--paper") { require(0, "sentum paper"); out.mode = Mode::Paper; return out; }
    if (command == "dashboard" || command == "--dashboard") { require(0, "sentum dashboard"); out.mode = Mode::Dashboard; out.tui = false; return out; }
    if (command == "incident") {
        if (positional.size() < 2) throw std::runtime_error("invalid arguments; expected: sentum incident <status|approve|deny|acknowledge|recover|resolve|close> ...");
        out.mode = Mode::Incident;
        out.tui = false;
        out.incident_command = positional[1];
        if (out.incident_command == "status") {
            if (positional.size() != 2) throw std::runtime_error("invalid arguments; expected: sentum incident status");
            return out;
        }
        if (out.incident_command == "recover") {
            if (positional.size() != 6) throw std::runtime_error("invalid arguments; expected: sentum incident recover <incident-id> <actor> <reconciliation-evidence-id> <reason>");
            out.target_id = positional[2];
            out.actor = positional[3];
            out.evidence_id = positional[4];
            out.reason = positional[5];
            return out;
        }
        if (out.incident_command == "approve" || out.incident_command == "deny" ||
            out.incident_command == "acknowledge" || out.incident_command == "resolve" ||
            out.incident_command == "close") {
            if (positional.size() != 5) throw std::runtime_error(
                "invalid arguments; expected: sentum incident <approve|deny|acknowledge|resolve|close> <request-or-incident-id> <actor> <reason>");
            out.target_id = positional[2];
            out.actor = positional[3];
            out.reason = positional[4];
            return out;
        }
        throw std::runtime_error("unknown incident command: " + out.incident_command);
    }
    if (command == "testnet" || command == "--testnet") { require(1, "sentum testnet <symbol>"); out.mode = Mode::Testnet; out.symbol = positional[1]; return out; }
    if (command == "research" || command == "--research") { require(1, "sentum research <config.json>"); out.mode = Mode::Research; out.input = positional[1]; out.tui = false; return out; }
    if (command == "replay" || command == "--replay") { require(2, "sentum replay <events.csv> <symbol>"); out.mode = Mode::Replay; out.input = positional[1]; out.symbol = positional[2]; out.tui = false; return out; }

    throw std::runtime_error("unknown command: " + command);
}

inline const char* usage() {
    return
        "Sentum unified CLI\n\n"
        "Usage:\n"
        "  sentum paper [--no-tui] [--dashboard-port PORT]\n"
        "  sentum testnet <symbol> [--no-tui] [--dashboard-port PORT]\n"
        "  sentum replay <events.csv> <symbol>\n"
        "  sentum research <research.json>\n"
        "  sentum dashboard [--dashboard-port PORT]\n"
        "  sentum incident status\n"
        "  sentum incident approve <request-id> <actor> <reason>\n"
        "  sentum incident deny <request-id> <actor> <reason>\n"
        "  sentum incident acknowledge <incident-id> <actor> <reason>\n"
        "  sentum incident recover <incident-id> <actor> <reconciliation-evidence-id> <reason>\n"
        "  sentum incident resolve <incident-id> <actor> <reason>\n"
        "  sentum incident close <incident-id> <actor> <reason>\n"
        "  sentum version\n"
        "  sentum help\n\n"
        "Legacy --paper/--testnet/--replay/--research/--dashboard forms remain supported.\n"
        "SENTUM_DASHBOARD_PORT is used when --dashboard-port is omitted.\n";
}

} // namespace sentum::cli
