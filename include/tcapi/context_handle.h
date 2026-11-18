#pragma once

#include <type_traits>

namespace tcapi {

  class CytnxContextHandle;

  namespace detail {

    // Return type of the create_context and destroy_context templates: void for
    // CytnxContextHandle, and a substitution failure for any other type.
    template <typename ContextHandleT> using if_cytnx_context_handle_t
        = std::enable_if_t<std::is_same_v<ContextHandleT, CytnxContextHandle>>;

  }  // namespace detail

  /**
   * @brief Create context handle (in-place version)
   *
   * @tparam ContextHandleT Context handle type. The template takes part in overload resolution
   * only for CytnxContextHandle. With any other template argument, deduced or named, it drops
   * out, so an unqualified call that argument-dependent lookup routes into tcapi selects the
   * caller's own function of the same name.
   * @param ctx Context handle to initialize
   */
  template <typename ContextHandleT>
  detail::if_cytnx_context_handle_t<ContextHandleT> create_context(ContextHandleT& ctx);

  /**
   * @brief Destroy context handle
   *
   * @tparam ContextHandleT Context handle type. The template takes part in overload resolution
   * only for CytnxContextHandle. With any other template argument, deduced or named, it drops
   * out, so an unqualified call that argument-dependent lookup routes into tcapi selects the
   * caller's own function of the same name.
   * @param ctx Context handle to destroy
   */
  template <typename ContextHandleT>
  detail::if_cytnx_context_handle_t<ContextHandleT> destroy_context(ContextHandleT& ctx);

  /// @brief Create context handle (in-place version), targeting GPU 0 when Cytnx reports a GPU
  /// and the CPU otherwise
  template <> void create_context(CytnxContextHandle& ctx);

  /// @brief Destroy context handle
  template <> void destroy_context(CytnxContextHandle& ctx);

  void create_context(CytnxContextHandle& ctx, int gpu_id);

  /**
   * @brief Context handle for Cytnx backend
   *
   * This class wraps an integer device ID and restricts direct assignment.
   * Only create_context() is allowed to change the value after construction,
   * preventing accidental misuse like `ctx = 0;`.
   * Default-constructed handles target CPU (cytnx::Device.cpu == -1).
   */
  class CytnxContextHandle {
  private:
    int value_;

    // Only create_context can call this
    void set_value(int v) { value_ = v; }

    // Friend declarations to allow create_context access
    template <typename T> friend detail::if_cytnx_context_handle_t<T> create_context(T& ctx);
    friend void create_context(CytnxContextHandle& ctx, int gpu_id);

  public:
    // Default to CPU device (cytnx::Device.cpu == -1); valid immediately after construction (RAII)
    CytnxContextHandle() : value_(-1) {}

    // Delete construction and assignment from int
    CytnxContextHandle(int) = delete;
    CytnxContextHandle& operator=(int) = delete;

    // Allow copy/move operations
    CytnxContextHandle(const CytnxContextHandle&) = default;
    CytnxContextHandle& operator=(const CytnxContextHandle&) = default;
    CytnxContextHandle(CytnxContextHandle&&) = default;
    CytnxContextHandle& operator=(CytnxContextHandle&&) = default;

    // Implicit conversion to int (for Cytnx API compatibility)
    operator int() const { return value_; }

    // Explicit getter
    int get() const { return value_; }
  };

}  // namespace tcapi
