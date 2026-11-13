#pragma once

// Sample tensor shared by the backend save / load tests.

#include <doctest/doctest.h>
#include <tcapi/tcapi.h>

#include <type_traits>

// Returns a two-element tensor holding 1.5 and -2.5 + 0.5i, the imaginary part
// dropped for a real element type.
template <typename TenT> TenT make_sample(tcapi::context_handle_t<TenT>& ctx) {
  using Elem = tcapi::elem_t<TenT>;
  TenT t;
  tcapi::allocate(ctx, {2}, t);
  tcapi::set_elem(ctx, t, {0}, Elem(1.5));
  if constexpr (std::is_floating_point_v<Elem>) {
    tcapi::set_elem(ctx, t, {1}, Elem(-2.5));
  } else {
    tcapi::set_elem(ctx, t, {1}, Elem(-2.5, 0.5));
  }
  return t;
}

// Checks that `t` holds the values make_sample sets for a real element type.
template <typename Ctx, typename TenT> void check_real_sample(Ctx& ctx, const TenT& t) {
  CHECK(tcapi::get_elem(ctx, t, {0}) == 1.5);
  CHECK(tcapi::get_elem(ctx, t, {1}) == -2.5);
}
