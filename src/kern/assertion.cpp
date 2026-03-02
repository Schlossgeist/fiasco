IMPLEMENTATION:

#include <cassert>
#include <cstdio>
#include <stdlib.h>

#include "ansi.h"

#include "kernel_console.h"
#include "thread.h"

extern "C"
void
assert_fail(char const *expr_msg, char const *file, unsigned int line,
            void const *caller)
{
  // Make sure that GZIP mode is off.
  //
  // We need to use the console_unchecked() method here to avoid potential
  // infinite recursion (calling assert() in the regular console() method).
  Kconsole::console_unchecked()->end_exclusive(Console::GZIP);

  printf("\nAssertion failed at %s:%u:%p: %s\n", file, line, caller, expr_msg);

  Thread::system_abort();
}

extern "C"
void
contract_assert_fail(char const *type_str, char const *expr_msg,
                     char const *file, unsigned int line, char const *func)
{
  // Make sure that GZIP mode is off.
  //
  // We need to use the console_unchecked() method here to avoid potential
  // infinite recursion (calling assert() in the regular console() method).
  Kconsole::console_unchecked()->end_exclusive(Console::GZIP);

  printf("\n"          ANSI("%s", RED, BOLD)
         " in "        ANSI("%s", BOLD)
         " at %s:%u: " ANSI("%s", CYAN, BOLD) "\n",
         type_str, func, file, line, expr_msg);

  Thread::system_abort();
}
