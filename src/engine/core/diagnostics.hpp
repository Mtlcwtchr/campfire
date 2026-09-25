#pragma once

// Deliberately expands in the caller's scope so diagnostic declarations can
// accompany diagnostic statements. Use braces around conditional call sites.
// In OFF builds even ill-formed diagnostic-only expressions are discarded.
#if ASR_ENABLE_DIAGNOSTICS
#define ASR_DIAGNOSTIC(...) __VA_ARGS__
#else
#define ASR_DIAGNOSTIC(...) ((void)0)
#endif
