// This translation unit includes tcapi/context_handle.h and no other tcapi header, so it checks
// that context_handle.h declares the CytnxContextHandle specializations of create_context and
// destroy_context by itself. tcapi/miscellaneous.h redeclares them, which would hide a missing
// declaration from any translation unit that includes it. Without a declaration, a deduced call
// on a CytnxContextHandle would name the deleted primary template.

#include <utility>

#include "detected.h"
#include "tcapi/context_handle.h"

namespace {

  template <typename Arg> using create_context_call
      = decltype(tcapi::create_context(std::declval<Arg&>()));

  template <typename Arg> using destroy_context_call
      = decltype(tcapi::destroy_context(std::declval<Arg&>()));

  static_assert(detected<create_context_call, tcapi::CytnxContextHandle>::value);
  static_assert(detected<destroy_context_call, tcapi::CytnxContextHandle>::value);

}  // namespace
