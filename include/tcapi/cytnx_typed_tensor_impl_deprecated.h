#pragma once

// TenT forms that the TCAPI V1 specification does not declare, kept for
// existing callers: the in-place construction functions and load, which the
// return-value forms replace; eq, to_container, assign_from_container, rank
// and rank_t, which close, to_range, assign_from_range, order and order_t
// replace; and trunc_svd overloads with pre-specification trailing
// parameters. Each is marked [[deprecated]] with its replacement, except the
// in-place assign_from_container.

#include "tcapi/cytnx_typed_tensor_impl.h"
#include "tcapi/io_operations.h"
#include "tcapi/miscellaneous.h"

namespace tcapi {

  // ============================================================
  // Construction and Destruction
  // ============================================================

  template <typename TenT>
  [[deprecated("Reserved for future GPU support. Use: auto result = tcapi::fill(ctx, shape, v);")]]
  void fill(context_handle_t<TenT>& ctx, const shape_t<TenT>& shape, elem_t<TenT> value, TenT& a) {
    a = allocate<TenT>(ctx, shape);
    auto total_size = static_cast<cytnx::cytnx_uint64>(a.backend.storage().size());
    auto* data = a.backend.storage().template data<elem_t<TenT>>();
    for (cytnx::cytnx_uint64 i = 0; i < total_size; ++i) {
      data[i] = value;
    }
  }

  template <typename TenT>
  [[deprecated("Reserved for future GPU support. Use: auto result = tcapi::zeros(ctx, shape);")]]
  void zeros(context_handle_t<TenT>& ctx, const shape_t<TenT>& shape, TenT& a) {
    fill(ctx, shape, elem_t<TenT>(0), a);
  }

  template <typename TenT>
  [[deprecated("Reserved for future GPU support. Use: auto result = tcapi::eye(ctx, N);")]]
  void eye(context_handle_t<TenT>& ctx, bond_dim_t<TenT> dim, TenT& a) {
    a.backend = cytnx::eye(dim, detail::elem_to_cytnx_type<elem_t<TenT>>(), ctx);
  }

  template <typename TenT, typename RandNumGen>
  [[deprecated("Use return-value version instead: auto result = random(ctx, shape, gen)")]]
  void random(context_handle_t<TenT>& ctx, const shape_t<TenT>& shape, RandNumGen& gen, TenT& a) {
    a = random<TenT>(ctx, shape, gen);
  }

  template <typename TenT, typename RandomIt, typename Func> [[deprecated(
      "Use assign_from_range instead. This API will be removed in the next major version")]]
  TenT assign_from_container(context_handle_t<TenT>& ctx, const shape_t<TenT>& shape,
                             RandomIt init_elems_begin, Func&& coors2idx) {
    return assign_from_range<TenT, RandomIt, Func>(ctx, shape, init_elems_begin,
                                                   std::forward<Func>(coors2idx));
  }

  template <typename TenT>
  [[deprecated("Use return-value version instead: auto result = allocate(ctx, shape)")]]
  void allocate(context_handle_t<TenT>& ctx, const shape_t<TenT>& shape, TenT& a) {
    a = allocate<TenT>(ctx, shape);
  }

  template <typename TenT>
  [[deprecated("Use return-value version instead: auto result = copy(ctx, orig)")]]
  void copy(context_handle_t<TenT>& ctx, const TenT& orig, TenT& dist) {
    dist = copy(ctx, orig);
  }

  template <typename TenT>
  [[deprecated("Use return-value version instead: auto result = move(ctx, from)")]]
  void move(context_handle_t<TenT>& ctx, TenT& from, TenT& to) {
    to = move(ctx, from);
  }

  // ============================================================
  // Read-only Getters
  // ============================================================

  template <typename TenT>
  [[deprecated("Use tcapi::order instead. This API will be removed in the next major version")]]
  order_t<TenT> rank(context_handle_t<TenT>& ctx, const TenT& a) {
    return order(ctx, a);
  }

  // ============================================================
  // Tensor Linear Algebra
  // ============================================================

  // Old trunc_svd overload (1) - only s_min
  template <typename TenT> [[deprecated(
      "Parameter order changed. Use trunc_svd(..., trunc_err, chi_max, s_min) or trunc_svd(..., "
      "trunc_err, chi_max, s_min, chi_min, target_trunc_err). This API will be removed in the next "
      "major version")]]
  void trunc_svd(context_handle_t<TenT>& ctx, const TenT& a, const order_t<TenT> num_of_bds_as_row,
                 TenT& u, real_ten_t<TenT>& s_diag, TenT& v_dag, real_t<TenT>& trunc_err,
                 const real_t<TenT> s_min) {
    // Forward to spec_v1 overload (2) with chi_min=1, target_trunc_err=0
    constexpr bond_dim_t<TenT> chi_min = 1;
    constexpr real_t<TenT> target_trunc_err = 0.0;
    trunc_svd(ctx, a, num_of_bds_as_row, u, s_diag, v_dag, trunc_err,
              std::numeric_limits<bond_dim_t<TenT>>::max(), s_min, chi_min, target_trunc_err);
  }

  // Old trunc_svd overload (2) - chi_max, target_trunc_err, s_min
  template <typename TenT> [[deprecated(
      "Parameter order changed. Use trunc_svd(..., trunc_err, chi_max, s_min) or trunc_svd(..., "
      "trunc_err, chi_max, s_min, chi_min, target_trunc_err). This API will be removed in the next "
      "major version")]]
  void trunc_svd(context_handle_t<TenT>& ctx, const TenT& a, const order_t<TenT> num_of_bds_as_row,
                 TenT& u, real_ten_t<TenT>& s_diag, TenT& v_dag, real_t<TenT>& trunc_err,
                 const bond_dim_t<TenT> chi_max, const real_t<TenT> target_trunc_err,
                 const real_t<TenT> s_min) {
    // Forward to spec_v1 overload (2) with chi_min=1
    constexpr bond_dim_t<TenT> chi_min = 1;
    trunc_svd(ctx, a, num_of_bds_as_row, u, s_diag, v_dag, trunc_err, chi_max, s_min, chi_min,
              target_trunc_err);
  }

  // trunc_svd(target_trunc_err, s_min) - not a spec_v1 form
  template <typename TenT> [[deprecated(
      "Not a TCAPI spec form. Its (target_trunc_err, s_min) tail collides with the spec's "
      "(chi_max, s_min) tail, so wherever both are declared the spec-shaped call is ambiguous "
      "for an integer literal. Use trunc_svd(..., trunc_err, chi_max, s_min, chi_min, "
      "target_trunc_err). This API will be removed in the next major version")]]
  void trunc_svd(context_handle_t<TenT>& ctx, const TenT& a, const order_t<TenT> num_of_bds_as_row,
                 TenT& u, real_ten_t<TenT>& s_diag, TenT& v_dag, real_t<TenT>& trunc_err,
                 const real_t<TenT> target_trunc_err, const real_t<TenT> s_min) {
    // Forward to the (chi_max, s_min, chi_min, target_trunc_err) form, capping chi_max at the
    // largest representable bond dimension rather than leaving it unconstrained.
    constexpr bond_dim_t<TenT> chi_min = 1;
    constexpr bond_dim_t<TenT> chi_max = std::numeric_limits<bond_dim_t<TenT>>::max();
    trunc_svd(ctx, a, num_of_bds_as_row, u, s_diag, v_dag, trunc_err, chi_max, s_min, chi_min,
              target_trunc_err);
  }

  // ============================================================
  // Miscellaneous
  // ============================================================

  template <typename TenT>
  [[deprecated("Use close instead. This API will be removed in the next major version")]]
  bool eq(context_handle_t<TenT>& ctx, const TenT& a, const TenT& b, const real_t<TenT> epsilon) {
    return close(ctx, a, b, epsilon);
  }

  // assign_from_container - create tensor from container, in place
  template <typename TenT, typename RandomIt, typename Func>
  void assign_from_container(context_handle_t<TenT>& ctx, const shape_t<TenT>& shape,
                             RandomIt init_elems_begin, Func&& coors2idx, TenT& a) {
    // Allocate tensor with the specified shape
    a = allocate<TenT>(ctx, shape);

    // Generate all coordinate combinations and assign values
    std::function<void(elem_coors_t<TenT>, std::size_t)> assign_recursive;
    assign_recursive = [&](elem_coors_t<TenT> current_coords, std::size_t dim) {
      if (dim == shape.size()) {
        // Base case: all dimensions set, assign the element
        auto index = std::invoke(coors2idx, current_coords);
        auto value = *(init_elems_begin + index);

        // Convert value to elem_t<TenT>
        elem_t<TenT> elem_val;
        if constexpr (std::is_same_v<elem_t<TenT>, decltype(value)>) {
          elem_val = value;
        } else if constexpr (std::is_arithmetic_v<decltype(value)>) {
          elem_val = static_cast<elem_t<TenT>>(value);
        } else if constexpr (std::is_same_v<elem_t<TenT>, cytnx::cytnx_complex128>
                             && std::is_same_v<decltype(value), std::complex<double>>) {
          elem_val = cytnx::cytnx_complex128(value.real(), value.imag());
        } else if constexpr (std::is_same_v<elem_t<TenT>, cytnx::cytnx_complex64>
                             && std::is_same_v<decltype(value), std::complex<float>>) {
          elem_val = cytnx::cytnx_complex64(value.real(), value.imag());
        } else {
          elem_val = static_cast<elem_t<TenT>>(value);
        }

        set_elem(ctx, a, current_coords, elem_val);
      } else {
        // Recursive case: iterate through current dimension
        for (bond_dim_t<TenT> i = 0; i < shape[dim]; ++i) {
          current_coords.push_back(i);
          assign_recursive(current_coords, dim + 1);
          current_coords.pop_back();
        }
      }
    };

    assign_recursive({}, 0);
  }

  template <typename TenT, typename RandomIt, typename Func>
  [[deprecated("Use to_range instead. This API will be removed in the next major version")]]
  void to_container(context_handle_t<TenT>& ctx, const TenT& a, RandomIt first, Func&& coors2idx) {
    to_range(ctx, a, first, std::forward<Func>(coors2idx));
  }

  // ============================================================
  // I/O
  // ============================================================

  template <typename TenT, typename Storage>
  [[deprecated("Use return-value version instead: auto result = tcapi::load<TenT>(ctx, strg)")]]
  inline void load(context_handle_t<TenT>& ctx, Storage&& strg, TenT& a) {
    a = load<TenT>(ctx, std::forward<Storage>(strg));
  }

  // ============================================================
  // Deprecated type aliases
  // ============================================================

  template <typename TenT> using rank_t [[deprecated(
      "Use tcapi::order_t instead. This API will be removed in the next major version")]]
  = typename detail::rank_type_fallback<TenT>::type;

}  // namespace tcapi
