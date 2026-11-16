#include <doctest/doctest.h>
#include <tcapi/tcapi.h>

#include <complex>
#include <type_traits>
#include <utility>

// A tcapi context argument brings namespace tcapi into argument-dependent lookup. These tests pin
// that a tcapi function template deduced with a type that has no tensor_traits specialization
// drops out of overload resolution, so a caller's own function of the same name is selected.

using Ten = tcapi::CytnxTensor<std::complex<double>>;

// Stands in for user code. Each function returns its own sentinel, so a call that resolved to
// the tcapi namesake, or to another of these functions, fails its check.
namespace adl_user {

  template <typename TenT> struct Chain {};

  template <typename TenT> int norm(tcapi::context_handle_t<TenT>&, const Chain<TenT>&) {
    return 1;
  }

  template <typename TenT> int normalize(tcapi::context_handle_t<TenT>&, Chain<TenT>&) { return 2; }

  template <typename TenT> int order(tcapi::context_handle_t<TenT>&, const Chain<TenT>&) {
    return 3;
  }

  template <typename TenT> int show(tcapi::context_handle_t<TenT>&, const Chain<TenT>&) {
    return 4;
  }

  template <typename TenT> int copy(tcapi::context_handle_t<TenT>&, const Chain<TenT>&) {
    return 5;
  }

  template <typename TenT> int convert(tcapi::context_handle_t<TenT>&, const Chain<TenT>&,
                                       tcapi::context_handle_t<TenT>&, Chain<TenT>&) {
    return 6;
  }

}  // namespace adl_user

// norm_accepts<T> holds when tcapi::norm(ctx, a) is well-formed for an lvalue ctx of type
// context_handle_t<Ten> and an lvalue a of type const T.
template <typename T, typename Void = void> struct norm_accepts : std::false_type {};

template <typename T>
struct norm_accepts<T,
                    std::void_t<decltype(tcapi::norm(std::declval<tcapi::context_handle_t<Ten>&>(),
                                                     std::declval<const T&>()))>> : std::true_type {
};

static_assert(norm_accepts<Ten>::value);
static_assert(!norm_accepts<int>::value);

TEST_CASE("ADL - a caller's function named like a tcapi function is selected for its own type") {
  tcapi::context_handle_t<Ten> ctx;
  tcapi::create_context(ctx);

  adl_user::Chain<Ten> in;
  adl_user::Chain<Ten> out;

  CHECK(norm(ctx, in) == 1);
  CHECK(normalize(ctx, out) == 2);
  CHECK(order(ctx, in) == 3);
  CHECK(show(ctx, in) == 4);
  CHECK(copy(ctx, in) == 5);
  CHECK(convert(ctx, in, ctx, out) == 6);

  tcapi::destroy_context(ctx);
}
