#include <doctest/doctest.h>
#include <tcapi/tcapi.h>

#include <complex>
#include <type_traits>

// A tcapi context argument brings namespace tcapi into argument-dependent lookup. For each public
// tcapi function template deducing its tensor type from an argument, these tests call a caller's
// own function of the same name with a type that has no tensor_traits specialization, and check
// at compile time that the call selects the caller's function. The caller's function takes every
// argument by const reference, so a tcapi template that stayed viable would bind the context at
// least as well and, being more specialized than a parameter pack, would win the call instead.

// The public tcapi functions whose tensor type is deduced from a tensor argument, and those
// deducing it from a List<CRef<TenT>> argument. A new public function of either kind joins its
// list.
#define ADL_TENSOR_ARG_NAMES(X) \
  X(order)                      \
  X(shape)                      \
  X(size)                       \
  X(size_bytes)                 \
  X(get_elem)                   \
  X(copy)                       \
  X(move)                       \
  X(clear)                      \
  X(set_elem)                   \
  X(reshape)                    \
  X(transpose)                  \
  X(cplx_conj)                  \
  X(to_cplx)                    \
  X(real)                       \
  X(imag)                       \
  X(for_each)                   \
  X(expand)                     \
  X(shrink)                     \
  X(extract_sub)                \
  X(replace_sub)                \
  X(for_each_with_coors)        \
  X(to_range)                   \
  X(show)                       \
  X(close)                      \
  X(convert)                    \
  X(save)                       \
  X(norm)                       \
  X(normalize)                  \
  X(scale)                      \
  X(diag)                       \
  X(trace)                      \
  X(contract)                   \
  X(svd)                        \
  X(qr)                         \
  X(lq)                         \
  X(trunc_svd)                  \
  X(eigvals)                    \
  X(eig)                        \
  X(eigvalsh)                   \
  X(eigh)                       \
  X(inverse)                    \
  X(exp)

#define ADL_LIST_ARG_NAMES(X) \
  X(concatenate)              \
  X(stack)                    \
  X(linear_combine)

// Stands in for user code: one catch-all per name, returning Hit, a type no tcapi function
// returns.
namespace adl_user {

  struct Chain {};

  struct Hit {};

#define ADL_USER_DEFINE(fn) \
  template <typename... Args> Hit fn(const Args&...) { return Hit{}; }
  ADL_TENSOR_ARG_NAMES(ADL_USER_DEFINE)
  ADL_LIST_ARG_NAMES(ADL_USER_DEFINE)
#undef ADL_USER_DEFINE

}  // namespace adl_user

// Calls fn unqualified with ctx, first, and then 0 to 9 further Chain arguments. These tcapi
// templates take 2 to 11 parameters, the most being trunc_svd's 11, so each of them is reached at
// its own arity and deduces its tensor type as the user type.
#define ADL_SELECTS_USER(...) std::is_same_v<decltype(__VA_ARGS__), adl_user::Hit>
#define ADL_CHECK_SELECTED(fn, first)                                      \
  static_assert(ADL_SELECTS_USER(fn(ctx, first)));                         \
  static_assert(ADL_SELECTS_USER(fn(ctx, first, c)));                      \
  static_assert(ADL_SELECTS_USER(fn(ctx, first, c, c)));                   \
  static_assert(ADL_SELECTS_USER(fn(ctx, first, c, c, c)));                \
  static_assert(ADL_SELECTS_USER(fn(ctx, first, c, c, c, c)));             \
  static_assert(ADL_SELECTS_USER(fn(ctx, first, c, c, c, c, c)));          \
  static_assert(ADL_SELECTS_USER(fn(ctx, first, c, c, c, c, c, c)));       \
  static_assert(ADL_SELECTS_USER(fn(ctx, first, c, c, c, c, c, c, c)));    \
  static_assert(ADL_SELECTS_USER(fn(ctx, first, c, c, c, c, c, c, c, c))); \
  static_assert(ADL_SELECTS_USER(fn(ctx, first, c, c, c, c, c, c, c, c, c)));

#define ADL_CHECK_TENSOR_ARG(fn) ADL_CHECK_SELECTED(fn, c)
#define ADL_CHECK_LIST_ARG(fn) ADL_CHECK_SELECTED(fn, list)

TEST_CASE("ADL - a caller's function named like a tcapi function is selected for its own type") {
  tcapi::context_handle_t<tcapi::CytnxTensor<std::complex<double>>> ctx;
  adl_user::Chain c;
  tcapi::List<tcapi::CRef<adl_user::Chain>> list;

  ADL_TENSOR_ARG_NAMES(ADL_CHECK_TENSOR_ARG)
  ADL_LIST_ARG_NAMES(ADL_CHECK_LIST_ARG)
}

#undef ADL_CHECK_LIST_ARG
#undef ADL_CHECK_TENSOR_ARG
#undef ADL_CHECK_SELECTED
#undef ADL_SELECTS_USER
#undef ADL_LIST_ARG_NAMES
#undef ADL_TENSOR_ARG_NAMES
