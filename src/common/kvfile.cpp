#include "common/kvfile.h"

#include <sstream>

namespace rvx {
namespace {

std::string trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r");
  if (b == std::string::npos) return "";
  const auto e = s.find_last_not_of(" \t\r");
  return s.substr(b, e - b + 1);
}

}  // namespace

KeyValues parse_kv(const std::string& text, const std::string& origin) {
  KeyValues kv;
  std::istringstream in(text);
  std::string raw;
  int line_no = 0;
  while (std::getline(in, raw)) {
    ++line_no;
    const auto hash = raw.find('#');
    const std::string line = trim(hash == std::string::npos ? raw : raw.substr(0, hash));
    if (line.empty()) continue;
    const auto eq = line.find('=');
    const std::string where = origin + ":" + std::to_string(line_no);
    if (eq == std::string::npos) throw ConfigError(where + ": expected `key = value`");
    const std::string key = trim(line.substr(0, eq));
    const std::string value = trim(line.substr(eq + 1));
    if (key.empty()) throw ConfigError(where + ": empty key");
    if (!kv.emplace(key, value).second) throw ConfigError(where + ": duplicate key `" + key + "`");
  }
  return kv;
}

std::pair<std::string, std::string> parse_override(const std::string& s) {
  const auto eq = s.find('=');
  if (eq == std::string::npos) throw ConfigError("override `" + s + "` is not key=value");
  const std::string key = trim(s.substr(0, eq));
  if (key.empty()) throw ConfigError("override `" + s + "` has an empty key");
  return {key, trim(s.substr(eq + 1))};
}

}  // namespace rvx
