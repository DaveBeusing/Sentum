#pragma once

#include <string>
#include <sentum/trader/types/RiskConfig.hpp>

void validate_stop_loss_config(const RiskConfig& config);
RiskConfig load_risk_config(const std::string& path);
