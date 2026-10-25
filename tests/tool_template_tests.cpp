#include "runtimes/ToolTemplate.h"
#include <stdexcept>
#include <iostream>
int main(){try{
    const std::string malformed=R"(prefix <tool_call>\n{{\"name\": <function-name>, \"arguments\": <args-json-object>}}\n</tool_call> suffix)";
    const std::string expected=R"(prefix <tool_call>\n{\"name\": <function-name>, \"arguments\": <args-json-object>}\n</tool_call> suffix)";
    const auto fixed=iiLocalLLM::detail::normalizedToolTemplate(malformed);
    if(fixed!=expected)throw std::runtime_error("Doubled tool example braces were not repaired");
    if(iiLocalLLM::detail::normalizedToolTemplate(fixed)!=fixed)throw std::runtime_error("Template repair was not idempotent");
    const std::string ordinary=R"({{ message.content }} {"name":"Read","arguments":{"path":"x"}})";
    if(iiLocalLLM::detail::normalizedToolTemplate(ordinary)!=ordinary)throw std::runtime_error("Valid Jinja or tool JSON changed");
    std::cout<<"TOOL_TEMPLATE_CONTRACT_OK\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
