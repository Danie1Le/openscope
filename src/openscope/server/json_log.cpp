#include "openscope/server/json_log.hpp"

#include <chrono>
#include <format>
#include <nlohmann/json.hpp>  // IWYU pragma: keep (the definition of ordered_json)
#include <nlohmann/json_fwd.hpp>
#include <ostream>
#include <string_view>

namespace openscope {

void JsonLog::write(std::string_view event, const nlohmann::ordered_json& fields) {
  // Truncation is the floor for any time after 1970.
  const auto now =
      std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
  nlohmann::ordered_json line = {{"time", std::format("{:%FT%TZ}", now)}, {"event", event}};
  line.update(fields);
  *out_ << line.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace) << '\n';
  out_->flush();
}

}  // namespace openscope
