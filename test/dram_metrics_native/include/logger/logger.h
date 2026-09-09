// Test-only logger replacement for the CPU-only client state-machine executable.
#pragma once
#define UC_DEBUG(...) ((void)0)
#define UC_INFO(...) ((void)0)
#define UC_WARN(...) ((void)0)
#define UC_ERROR(...) ((void)0)
#define UC_ERROR_UNLIMITED(...) ((void)0)
