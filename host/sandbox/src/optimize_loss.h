#pragma once

#include <limits>
#include <map>
#include <string>

#include "supple/analysis/analysis.h"

namespace supple::sandbox::detail {

struct Loss {
    double total = std::numeric_limits<double>::infinity();
    std::map<std::string, double> terms;
};

void term(Loss& loss, const std::string& name, double distance, double weight, double scale);
Loss referenceLoss(const analysis::Report& reference, const analysis::Report& candidate);

}  // namespace supple::sandbox::detail
