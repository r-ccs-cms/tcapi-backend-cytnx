#pragma once

// IWYU pragma: begin_exports
#include "tcapi/construction_destruction.h"
#include "tcapi/cytnx_tensor_traits.h"
#include "tcapi/cytnx_typed_tensor_impl.h"
#include "tcapi/debugging.h"
#include "tcapi/io_operations.h"
#include "tcapi/miscellaneous.h"
#include "tcapi/read_only_getters.h"
#include "tcapi/tensor_linear_algebra.h"
#include "tcapi/tensor_manipulation.h"
#include "tcapi/tensor_traits.h"
#include "tcapi/version.h"
// IWYU pragma: end_exports

/**
 * @file tcapi.h
 * @brief Tensor Computing Application Programming Interface (TCAPI) - Universal interface for
 * tensor computing
 *
 * This header provides a universal interface for tensor computations that can be
 * implemented by various underlying tensor libraries. The current implementation
 * uses Cytnx as the backend tensor library.
 *
 * The interface is designed using C++ template genericity to abstract different
 * tensor library implementations while providing a consistent API.
 */

namespace tcapi {
  // All TCAPI functions and types are defined in their respective headers
  // This main header serves as a convenient include point for all TCAPI functionality
}