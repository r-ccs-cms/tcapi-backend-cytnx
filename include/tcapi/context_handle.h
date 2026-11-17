#pragma once

namespace tcapi {

  // Deleted so that a call to a specialization no one has declared fails to compile instead of to
  // link. A deleted definition must be the first declaration, hence the top of the header.

  /**
   * @brief Create context handle (in-place version)
   *
   * @tparam ContextHandleT Context handle type. This library defines only the CytnxContextHandle
   * specialization: with any other template argument, deduced or named, a call does not compile
   * unless the program declares its own explicit specialization for that type.
   * @param ctx Context handle to initialize
   */
  template <typename ContextHandleT> void create_context(ContextHandleT& ctx) = delete;

  /**
   * @brief Destroy context handle
   *
   * @tparam ContextHandleT Context handle type. This library defines only the CytnxContextHandle
   * specialization: with any other template argument, deduced or named, a call does not compile
   * unless the program declares its own explicit specialization for that type.
   * @param ctx Context handle to destroy
   */
  template <typename ContextHandleT> void destroy_context(ContextHandleT& ctx) = delete;

  class CytnxContextHandle;

  // Declared ahead of CytnxContextHandle, whose friend declaration names the create_context
  // specialization: declared after it, clang rejects create_context(ctx) on a handle as a call to
  // the deleted primary.

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
    friend void create_context<>(CytnxContextHandle& ctx);
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
