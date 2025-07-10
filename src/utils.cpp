#include "../include/config.hpp"
#include <fstream>
#include <sstream>
#include <map>
#include <iostream>
#include <algorithm>
#include <cctype>

std::map<std::string, std::string> load_secrets(const std::string& path = "secrets.conf") {
    std::ifstream file(path);
    std::map<std::string, std::string> secrets;
    std::string line;
    bool in_binance_section = false;

    while (std::getline(file, line)) {
        // Entferne Leerzeichen
        line.erase(remove_if(line.begin(), line.end(), isspace), line.end());

        // Kommentar oder leer?
        if (line.empty() || line[0] == '#') continue;

        // Sektion erkennen
        if (line == "[binance]") {
            in_binance_section = true;
            continue;
        }

        if (in_binance_section) {
            size_t delim_pos = line.find('=');
            if (delim_pos != std::string::npos) {
                std::string key = line.substr(0, delim_pos);
                std::string val = line.substr(delim_pos + 1);
                secrets[key] = val;
            }
        }
    }

    return secrets;
}

std::string get_api_key() {
    static auto secrets = load_secrets();
    return secrets["api_key"];
}

std::string get_secret_key() {
    static auto secrets = load_secrets();
    return secrets["secret_key"];
}
