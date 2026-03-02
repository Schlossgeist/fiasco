#pragma once

#include <features.h>
#include <cdefs.h>

__BEGIN_DECLS

void
assert_fail(char const *expr_msg, char const *file, unsigned int line,
            void const *caller)
     __attribute__ ((__noreturn__));

void
contract_assert_fail(char const *type_str, char const *expr_msg,
					 char const *file, unsigned int line, char const *func)
	 __attribute__ ((__noreturn__));

__END_DECLS

#define ASSERT_EXPECT_FALSE(exp)  __builtin_expect((exp), 0)

#ifdef NDEBUG
#define assert(expr)		do {} while (0)
#define precondition(expr)	do {} while (0)
#define invariant(expr)		do {} while (0)
#define postcondition(expr)	do {} while (0)
#define check(expr)			(void)(expr)
#else
# define assert(expr)										\
  ((void)((ASSERT_EXPECT_FALSE(!(expr)))					\
	  ? (assert_fail(#expr, __FILE__, __LINE__,         	\
                     __builtin_return_address(0)), 0)		\
	  : 0))

# define contract_assert(msg, expr)								\
  ((void)((ASSERT_EXPECT_FALSE(!(expr)))						\
	  ? (contract_assert_fail(msg, #expr, __FILE__, __LINE__,	\
							  __FUNCTION__), 0)					\
	  : 0))

#define precondition(expr)		contract_assert("Precondition failed", expr)
#define invariant(expr)		    contract_assert("Invariant failed", expr)
#define postcondition(expr)		contract_assert("Postcondition failed", expr)

# define check(expr) assert(expr)
#endif
