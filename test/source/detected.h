#pragma once

// Detection idiom shared by the backend tests that check which calls are
// well-formed.

#include <type_traits>

namespace tcapi_test {

  template <typename Void, template <typename...> class Op, typename... Args> struct detector
      : std::false_type {};

  template <template <typename...> class Op, typename... Args>
  struct detector<std::void_t<Op<Args...>>, Op, Args...> : std::true_type {};

  // detected<Op, Args...> holds when Op<Args...> is well-formed.
  template <template <typename...> class Op, typename... Args> using detected
      = detector<void, Op, Args...>;

}  // namespace tcapi_test
