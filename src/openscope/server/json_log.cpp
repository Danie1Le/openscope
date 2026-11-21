#include "openscope/server/json_log.hpp"

#include <chrono>
#include <format>
#include <nlohmann/json.hpp>
#include <ostream>
#include <string_view>

namespace openscope {

void JsonLog::write(std::string_view event, const nlohmann::ordered_json& fields) {
  const auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
  nlohmann::ordered_json line = {{"time", std::format("{:%FT%TZ}", now)}, {"event", event}};
  line.update(fields);
  *out_ << line.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace) << std::endl;
}

}  // namespace openscope
