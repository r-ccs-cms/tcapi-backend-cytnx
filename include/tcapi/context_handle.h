#pragma once

#include <type_traits>

namespace tcapi {

  class CytnxContextHandle;

  namespace detail {

    // Default argument of the second template parameter of the create_context and destroy_context
    // templates: void for CytnxContextHandle, and a substitution failure for any other type.
    template <typename ContextHandleT> using if_cytnx_context_handle_t
        = std::enable_if_t<std::is_same_v<ContextHandleT, CytnxContextHandle>>;

  }  // namespace detail

  // Deleted so that a call to a specialization no one has declared fails to compile instead of to
  // link. A deleted definition must be the first declaration, hence these precede every other
  // declaration of the two templates.

  /**
   * @brief Create context handle (in-place version)
   *
   * @tparam ContextHandleT Context handle type. With the second template argument left to its
   * default, the template takes part in overload resolution only for CytnxContextHandle, and for
   * any other ContextHandleT, deduced or named, it drops out.
   * @param ctx Context handle to initialize
   */
  template <typename ContextHandleT, typename = detail::if_cytnx_context_handle_t<ContextHandleT>>
  void create_context(ContextHandleT& ctx) = delete;

  /**
   * @brief Destroy context handle
   *
   * @tparam ContextHandleT Context handle type. With the second template argument left to its
   * default, the template takes part in overload resolution only for CytnxContextHandle, and for
   * any other ContextHandleT, deduced or named, it drops out.
   * @param ctx Context handle to destroy
   */
  template <typename ContextHandleT, typename = detail::if_cytnx_context_handle_t<ContextHandleT>>
  void destroy_context(ContextHandleT& ctx) = delete;

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
   * The handle does not convert to int either; get() reads the device ID.
   * Default-constructed handles target CPU (cytnx::Device.cpu == -1).
   */
  class CytnxContextHandle {
  private:
    int value_;

    // Only create_context can call this
    void set_value(int v) { value_ = v; }

    // Friend declarations to allow create_context access
    template <typename T, typename> friend void create_context(T& ctx);
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

    // A call that passes the handle to an int through this conversion fails to compile. Being
    // deleted, the conversion still takes part in overload resolution, so such a call cannot
    // silently select another overload instead.
    operator int() const = delete;

    // Explicit getter
    int get() const { return value_; }
  };

}  // namespace tcapi
