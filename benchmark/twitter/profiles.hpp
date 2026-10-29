#pragma once

#include "benchmark/synthetic/profile.hpp"

#include <optional>
#include <string_view>

namespace twitter_workload {

auto GetProfile(std::string_view code) -> std::optional<synthetic::Profile>;

}  // namespace twitter_workload
