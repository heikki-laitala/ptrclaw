#include "tool.hpp"
#include "plugin.hpp"

namespace ptrclaw {

std::vector<std::unique_ptr<Tool>> create_builtin_tools() {
    return PluginRegistry::instance().create_all_tools();
}

} // namespace ptrclaw
