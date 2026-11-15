#pragma once

#include <cytnx.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace tcapi {

  namespace detail {

    /// True when `t` holds the dtype `ElemT` maps to, or is default-constructed (dtype Void).
    /// The CytnxTensor constructor and tcapi::load both apply this rule.
    template <typename ElemT> bool backend_dtype_is_valid(const cytnx::Tensor& t) {
      return t.is_void() || t.dtype() == cytnx::Type_class::cy_typeid_v<ElemT>;
    }

    /// Cytnx's name for the dtype `ElemT` maps to, for error messages.
    template <typename ElemT> std::string dtype_name() {
      return cytnx::Type.getname(cytnx::Type_class::cy_typeid_v<ElemT>);
    }

  }  // namespace detail

  /**
   * @brief Typed wrapper for cytnx::Tensor to provide compile-time element type information
   *
   * @tparam ElemT Element type (cytnx::cytnx_double, cytnx::cytnx_complex128, etc.)
   *
   * This wrapper enables compile-time type safety for Cytnx tensors, which are
   * dynamically typed at runtime. It provides a TenT type that can be used with
   * tcapi::tensor_traits, similar to other statically-typed tensor backends.
   * At runtime, `backend` must hold the dtype `ElemT` maps to or be default-constructed; see
   * `backend`.
   *
   * CytnxTensor is not copyable, because TCAPI V1 does not require TenT to be: code meant to run
   * on any conforming backend duplicates a tensor with `tcapi::copy` and transfers one with
   * `tcapi::move`.
   *
   * Usage:
   * @code
   * using Tensor = tcapi::CytnxTensor<cytnx::cytnx_complex128>;
   * using Elem = tcapi::tensor_traits<Tensor>::elem_t;  // = std::complex<double>
   *
   * Tensor t;
   * tcapi::allocate(ctx, {3, 3}, t);
   * Elem elem = tcapi::get_elem(ctx, t, {0, 0});
   * auto result = elem / 2.0;  // Standard C++ operators work
   * @endcode
   */
  template <typename ElemT> struct CytnxTensor {
    /// The underlying Cytnx tensor backend. It holds the dtype `ElemT` maps to, or is
    /// default-constructed (dtype Void); tcapi functions assume this. Cytnx operations on it can
    /// change its dtype without error, so code that writes to it must keep the dtype.
    cytnx::Tensor backend;

    /// Compile-time element type information
    using elem_type = ElemT;

    /// Default constructor
    CytnxTensor() = default;

    /// Construct from existing cytnx::Tensor. Throws std::invalid_argument when `t` is neither of
    /// the dtype `ElemT` maps to nor default-constructed.
    explicit CytnxTensor(cytnx::Tensor t) : backend(std::move(t)) {
      if (!detail::backend_dtype_is_valid<ElemT>(backend)) {
        throw std::invalid_argument("tcapi::CytnxTensor<" + detail::dtype_name<ElemT>()
                                    + "> cannot hold a cytnx::Tensor of dtype "
                                    + cytnx::Type.getname(backend.dtype()));
      }
    }

    /// Deleted: an ordinary C++ copy is not part of the TCAPI V1 contract, and a copy of `backend`
    /// shares its storage. Use `tcapi::copy` for a deep copy.
    CytnxTensor(const CytnxTensor&) = delete;

    /// Move constructor. cytnx::Tensor has no move operations, so the source keeps sharing the
    /// storage it held with the new tensor; `tcapi::move` leaves the source default-constructed.
    CytnxTensor(CytnxTensor&&) = default;

    /// Deleted, like the copy constructor. Assign the result of `tcapi::copy` for a deep copy.
    CytnxTensor& operator=(const CytnxTensor&) = delete;

    /// Move assignment. As with the move constructor, the source keeps sharing its storage.
    CytnxTensor& operator=(CytnxTensor&&) = default;

    /// Destructor
    ~CytnxTensor() = default;
  };

}  // namespace tcapi