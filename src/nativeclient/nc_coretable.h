// Checks that the opened core library has every contract function (Linux also binds them; Windows reaches them through delay-load). See gen-core-table.cjs.
#pragma once
#include <string>
namespace nc {
bool bindCoreTable(void* library, std::string& missing, bool bind = true);
}
