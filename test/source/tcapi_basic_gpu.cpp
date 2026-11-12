#include <doctest/doctest.h>
#include <tcapi/tcapi.h>

#include <cytnx.hpp>
#include <functional>
#include <stdexcept>

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
  if (cytnx::Device.Ngpus == 0) {
    MESSAGE("Skipping GPU tests - no CUDA devices available");
    return;
  }

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
  if (cytnx::Device.Ngpus == 0) {
    MESSAGE("Skipping GPU tests - no CUDA devices available");
    return;
  }

  using Ten = tcapi::CytnxTensor<cytnx::cytnx_double>;
  tcapi::context_handle_t<Ten> cpu_ctx;
  tcapi::context_handle_t<Ten> gpu_ctx;
  tcapi::create_context(cpu_ctx);
  tcapi::create_context(gpu_ctx, 0);  // GPU 0

  // Zeroth-order inputs: Cytnx's in-place addition accepts a one-element CPU right
  // operand beside a GPU left operand, so only the device check rejects that order.
  auto on_cpu = tcapi::zeros<Ten>(cpu_ctx, {});
  auto on_gpu = tcapi::zeros<Ten>(gpu_ctx, {});
  const tcapi::List<tcapi::CRef<Ten>> gpu_first = {std::cref(on_gpu), std::cref(on_cpu)};
  const tcapi::List<tcapi::CRef<Ten>> cpu_first = {std::cref(on_cpu), std::cref(on_gpu)};
  for (const auto* ins : {&gpu_first, &cpu_first}) {
    CHECK_THROWS_AS(tcapi::linear_combine<Ten>(gpu_ctx, *ins), std::invalid_argument);
    CHECK_THROWS_AS(tcapi::linear_combine<Ten>(gpu_ctx, *ins, {1.0, 1.0}), std::invalid_argument);
  }

  tcapi::destroy_context(gpu_ctx);
  tcapi::destroy_context(cpu_ctx);
}
