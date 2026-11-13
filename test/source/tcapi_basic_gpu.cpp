#include <doctest/doctest.h>
#include <tcapi/tcapi.h>

#include <cytnx.hpp>
#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <vector>

namespace {

  // True, after reporting the skip, when no CUDA device is available.
  bool skip_without_gpu() {
    if (cytnx::Device.Ngpus > 0) return false;
    MESSAGE("Skipping GPU tests - no CUDA devices available");
    return true;
  }

}  // namespace

TEST_CASE("TCAPI Context Management") {
  tcapi::context_handle_t<tcapi::CytnxTensor<cytnx::cytnx_complex128>> ctx;

  SUBCASE("Create and destroy context") {
    CHECK_NOTHROW(tcapi::create_context(ctx));
    CHECK_NOTHROW(tcapi::destroy_context(ctx));
  }

  SUBCASE("Create GPU context") {
    if (cytnx::Device.Ngpus > 0) {
      CHECK_NOTHROW(tcapi::create_context(ctx, 0));  // GPU 0
      CHECK(ctx.get() == cytnx::Device.cuda + 0);
      CHECK_NOTHROW(tcapi::destroy_context(ctx));
    }
  }

  SUBCASE("Create context with different GPU IDs") {
    if (cytnx::Device.Ngpus > 1) {
      CHECK_NOTHROW(tcapi::create_context(ctx, 1));  // GPU 1
      CHECK(ctx.get() == cytnx::Device.cuda + 1);
      CHECK_NOTHROW(tcapi::destroy_context(ctx));
    }
  }

  SUBCASE("GPU context error handling") {
    if (cytnx::Device.Ngpus == 0) {
      // No GPU available - should throw
      CHECK_THROWS_AS(tcapi::create_context(ctx, 0), std::runtime_error);
    } else {
      // GPU available - test out of range
      CHECK_THROWS_AS(tcapi::create_context(ctx, cytnx::Device.Ngpus), std::out_of_range);
      CHECK_THROWS_AS(tcapi::create_context(ctx, -1), std::out_of_range);
    }
  }
}

TEST_CASE("TCAPI GPU Tensor Operations") {
  // Skip test if no GPU available
  if (skip_without_gpu()) return;

  tcapi::context_handle_t<tcapi::CytnxTensor<cytnx::cytnx_double>> ctx;
  tcapi::create_context(ctx, 0);  // GPU 0

  SUBCASE("Create tensor on GPU") {
    tcapi::shape_t<tcapi::CytnxTensor<cytnx::cytnx_double>> shape = {2, 3};
    tcapi::CytnxTensor<cytnx::cytnx_double> tensor;

    CHECK_NOTHROW(tcapi::zeros(ctx, shape, tensor));
    CHECK(tcapi::order(ctx, tensor) == 2);
    CHECK(tcapi::shape(ctx, tensor) == shape);
  }

  SUBCASE("GPU tensor element operations") {
    tcapi::shape_t<tcapi::CytnxTensor<cytnx::cytnx_double>> shape = {2, 2};
    tcapi::CytnxTensor<cytnx::cytnx_double> tensor;
    tcapi::zeros(ctx, shape, tensor);

    // Set and get element on GPU
    tcapi::elem_coors_t<tcapi::CytnxTensor<cytnx::cytnx_double>> coord = {0, 0};
    CHECK_NOTHROW(tcapi::set_elem(ctx, tensor, coord, 42.0));

    auto elem = tcapi::get_elem(ctx, tensor, coord);
    CHECK(std::abs(elem - 42.0) < 1e-10);
  }

  tcapi::destroy_context(ctx);
}

TEST_CASE("TCAPI linear_combine - inputs on different devices") {
  if (skip_without_gpu()) return;

  using Ten = tcapi::CytnxTensor<cytnx::cytnx_double>;
  // A default-constructed handle targets the CPU; create_context without a GPU id would pick
  // GPU 0 here, because this test runs only where a GPU exists.
  tcapi::context_handle_t<Ten> cpu_ctx;
  tcapi::context_handle_t<Ten> gpu_ctx;
  tcapi::create_context(gpu_ctx, 0);  // GPU 0

  // Zeroth-order inputs: some of Cytnx's additions accept a one-element CPU operand
  // beside a GPU tensor, so without the device check some orders would be summed.
  auto on_cpu = tcapi::zeros<Ten>(cpu_ctx, {});
  auto on_gpu = tcapi::zeros<Ten>(gpu_ctx, {});
  const tcapi::List<tcapi::CRef<Ten>> gpu_first = {std::cref(on_gpu), std::cref(on_cpu)};
  const tcapi::List<tcapi::CRef<Ten>> cpu_first = {std::cref(on_cpu), std::cref(on_gpu)};
  // Tensors with more than one element.
  auto matrix_on_cpu = tcapi::zeros<Ten>(cpu_ctx, {2, 3});
  auto matrix_on_gpu = tcapi::zeros<Ten>(gpu_ctx, {2, 3});
  const tcapi::List<tcapi::CRef<Ten>> matrix_gpu_first
      = {std::cref(matrix_on_gpu), std::cref(matrix_on_cpu)};
  const tcapi::List<tcapi::CRef<Ten>> matrix_cpu_first
      = {std::cref(matrix_on_cpu), std::cref(matrix_on_gpu)};
  for (const Ten* t : {&on_cpu, &matrix_on_cpu}) REQUIRE(t->backend.device() == cytnx::Device.cpu);
  for (const Ten* t : {&on_gpu, &matrix_on_gpu}) REQUIRE(t->backend.device() == cytnx::Device.cuda);
  for (const auto* ins : {&gpu_first, &cpu_first, &matrix_gpu_first, &matrix_cpu_first}) {
    CHECK_THROWS_AS(tcapi::linear_combine<Ten>(gpu_ctx, *ins), std::invalid_argument);
    CHECK_THROWS_AS(tcapi::linear_combine<Ten>(gpu_ctx, *ins, {1.0, 1.0}), std::invalid_argument);
  }
  // The message names the input on the other device.
  CHECK_THROWS_WITH_AS(tcapi::linear_combine<Ten>(gpu_ctx, gpu_first),
                       "linear_combine: input 1 is on a device other than input 0's",
                       std::invalid_argument);

  tcapi::destroy_context(gpu_ctx);
}

TEST_CASE("TCAPI linear_combine - accumulation on a GPU") {
  if (skip_without_gpu()) return;

  using Ten = tcapi::CytnxTensor<cytnx::cytnx_double>;
  tcapi::context_handle_t<Ten> ctx;
  tcapi::create_context(ctx, 0);  // GPU 0

  // a(i, j) = 3 i + j. b is a {3, 2} tensor with b(r, c) = 10 + 2 r + c, transposed on the GPU,
  // so b(i, j) = 10 + 2 j + i and its backend is non-contiguous.
  Ten a(cytnx::arange(6).astype(cytnx::Type.Double).reshape({2, 3}).to(cytnx::Device.cuda));
  Ten b((cytnx::arange(6) + 10.0)
            .astype(cytnx::Type.Double)
            .reshape({3, 2})
            .to(cytnx::Device.cuda)
            .permute({1, 0}));
  REQUIRE_FALSE(b.backend.is_contiguous());

  // Every element (i, j) of the {2, 3} GPU tensor `t` equals expected(i, j).
  const auto check_elems = [](const Ten& t, auto expected) {
    REQUIRE(t.backend.device() == cytnx::Device.cuda);
    REQUIRE(t.backend.shape() == std::vector<cytnx::cytnx_uint64>{2, 3});
    const cytnx::Tensor host = t.backend.to(cytnx::Device.cpu);
    for (cytnx::cytnx_uint64 i = 0; i < 2; ++i) {
      for (cytnx::cytnx_uint64 j = 0; j < 3; ++j) {
        CHECK(host.at<cytnx::cytnx_double>({i, j})
              == expected(static_cast<double>(i), static_cast<double>(j)));
      }
    }
  };

  // A transposed later input, and a transposed first input.
  const tcapi::List<tcapi::CRef<Ten>> a_then_b = {std::cref(a), std::cref(b)};
  const tcapi::List<tcapi::CRef<Ten>> b_then_a = {std::cref(b), std::cref(a)};
  for (const auto* ins : {&a_then_b, &b_then_a}) {
    check_elems(tcapi::linear_combine<Ten>(ctx, *ins),
                [](double i, double j) { return 4.0 * i + 3.0 * j + 10.0; });
  }
  check_elems(tcapi::linear_combine<Ten>(ctx, a_then_b, {0.5, 2.0}),
              [](double i, double j) { return 0.5 * (3.0 * i + j) + 2.0 * (10.0 + 2.0 * j + i); });
  check_elems(tcapi::linear_combine<Ten>(ctx, b_then_a, {2.0, 0.5}),
              [](double i, double j) { return 2.0 * (10.0 + 2.0 * j + i) + 0.5 * (3.0 * i + j); });

  tcapi::destroy_context(ctx);
}
