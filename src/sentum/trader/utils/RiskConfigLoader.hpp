#pragma once

#include <string>
#include <sentum/trader/types/RiskConfig.hpp>

RiskConfig load_risk_config(const std::string& path);
