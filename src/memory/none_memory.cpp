#include "none_memory.hpp"
#include "../plugin.hpp"

static ptrclaw::MemoryRegistrar reg_none("none",
    [](const ptrclaw::Config&) { return std::make_unique<ptrclaw::NoneMemory>(); });
