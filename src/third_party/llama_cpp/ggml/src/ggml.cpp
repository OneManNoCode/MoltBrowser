#include "ggml-impl.h"

#include <cstdlib>
#include <exception>

// MoltBrowser: no process-wide terminate handler. It would be a static
// initializer in the browser binary that replaces Chromium's handler for every
// std::terminate, and it runs ggml_print_backtrace (see ggml.c).
#if !defined(GGML_MOLTBROWSER_NO_BACKTRACE)
static std::terminate_handler previous_terminate_handler;

GGML_NORETURN static void ggml_uncaught_exception() {
    ggml_print_backtrace();
    if (previous_terminate_handler) {
        previous_terminate_handler();
    }
    abort(); // unreachable unless previous_terminate_handler was nullptr
}

static bool ggml_uncaught_exception_init = []{
    const char * GGML_NO_BACKTRACE = getenv("GGML_NO_BACKTRACE");
    if (GGML_NO_BACKTRACE) {
        return false;
    }
    const auto prev{std::get_terminate()};
    GGML_ASSERT(prev != ggml_uncaught_exception);
    previous_terminate_handler = prev;
    std::set_terminate(ggml_uncaught_exception);
    return true;
}();
#endif  // !defined(GGML_MOLTBROWSER_NO_BACKTRACE)
