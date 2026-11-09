// Wires TCAPICT conformance tests into doctest via the bulk-registration
// adapter. Each `TCAPICT_DOCTEST_REGISTER_*` line expands into one `TEST_CASE`
// per applicable conformance test; see the tcapict adapter header for the
// suite categorization.
//
// TCAPI_NO_DEPRECATED_API is defined before the first include of
// `<tcapi/tcapi.h>`, which the adapter also includes, so the overloads in
// cytnx_typed_tensor_impl_deprecated.h stay out of the conformance tests.

#include <doctest/doctest.h>
#define TCAPI_NO_DEPRECATED_API
#include <tcapi/tcapi.h>
#include <tcapict/adapters/doctest.h>

using CytnxRealF = tcapi::CytnxTensor<cytnx::cytnx_float>;
using CytnxRealD = tcapi::CytnxTensor<cytnx::cytnx_double>;
using CytnxCplxF = tcapi::CytnxTensor<cytnx::cytnx_complex64>;
using CytnxCplxD = tcapi::CytnxTensor<cytnx::cytnx_complex128>;

TCAPICT_DOCTEST_REGISTER_REAL(float, CytnxRealF)
TCAPICT_DOCTEST_REGISTER_REAL(double, CytnxRealD)
TCAPICT_DOCTEST_REGISTER_CPLX(cfloat, CytnxCplxF)
TCAPICT_DOCTEST_REGISTER_CPLX(cdouble, CytnxCplxD)
