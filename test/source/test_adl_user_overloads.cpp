#include <doctest/doctest.h>
#include <tcapi/tcapi.h>

#include <complex>
#include <string_view>
#include <type_traits>
#include <utility>

// A tcapi context argument brings namespace tcapi into argument-dependent lookup. For each public
// tcapi function template over a tensor type TenT deduced from an argument, these tests call a
// caller's own function of the same name with a type that has no tensor_traits specialization, and
// check at compile time that the call selects the caller's function. The caller's function takes
// every argument by const reference, so a tcapi template that stayed viable would bind the context
// at least as well and, being more specialized than a parameter pack, would win the call instead.
// The same check covers create_context and destroy_context, templates over a handle type, called
// with a type that brings tcapi into the lookup without being CytnxContextHandle.

// The public tcapi function templates over a tensor type TenT deduced from a TenT argument, and
// those deducing it from a List<CRef<TenT>> argument. A new public function of either kind joins
// its list.
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

// The public tcapi function templates over a context handle type deduced from a handle argument.
#define ADL_HANDLE_ARG_NAMES(X) \
  X(create_context)             \
  X(destroy_context)

// Every listed name must be a tcapi function, or the checks below would pass with no tcapi
// candidate to discard; a using-declaration of a name tcapi does not declare fails to compile.
namespace adl_tcapi_names {
#define ADL_TCAPI_DECLARES(fn) using tcapi::fn;
  ADL_TENSOR_ARG_NAMES(ADL_TCAPI_DECLARES)
  ADL_LIST_ARG_NAMES(ADL_TCAPI_DECLARES)
  ADL_HANDLE_ARG_NAMES(ADL_TCAPI_DECLARES)
#undef ADL_TCAPI_DECLARES
}  // namespace adl_tcapi_names

// Stands in for user code: one catch-all per name, returning Hit, a type no tcapi function
// returns.
namespace adl_user {

  struct Chain {};

  struct Hit {};

  // Each brings tcapi into argument-dependent lookup without being CytnxContextHandle: Session
  // through its template argument, DerivedHandle through its base class.
  template <typename H> struct Session {
    H h;
  };

  struct DerivedHandle : tcapi::CytnxContextHandle {};

#define ADL_USER_DEFINE(fn) \
  template <typename... Args> Hit fn(const Args&...) { return Hit{}; }
  ADL_TENSOR_ARG_NAMES(ADL_USER_DEFINE)
  ADL_LIST_ARG_NAMES(ADL_USER_DEFINE)
  ADL_HANDLE_ARG_NAMES(ADL_USER_DEFINE)
#undef ADL_USER_DEFINE

}  // namespace adl_user

// Calls fn unqualified with ctx, first, and then 0 to 9 further Chain arguments. These tcapi
// templates take 2 to 11 parameters, the most being trunc_svd's 11, so each of their overloads
// over a tensor type TenT whose other parameters all accept a Chain is checked at its own arity;
// the test case checks the remaining overloads over TenT separately.
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

  // convert deduces its two tensor types separately, so either one alone being the user type
  // takes the tcapi templates out of the call as well.
  static_assert(ADL_SELECTS_USER(
      convert(ctx, std::declval<const tcapi::CytnxTensor<std::complex<double>>&>(), ctx, c)));
  static_assert(ADL_SELECTS_USER(
      convert(ctx, c, ctx, std::declval<tcapi::CytnxTensor<std::complex<double>>&>())));

  // Two overloads over TenT have a parameter a Chain cannot fill: contract's string-label form
  // takes std::string_view labels, and trunc_svd's deleted form requires a floating-point
  // target_trunc_err. Each is called here with arguments that fill those parameters.
  const std::string_view labels;
  static_assert(ADL_SELECTS_USER(contract(ctx, c, labels, c, labels, c, labels)));
  static_assert(ADL_SELECTS_USER(trunc_svd(ctx, c, c, c, c, c, c, 0.0, c)));
}

// A handle routine that stayed viable for the argument would be selected in an ordinary call, and
// in an unevaluated operand inside a SFINAE context it would leave the expression ill-formed, so a
// detection of the caller's function would report false. The generic lambda's trailing return type
// is such a context: std::is_invocable is true exactly when the call in it is well-formed.
#define ADL_CHECK_HANDLE_ARG(fn)                                              \
  {                                                                           \
    static_assert(ADL_SELECTS_USER(fn(session)));                             \
    static_assert(ADL_SELECTS_USER(fn(derived)));                             \
    auto detect = [](auto& a) -> decltype(fn(a)) { return fn(a); };           \
    static_assert(std::is_invocable_v<decltype(detect), decltype(session)&>); \
    static_assert(std::is_invocable_v<decltype(detect), decltype(derived)&>); \
  }

TEST_CASE("ADL - a caller's create_context and destroy_context are selected for its own type") {
  adl_user::Session<tcapi::CytnxContextHandle> session;
  adl_user::DerivedHandle derived;

  ADL_HANDLE_ARG_NAMES(ADL_CHECK_HANDLE_ARG)
}

#undef ADL_CHECK_HANDLE_ARG
#undef ADL_CHECK_LIST_ARG
#undef ADL_CHECK_TENSOR_ARG
#undef ADL_CHECK_SELECTED
#undef ADL_SELECTS_USER
#undef ADL_HANDLE_ARG_NAMES
#undef ADL_LIST_ARG_NAMES
#undef ADL_TENSOR_ARG_NAMES
