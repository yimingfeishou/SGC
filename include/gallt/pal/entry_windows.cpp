#include "platform.hpp"

#if SGC_PLATFORM_WINDOWS

#include "../driver/entry.hpp"
#include <string>
#include <vector>

int wmain(int argc, wchar_t* argv[]) {
    gallt::pal::install_stack_guard();

    std::vector<std::wstring> arguments;

    if (argc > 0) {
        arguments.reserve(static_cast<std::size_t>(argc));
    }

    for (int index = 0; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    return gallt::sgc_main(arguments);
}

#endif
