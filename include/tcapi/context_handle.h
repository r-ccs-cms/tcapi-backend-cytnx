#pragma once

namespace tcapi {

  // Of these two templates, only the CytnxContextHandle specializations below have non-deleted
  // definitions. Deleting the primary templates turns a call whose argument deduces another type
  // into a compile error instead of an undefined reference at link time. A deleted definition
  // must be the first declaration of its template, so the deletions open this header.
  template <typename ContextHandleT> void create_context(ContextHandleT& ctx) = delete;
  template <typename ContextHandleT> void destroy_context(ContextHandleT& ctx) = delete;
  class CytnxContextHandle;

  /**
   * @brief Create context handle (in-place version)
   *
   * An argument whose deduced type is not CytnxContextHandle selects the deleted primary
   * template and does not compile.
   *
   * @param ctx Context handle to initialize
   */
  template <> void create_context(CytnxContextHandle& ctx);

  /**
   * @brief Destroy context handle
   *
   * An argument whose deduced type is not CytnxContextHandle selects the deleted primary
   * template and does not compile.
   *
   * @param ctx Context handle to destroy
   */
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
