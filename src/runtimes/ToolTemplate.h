#pragma once
#include <string>
#include <string_view>

namespace iiLocalLLM::detail {
// Some GGUF templates escaped a format-string example twice before embedding
// it in a Jinja string. Repair only that documented placeholder example, never
// arbitrary braces, runtime messages, or a host-selected template.
inline std::string normalizedToolTemplate(std::string value) {
    constexpr std::string_view malformed=R"({{\"name\": <function-name>, \"arguments\": <args-json-object>}})";
    constexpr std::string_view corrected=R"({\"name\": <function-name>, \"arguments\": <args-json-object>})";
    for(auto at=value.find(malformed);at!=std::string::npos;at=value.find(malformed,at+corrected.size()))
        value.replace(at,malformed.size(),corrected);
    return value;
}
}
