#include "tool.hpp"
#include "tools/shell.hpp"
#include "tools/file_read.hpp"
#include "tools/file_write.hpp"
#include "tools/file_edit.hpp"

namespace ptrclaw {

std::vector<std::unique_ptr<Tool>> create_builtin_tools() {
    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<ShellTool>());
    tools.push_back(std::make_unique<FileReadTool>());
    tools.push_back(std::make_unique<FileWriteTool>());
    tools.push_back(std::make_unique<FileEditTool>());
    return tools;
}

} // namespace ptrclaw
