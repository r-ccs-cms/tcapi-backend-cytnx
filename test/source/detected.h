#pragma once

// Detection helper for tests that check whether an expression is well-formed.

#include <type_traits>

template <typename Void, template <typename...> class Op, typename... Args> struct detector
    : std::false_type {};

template <template <typename...> class Op, typename... Args>
struct detector<std::void_t<Op<Args...>>, Op, Args...> : std::true_type {};

// detected<Op, Args...> holds when Op<Args...> is well-formed.
template <template <typename...> class Op, typename... Args> using detected
    = detector<void, Op, Args...>;
