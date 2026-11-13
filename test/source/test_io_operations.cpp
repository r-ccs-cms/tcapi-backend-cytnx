#include <doctest/doctest.h>
#include <tcapi/tcapi.h>

#include <cmath>
#include <cytnx.hpp>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>

#include "sample_tensor.h"

TEST_CASE("tcapi::save file creation verification") {
  tcapi::context_handle_t<tcapi::CytnxTensor<cytnx::cytnx_complex128>> ctx;
  tcapi::create_context(ctx);

  SUBCASE("Save creates file on disk") {
    tcapi::CytnxTensor<cytnx::cytnx_complex128> tensor;
    tcapi::eye(ctx, 2, tensor);

    std::string filepath = "/tmp/claude/test_tensor.cytnx";
    CHECK_NOTHROW(tcapi::save(ctx, tensor, filepath));
    CHECK(std::filesystem::exists(filepath));
  }

  tcapi::destroy_context(ctx);
}

TEST_CASE("tcapi::load error handling") {
  tcapi::context_handle_t<tcapi::CytnxTensor<cytnx::cytnx_complex128>> ctx;
  tcapi::create_context(ctx);

  SUBCASE("Load nonexistent file throws with message") {
    std::string filepath = "/tmp/claude/nonexistent_tensor.cytnx";
    tcapi::CytnxTensor<cytnx::cytnx_complex128> tensor;
    CHECK_THROWS_WITH(tcapi::load(ctx, filepath, tensor), doctest::Contains("could not find file"));
  }

  SUBCASE("Out-of-place load nonexistent file throws with message") {
    std::string filepath = "/tmp/claude/nonexistent_tensor.cytnx";
    CHECK_THROWS_WITH(tcapi::load<tcapi::CytnxTensor<cytnx::cytnx_complex128>>(ctx, filepath),
                      doctest::Contains("could not find file"));
  }

  tcapi::destroy_context(ctx);
}

// Cytnx fails to save a default-constructed tensor only after truncating the
// target file, so tcapi::save rejects one before opening anything.
TEST_CASE("tcapi::save rejects a default-constructed tensor") {
  using Tensor = tcapi::CytnxTensor<cytnx::cytnx_double>;
  tcapi::context_handle_t<Tensor> ctx;
  tcapi::create_context(ctx);
  const Tensor empty;

  SUBCASE("to a path, leaving an existing file intact") {
    const auto path
        = (std::filesystem::temp_directory_path() / "tcapi_test_save_default_constructed.cytn")
              .string();
    tcapi::save(ctx, make_sample<Tensor>(ctx), path);

    CHECK_THROWS_WITH_AS(tcapi::save(ctx, empty, path), doctest::Contains("default-constructed"),
                         std::runtime_error);

    const Tensor loaded = tcapi::load<Tensor>(ctx, path);
    check_real_sample(ctx, loaded);
    std::filesystem::remove(path);
  }

  SUBCASE("to an output stream, writing nothing") {
    std::ostringstream out;
    CHECK_THROWS_WITH_AS(tcapi::save(ctx, empty, out), doctest::Contains("default-constructed"),
                         std::runtime_error);
    CHECK(out.str().empty());
  }

  tcapi::destroy_context(ctx);
}
