// Minimal `key = value` configuration text format (docs/phase3-design.md §1).
//
//   # comment
//   section.key = value   # trailing comment
//
// Keys and values are trimmed. A line without '=' or with an empty key, and a
// key that appears twice in one file, are errors. Interpreting the values
// (types, allowed keys) is the caller's job.
#pragma once

#include <map>
#include <stdexcept>
#include <string>

namespace rvx {

struct ConfigError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Ordered map so iteration (and therefore error order) is deterministic.
using KeyValues = std::map<std::string, std::string>;

// `origin` (e.g. a file name) is used in error messages.
KeyValues parse_kv(const std::string& text, const std::string& origin);

// Parses a single "key=value" override as given on the command line.
std::pair<std::string, std::string> parse_override(const std::string& s);

}  // namespace rvx
