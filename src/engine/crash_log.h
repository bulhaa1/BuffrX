// A crash report that says where.
//
// The engine died once while the graphics backend was working, and the log said nothing useful.
// This records what happened: a vectored exception handler notes the exception, writes the report
// where the C runtime is safe to use, and the tick repeats it in the ordinary log.
//
// The handler does the least it can and writes the report with nothing but the operating system.
// It must not format anything with the C runtime, take a lock, or call into it: libobs's graphics
// backend raises exceptions of its own while it works, and a handler that uses the C runtime while
// the thread that raised the exception is already holding its lock turns a handled exception into a
// dead process. That is not a theory - it is what happened, and the engine died at every startup
// until the handler was made this small.
#pragma once

namespace bx {

// Installs the handler. Safe to call once, early: it does nothing if the log is not open yet.
void install_crash_handler();

// Writes down the last exception, if there was one. Called from the tick, not from the handler.
void report_fault();

}  // namespace bx
