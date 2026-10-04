#include "platform.hpp"

#if SGC_PLATFORM_LINUX

#include "../driver/entry.hpp"
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    gallt::pal::install_stack_guard();

    std::vector<std::wstring> arguments;

    if (argc > 0) {
        arguments.reserve(static_cast<std::size_t>(argc));
    }

    for (int index = 0; index < argc; ++index) {
        arguments.emplace_back(gallt::pal::to_wide(argv[index]));
    }

    return gallt::sgc_main(arguments);
}

#endif
