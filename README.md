# TCAPI (Tensor Computing Application Programming Interface) - Cytnx Backend

[TCAPI (Tensor Computing Application Programming Interface)](https://arxiv.org/abs/2512.23917) is a universal C++ interface for tensor computing libraries. This repository provides the Cytnx backend implementation, allowing TCAPI APIs to work with the [Cytnx](https://github.com/Cytnx-dev/Cytnx) tensor library.

## C++ Standard Requirements

- **Public API**: C++17 compatible — no C++20 features leak into user-facing headers
- **TCAPI build**: C++20 (required by Cytnx headers)

## Dependencies

### Required

- **CMake 3.25+**: Required for Cytnx
- **C++20 compiler with a GNU-style command line**: GCC, Clang (AppleClang included), or Intel oneAPI icpx, whose standard library provides `std::format`, which Cytnx requires: libstdc++ from GCC 13 or later, or libc++ from LLVM 17 or later. Clang must be 16 or later to build against libstdc++. AppleClang uses the libc++ that comes with Xcode or its Command Line Tools, which carries no LLVM release number; configure checks it for `std::format` and stops with an error when it is missing
- **Not supported**: compilers that CMake drives with MSVC's `cl` command-line syntax (MSVC, clang-cl, and the Intel oneAPI compilers on Windows); configuring with one stops with an error. Intel's classic `icpc` is not supported either: it cannot compile libstdc++'s `<format>`, which Cytnx requires
- **BLAS/LAPACK**: OpenBLAS recommended
- **Cytnx**: Included as git submodule
- **OpenMP**: For parallel computations

### Optional

- **CUDA**: For GPU acceleration (via Cytnx)

## Quick Start

### 1. Clone with Submodules

```bash
git clone --recursive https://github.com/r-ccs-cms/tcapi-backend-cytnx.git
cd tcapi-backend-cytnx
```

### 2. Install Dependencies

#### macOS with Homebrew

```bash
brew install openblas llvm libomp cmake boost arpack ninja
```

The `brew*` presets build with Apple Clang, the system `c++`, unless `CC` and `CXX` select another compiler; `llvm` is optional.

#### Linux with Intel oneAPI (HPC environments)

```bash
module load intel/oneapi  # Provides icx, icpx, ifort, and MKL
module load boost         # Required by Cytnx
```

ARPACK-NG will be automatically built from source if not found. To use a pre-built ARPACK, set `ARPACK_ROOT` or pass `-DARPACK_ROOT=/path/to/arpack`.

### 3. Configure, Build, and Test

#### Using CMake Presets (Recommended)

**macOS with Homebrew:**

```bash
cmake --preset brew-debug
cmake --build --preset brew-debug
ctest --preset brew-debug
```

**Linux with Intel oneAPI:**

```bash
module load intel/oneapi boost

cmake --preset intel-debug
cmake --build --preset intel-debug
ctest --preset intel-debug
```

Replace `debug` with `release` for optimized builds.

#### Manual Configuration

```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/macos-homebrew.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_STANDARD=20 \
  -DBUILD_PYTHON=OFF \
  -DTCAPI_BUILD_TESTS=ON

cmake --build build --parallel 8
```

### Running Tests Directly

```bash
# All tests
./build-debug/test/TCAPITests

# Specific test cases
./build-debug/test/TCAPITests --test-case="*template*"
```

## Usage Example

### Basic Usage with CytnxTensor

```cpp
#include "tcapi/tcapi.h"

int main() {
    using Tensor = tcapi::CytnxTensor<cytnx::cytnx_complex128>;

    tcapi::context_handle_t<Tensor> ctx;
    tcapi::create_context(ctx);

    Tensor a = tcapi::zeros<Tensor>(ctx, {3, 4});
    Tensor b = tcapi::eye<Tensor>(ctx, 3);

    Tensor c = tcapi::copy(ctx, a);
    auto norm_val = tcapi::norm(ctx, b);

    tcapi::show(ctx, b);

    tcapi::destroy_context(ctx);
    return 0;
}
```

### Element-wise Arithmetic

```cpp
#include "tcapi/tcapi.h"

int main() {
    using Tensor = tcapi::CytnxTensor<cytnx::cytnx_complex128>;
    using Elem = tcapi::elem_t<Tensor>;  // std::complex<double>

    tcapi::context_handle_t<Tensor> ctx;
    tcapi::create_context(ctx);

    Tensor tensor = tcapi::allocate<Tensor>(ctx, {100, 100});

    tcapi::for_each(ctx, tensor, [](Elem& elem) {
        elem = std::sqrt(elem) * 2.0;
        elem = elem / std::abs(elem);
    });

    tcapi::destroy_context(ctx);
    return 0;
}
```

- `CytnxTensor<ElemT>` fixes the element type at compile time, so `tcapi::elem_t<Tensor>` resolves to `ElemT` and supports direct arithmetic.
- The wrapper exposes the underlying Cytnx tensor via `Tensor::backend` when lower-level Cytnx APIs are required. `backend` holds the dtype `ElemT` maps to, or is default-constructed, and TCAPI functions assume this. Constructing a `CytnxTensor<ElemT>` from a `cytnx::Tensor` of another dtype throws `std::invalid_argument`. Cytnx operations on `backend` can change its dtype without error, for example in-place `+=` with a complex tensor operand or assigning the result of `.real()`, so code that writes to it must keep the dtype.

### Compiling User Code (C++17)

```bash
g++ -std=c++17 -I<tcapi-install>/include your_code.cpp -lTCAPI -lcytnx
```

## Backend Behavior

The functions follow the TCAPI specification linked above. This section covers behavior specific to this backend.

### Contexts

`tcapi::create_context(ctx)` binds `ctx` to GPU 0 when Cytnx reports a GPU, and to the CPU otherwise. `tcapi::create_context(ctx, gpu_id)` binds it to another GPU, and throws when Cytnx reports no GPU or `gpu_id` is out of range.

### Copying and Moving

`CytnxTensor` cannot be copied: `Tensor b = a;` and `b = a;` do not compile, because TCAPI V1 does not require tensors to be copyable. Duplicate a tensor with `tcapi::copy`, which makes a deep copy, and transfer one with `tcapi::move`, which leaves the source as `Tensor{}`:

```cpp
Tensor b = tcapi::copy<Tensor>(ctx, a);  // b has its own storage
Tensor c = tcapi::move<Tensor>(ctx, b);  // c takes b's storage; b is now Tensor{}
```

A plain `std::move` also compiles, but the moved-from tensor keeps sharing storage with the tensor it was moved into, so a later write through one is visible through the other. Use `tcapi::move` when the source will be used again.

### Saving and Loading

`tcapi::load` reads a file as the element type it was saved with, and throws `std::runtime_error` for any other element type. To get another element type, load the saved one and change it: `tcapi::convert` casts real to complex and between precisions, and `tcapi::real` or `tcapi::imag` take a complex tensor to a real one.

`tcapi::save` of a default-constructed tensor throws `std::runtime_error` before anything is written, so an existing file at the path is left as it was.

## Documentation

```bash
# Requires Doxygen and uv
cmake -S documentation -B build/doc
cmake --build build/doc --target GenerateDocs
open build/doc/doxygen/html/index.html
```

## License

[Apache-2.0 License](LICENSE)

