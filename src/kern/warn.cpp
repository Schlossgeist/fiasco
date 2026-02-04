INTERFACE:

#include <cstdio>

#include "ansi.h"

#include "config.h"

enum class Warn_level : int
{
  Error   = 0,
  Warning = 1,
  Info    = 2,
};

namespace Warn
{

static char const *Warn_labels[] =
{
  ANSI("Error", RED),
  ANSI("Warning", YELLOW),
  ANSI("Info", CYAN),
};

constexpr bool is_enabled(Warn_level level)
{
  (void) Warn_labels;   // silence "unused variable" warning
  return level <= Warn_level{Config::Warn_level};
}

}

#define WARNX(level, ...)                                                   \
  do {                                                                      \
       if constexpr (Warn::is_enabled(Warn_level::level))                   \
         {                                                                  \
           printf("\n" ANSI("KERNEL", RED, BOLD) ": %s\n",                  \
                  Warn::Warn_labels[static_cast<int>(Warn_level::level)]);  \
           printf("\t" __VA_ARGS__);	                                    \
           printf("\n");	                                                \
         }                                                                  \
     } while (0)

#define ERROR(...)  WARNX(Error, __VA_ARGS__)
#define WARN(...)   WARNX(Warning, __VA_ARGS__)
#define INFO(...)   WARNX(Info, __VA_ARGS__)
