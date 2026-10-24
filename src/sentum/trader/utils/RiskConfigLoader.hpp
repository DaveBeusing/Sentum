#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include <sentum/trader/types/RiskConfig.hpp>

void validate_stop_loss_config(const RiskConfig& config);
void validate_risk_config(const RiskConfig& config);
RiskConfig parse_risk_config(const nlohmann::json& configuration);
RiskConfig load_risk_config(const std::string& path);
