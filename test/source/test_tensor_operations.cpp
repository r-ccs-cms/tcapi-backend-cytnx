#include <doctest/doctest.h>
#include <tcapi/tcapi.h>

#include <cmath>
#include <cytnx.hpp>
#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <vector>

// FIXME: Trace operations cause AddressSanitizer container-overflow in Cytnx internal code
// This appears to be a bug in the Cytnx library itself (UniTensor::UniTensor constructor)
// Skip this entire test case until the Cytnx library issue is resolved
TEST_CASE("TCAPI Trace Operations" * doctest::skip()) {
  tcapi::context_handle_t<tcapi::CytnxTensor<cytnx::cytnx_complex128>> ctx;
  tcapi::create_context(ctx);

  SUBCASE("Matrix trace calculation") {
    tcapi::shape_t<tcapi::CytnxTensor<cytnx::cytnx_complex128>> shape = {3, 3};
    tcapi::CytnxTensor<cytnx::cytnx_complex128> matrix;
    tcapi::zeros(ctx, shape, matrix);

    tcapi::set_elem(ctx, matrix, {0, 0}, cytnx::cytnx_complex128(2.0, 0.0));
    tcapi::set_elem(ctx, matrix, {1, 1}, cytnx::cytnx_complex128(3.0, 0.0));
    tcapi::set_elem(ctx, matrix, {2, 2}, cytnx::cytnx_complex128(4.0, 0.0));
    tcapi::set_elem(ctx, matrix, {0, 1}, cytnx::cytnx_complex128(5.0, 0.0));
    tcapi::set_elem(ctx, matrix, {1, 2}, cytnx::cytnx_complex128(6.0, 0.0));

    tcapi::bond_idx_pairs_t<tcapi::CytnxTensor<cytnx::cytnx_complex128>> pairs = {{0, 1}};
    tcapi::trace(ctx, matrix, pairs);

    CHECK(tcapi::order(ctx, matrix) == 0);
    CHECK(std::abs(tcapi::real(tcapi::get_elem(ctx, matrix, {})) - 9.0) < 1e-10);
  }

  tcapi::destroy_context(ctx);
}

TEST_CASE("TCAPI Contract - duplicate label validation") {
  tcapi::context_handle_t<tcapi::CytnxTensor<cytnx::cytnx_complex128>> ctx;
  tcapi::create_context(ctx);

  SUBCASE("Duplicate labels within single tensor should throw") {
    tcapi::shape_t<tcapi::CytnxTensor<cytnx::cytnx_complex128>> shape_a = {3, 3, 4};
    tcapi::shape_t<tcapi::CytnxTensor<cytnx::cytnx_complex128>> shape_b = {5};
    tcapi::CytnxTensor<cytnx::cytnx_complex128> a, b, c;
    tcapi::zeros(ctx, shape_a, a);
    tcapi::zeros(ctx, shape_b, b);

    CHECK_THROWS_AS(tcapi::contract(ctx, a, "iij", b, "k", c, "jk"), std::invalid_argument);

    tcapi::shape_t<tcapi::CytnxTensor<cytnx::cytnx_complex128>> shape_b2 = {5, 5};
    tcapi::CytnxTensor<cytnx::cytnx_complex128> b2;
    tcapi::zeros(ctx, shape_b2, b2);
    CHECK_THROWS_AS(tcapi::contract(ctx, a, "ijk", b2, "kk", c, "ij"), std::invalid_argument);

    tcapi::List<tcapi::bond_label_t<tcapi::CytnxTensor<cytnx::cytnx_complex128>>> bd_labs_a_dup
        = {1, 1, 2};
    tcapi::List<tcapi::bond_label_t<tcapi::CytnxTensor<cytnx::cytnx_complex128>>> bd_labs_b_single
        = {3};
    tcapi::List<tcapi::bond_label_t<tcapi::CytnxTensor<cytnx::cytnx_complex128>>> bd_labs_c_out
        = {2, 3};
    CHECK_THROWS_AS(tcapi::contract(ctx, a, bd_labs_a_dup, b, bd_labs_b_single, c, bd_labs_c_out),
                    std::invalid_argument);
  }

  tcapi::destroy_context(ctx);
}

TEST_CASE("TCAPI Contract - every bond is contracted or kept") {
  using Ten = tcapi::CytnxTensor<cytnx::cytnx_double>;
  using Labels = tcapi::List<tcapi::bond_label_t<Ten>>;
  tcapi::context_handle_t<Ten> ctx;
  tcapi::create_context(ctx);

  const char* const label_count = "contract: each operand needs one label per bond";
  const char* const unaccounted
      = "contract: every bond of each operand must be contracted or named in the output labels";

  const auto rejects = [&](const tcapi::shape_t<Ten>& shape_a, const auto& labs_a,
                           const tcapi::shape_t<Ten>& shape_b, const auto& labs_b,
                           const auto& labs_c, const char* message) {
    auto a = tcapi::zeros<Ten>(ctx, shape_a);
    auto b = tcapi::zeros<Ten>(ctx, shape_b);
    Ten c;
    CHECK_THROWS_WITH_AS(tcapi::contract<Ten>(ctx, a, labs_a, b, labs_b, c, labs_c), message,
                         std::invalid_argument);
  };

  SUBCASE("A label in neither the other operand nor the output") {
    rejects({2, 2}, "ij", {2}, "j", "", unaccounted);
    rejects({1, 2}, "ij", {2}, "j", "", unaccounted);
    rejects({2, 3}, "ij", {3, 4}, "jk", "i", unaccounted);
  }

  SUBCASE("Outer products into an empty output") {
    rejects({2}, "i", {2}, "j", "", unaccounted);
    rejects({}, "", {3}, "j", "", unaccounted);
  }

  SUBCASE("Fewer labels than bonds") {
    rejects({2, 2}, "i", {2}, "i", "", label_count);
    rejects({2, 2}, Labels{7}, {2}, Labels{7}, Labels{}, label_count);
  }

  SUBCASE("More labels than bonds") {
    rejects({2}, "ij", {3}, "k", "jk", label_count);
    rejects({2}, Labels{1, 2}, {3}, Labels{3}, Labels{2, 3}, label_count);
    rejects({2}, "i", {2}, "ik", "", label_count);
  }

  tcapi::destroy_context(ctx);
}

TEST_CASE("TCAPI linear_combine - zeroth-order inputs") {
  using Ten = tcapi::CytnxTensor<cytnx::cytnx_complex128>;
  using Elem = cytnx::cytnx_complex128;
  tcapi::context_handle_t<Ten> ctx;
  tcapi::create_context(ctx);

  auto s = tcapi::fill<Ten>(ctx, {}, Elem(2.0, -1.0));
  auto t = tcapi::fill<Ten>(ctx, {}, Elem(0.5, 3.0));

  SUBCASE("Zeroth-order inputs combine into a zeroth-order result") {
    auto sum = tcapi::linear_combine<Ten>(ctx, {std::cref(s), std::cref(t)});
    CHECK(tcapi::order(ctx, sum) == 0);
    CHECK(tcapi::get_elem(ctx, sum, {}) == Elem(2.5, 2.0));

    auto weighted = tcapi::linear_combine<Ten>(ctx, {std::cref(s), std::cref(t)},
                                               {Elem(0.0, 1.0), Elem(2.0)});
    CHECK(tcapi::order(ctx, weighted) == 0);
    CHECK(tcapi::get_elem(ctx, weighted, {}) == Elem(0.0, 1.0) * Elem(2.0, -1.0) + Elem(1.0, 6.0));
  }

  SUBCASE("A zeroth-order input beside another shape or a default-constructed tensor is rejected") {
    auto one = tcapi::fill<Ten>(ctx, {1}, Elem(2.0, -1.0));
    auto three = tcapi::fill<Ten>(ctx, {3}, Elem(1.0));
    Ten empty;
    CHECK_THROWS_AS(tcapi::linear_combine<Ten>(ctx, {std::cref(s), std::cref(one)}),
                    std::invalid_argument);
    CHECK_THROWS_AS(tcapi::linear_combine<Ten>(ctx, {std::cref(three), std::cref(s)}),
                    std::invalid_argument);
    CHECK_THROWS_AS(
        tcapi::linear_combine<Ten>(ctx, {std::cref(s), std::cref(empty)}, {Elem(1.0), Elem(1.0)}),
        std::invalid_argument);
  }

  tcapi::destroy_context(ctx);
}

// linear_combine accumulates into one buffer it owns. These cases pin what that
// must not change: no input is written, the result shares storage with none of
// them, a multi-input result is contiguous whatever ins[0]'s layout, and inputs
// of different shapes are rejected rather than broadcast. They also pin the
// rejection of a coefficient count other than the input count and of a
// default-constructed input.
TEST_CASE("TCAPI linear_combine - accumulation into an owned buffer") {
  using Ten = tcapi::CytnxTensor<cytnx::cytnx_double>;
  using Coor = tcapi::elem_coor_t<Ten>;
  using Coors = tcapi::elem_coors_t<Ten>;
  using Coefs = std::vector<tcapi::elem_t<Ten>>;
  tcapi::context_handle_t<Ten> ctx;
  tcapi::create_context(ctx);

  // Element (i, j) of a {2, 3} input built from `base`.
  const auto ramp_value = [](double base, Coor i, Coor j) { return base + 3.0 * i + j; };
  // ramp_value(base, i, j) as a function of (i, j).
  const auto ramp_at
      = [=](double base) { return [=](Coor i, Coor j) { return ramp_value(base, i, j); }; };
  // Calls f(i, j) for every coordinate of a {2, 3} tensor.
  const auto each_coor = [](auto f) {
    for (Coor i = 0; i < 2; ++i) {
      for (Coor j = 0; j < 3; ++j) f(i, j);
    }
  };
  // A contiguous {2, 3} tensor holding ramp_value(base, i, j).
  const auto ramp = [&](double base) {
    auto t = tcapi::zeros<Ten>(ctx, {2, 3});
    each_coor(
        [&](Coor i, Coor j) { tcapi::set_elem(ctx, t, Coors{i, j}, ramp_value(base, i, j)); });
    return t;
  };
  // The same values, as a transpose of a {3, 2} tensor, so the backend is non-contiguous.
  const auto transposed_ramp = [&](double base) {
    auto t = tcapi::zeros<Ten>(ctx, {3, 2});
    each_coor(
        [&](Coor i, Coor j) { tcapi::set_elem(ctx, t, Coors{j, i}, ramp_value(base, i, j)); });
    tcapi::transpose(ctx, t, {1, 0});
    return t;
  };
  // Every element of the {2, 3} tensor `t` equals expected(i, j).
  const auto check_elems = [&](const Ten& t, auto expected) {
    REQUIRE(tcapi::shape(ctx, t) == tcapi::shape_t<Ten>{2, 3});
    each_coor(
        [&](Coor i, Coor j) { CHECK(tcapi::get_elem(ctx, t, Coors{i, j}) == expected(i, j)); });
  };
  const auto check_ramp = [&](const Ten& t, double base) { check_elems(t, ramp_at(base)); };
  // `result` shares storage with none of `inputs`.
  const auto check_unshared = [](const Ten& result, std::initializer_list<const Ten*> inputs) {
    for (const Ten* in : inputs) CHECK_FALSE(result.backend.same_data(in->backend));
  };
  // Both overloads reject `ins`, the weighted one given one coefficient per input.
  const auto rejects_inputs = [&](const tcapi::List<tcapi::CRef<Ten>>& ins) {
    CHECK_THROWS_AS(tcapi::linear_combine<Ten>(ctx, ins), std::invalid_argument);
    CHECK_THROWS_AS(tcapi::linear_combine<Ten>(ctx, ins, Coefs(ins.size(), 1.0)),
                    std::invalid_argument);
  };
  // A multi-input result: unshared, and contiguous.
  const auto check_owned = [&](const Ten& result, std::initializer_list<const Ten*> inputs) {
    check_unshared(result, inputs);
    CHECK(result.backend.is_contiguous());
  };

  SUBCASE("Repeated and distinct contiguous inputs") {
    auto a = ramp(1.0);
    auto b = ramp(10.0);
    const auto av = ramp_at(1.0);
    const auto bv = ramp_at(10.0);

    auto one = tcapi::linear_combine<Ten>(ctx, {std::cref(a)});
    check_elems(one, av);
    auto pair = tcapi::linear_combine<Ten>(ctx, {std::cref(a), std::cref(b)});
    check_elems(pair, [&](Coor i, Coor j) { return av(i, j) + bv(i, j); });
    auto triple = tcapi::linear_combine<Ten>(ctx, {std::cref(a), std::cref(a), std::cref(a)});
    check_elems(triple, [&](Coor i, Coor j) { return 3.0 * av(i, j); });
    auto mixed = tcapi::linear_combine<Ten>(ctx, {std::cref(a), std::cref(b), std::cref(a)});
    check_elems(mixed, [&](Coor i, Coor j) { return 2.0 * av(i, j) + bv(i, j); });
    auto scaled = tcapi::linear_combine<Ten>(ctx, {std::cref(a)}, Coefs{0.5});
    check_elems(scaled, [&](Coor i, Coor j) { return 0.5 * av(i, j); });
    auto weighted = tcapi::linear_combine<Ten>(ctx, {std::cref(a), std::cref(b), std::cref(a)},
                                               Coefs{0.5, -2.0, 0.25});
    check_elems(weighted, [&](Coor i, Coor j) { return 0.75 * av(i, j) - 2.0 * bv(i, j); });

    check_unshared(one, {&a});
    check_unshared(scaled, {&a});
    for (const Ten* result : {&pair, &triple, &mixed, &weighted}) check_owned(*result, {&a, &b});
    // Writing to a result leaves the inputs as they were.
    tcapi::set_elem(ctx, triple, Coors{0, 0}, -100.0);
    check_ramp(a, 1.0);
    check_ramp(b, 10.0);
  }

  SUBCASE("A transposed first input") {
    auto p = transposed_ramp(1.0);
    auto b = ramp(10.0);
    REQUIRE_FALSE(p.backend.is_contiguous());
    const auto pv = ramp_at(1.0);
    const auto bv = ramp_at(10.0);

    auto pair = tcapi::linear_combine<Ten>(ctx, {std::cref(p), std::cref(b)});
    check_elems(pair, [&](Coor i, Coor j) { return pv(i, j) + bv(i, j); });
    check_owned(pair, {&p, &b});
    auto triple = tcapi::linear_combine<Ten>(ctx, {std::cref(p), std::cref(b), std::cref(p)});
    check_elems(triple, [&](Coor i, Coor j) { return 2.0 * pv(i, j) + bv(i, j); });
    check_owned(triple, {&p, &b});
    auto weighted = tcapi::linear_combine<Ten>(ctx, {std::cref(p), std::cref(b)}, Coefs{0.5, 2.0});
    check_elems(weighted, [&](Coor i, Coor j) { return 0.5 * pv(i, j) + 2.0 * bv(i, j); });
    check_owned(weighted, {&p, &b});
    auto weighted_triple = tcapi::linear_combine<Ten>(
        ctx, {std::cref(p), std::cref(b), std::cref(p)}, Coefs{0.5, 2.0, -1.0});
    check_elems(weighted_triple, [&](Coor i, Coor j) { return -0.5 * pv(i, j) + 2.0 * bv(i, j); });
    check_owned(weighted_triple, {&p, &b});

    auto one = tcapi::linear_combine<Ten>(ctx, {std::cref(p)});
    check_elems(one, pv);
    auto scaled = tcapi::linear_combine<Ten>(ctx, {std::cref(p)}, Coefs{2.0});
    check_elems(scaled, [&](Coor i, Coor j) { return 2.0 * pv(i, j); });
    check_unshared(one, {&p});
    check_unshared(scaled, {&p});

    check_ramp(p, 1.0);
    check_ramp(b, 10.0);
  }

  SUBCASE("A transposed later input") {
    auto a = ramp(1.0);
    auto q = transposed_ramp(10.0);
    REQUIRE_FALSE(q.backend.is_contiguous());
    const auto av = ramp_at(1.0);
    const auto qv = ramp_at(10.0);

    auto pair = tcapi::linear_combine<Ten>(ctx, {std::cref(a), std::cref(q)});
    check_elems(pair, [&](Coor i, Coor j) { return av(i, j) + qv(i, j); });
    auto weighted = tcapi::linear_combine<Ten>(ctx, {std::cref(a), std::cref(q)}, Coefs{-1.0, 0.5});
    check_elems(weighted, [&](Coor i, Coor j) { return -av(i, j) + 0.5 * qv(i, j); });
    check_owned(pair, {&a, &q});
    check_owned(weighted, {&a, &q});

    check_ramp(a, 1.0);
    check_ramp(q, 10.0);
  }

  SUBCASE("A zero-size transposed first input") {
    auto z = tcapi::zeros<Ten>(ctx, {0, 3});
    tcapi::transpose(ctx, z, {1, 0});
    REQUIRE_FALSE(z.backend.is_contiguous());
    auto sum = tcapi::linear_combine<Ten>(ctx, {std::cref(z), std::cref(z)});
    CHECK(tcapi::shape(ctx, sum) == tcapi::shape_t<Ten>{3, 0});
    check_owned(sum, {&z});
    auto weighted = tcapi::linear_combine<Ten>(ctx, {std::cref(z), std::cref(z)}, Coefs{1.0, 2.0});
    CHECK(tcapi::shape(ctx, weighted) == tcapi::shape_t<Ten>{3, 0});
    check_owned(weighted, {&z});
  }

  SUBCASE("Inputs of different shapes are rejected, in either order") {
    const auto rejects
        = [&](const tcapi::shape_t<Ten>& shape_x, const tcapi::shape_t<Ten>& shape_y) {
            auto x = tcapi::zeros<Ten>(ctx, shape_x);
            auto y = tcapi::zeros<Ten>(ctx, shape_y);
            rejects_inputs({std::cref(x), std::cref(y)});
            rejects_inputs({std::cref(y), std::cref(x)});
          };
    // A one-element input beside another shape; Cytnx's in-place addition broadcasts a one-element
    // right operand.
    rejects({1}, {3});
    rejects({1}, {1, 1});
    rejects({1, 1}, {2, 3});
    rejects({1}, {0});
    // The same element count, and a different rank.
    rejects({2, 3}, {3, 2});
    rejects({2, 3}, {6});

    // The message names the input that differs and both shapes.
    auto x = tcapi::zeros<Ten>(ctx, {2, 3});
    auto y = tcapi::zeros<Ten>(ctx, {3, 2});
    CHECK_THROWS_WITH_AS(
        tcapi::linear_combine<Ten>(ctx, {std::cref(x), std::cref(x), std::cref(y)}),
        "linear_combine: input 2 has shape {3, 2}, but input 0 has shape {2, 3}; every input "
        "must have the same shape",
        std::invalid_argument);
  }

  SUBCASE("A coefficient count other than the input count is rejected") {
    auto a = ramp(1.0);
    auto b = ramp(10.0);
    CHECK_THROWS_AS(tcapi::linear_combine<Ten>(ctx, {std::cref(a), std::cref(b)}, Coefs{1.0}),
                    std::invalid_argument);
    CHECK_THROWS_AS(
        tcapi::linear_combine<Ten>(ctx, {std::cref(a), std::cref(b)}, Coefs{1.0, 1.0, 1.0}),
        std::invalid_argument);
    CHECK_THROWS_AS(tcapi::linear_combine<Ten>(ctx, {}, Coefs{1.0}), std::invalid_argument);
    // No inputs and no coefficients still give a default-constructed tensor.
    CHECK(tcapi::linear_combine<Ten>(ctx, {}, Coefs{}).backend.is_void());
  }

  SUBCASE("Default-constructed inputs") {
    Ten v;
    Ten w;
    auto a = ramp(1.0);
    // A default-constructed input is rejected wherever it appears, alone included.
    rejects_inputs({std::cref(v)});
    rejects_inputs({std::cref(v), std::cref(w)});
    rejects_inputs({std::cref(v), std::cref(a)});
    rejects_inputs({std::cref(a), std::cref(v)});
    // The message names the input.
    CHECK_THROWS_WITH_AS(tcapi::linear_combine<Ten>(ctx, {std::cref(a), std::cref(v)}),
                         "linear_combine: input 1 is a default-constructed tensor",
                         std::invalid_argument);
  }

  tcapi::destroy_context(ctx);
}

TEST_CASE("TCAPI Stack Operations") {
  using Ten = tcapi::CytnxTensor<cytnx::cytnx_complex128>;
  tcapi::context_handle_t<Ten> ctx;
  tcapi::create_context(ctx);

  SUBCASE("Stack three 2x4 tensors along axis 1") {
    tcapi::shape_t<Ten> shape = {2, 4};
    Ten a, b, c;
    tcapi::zeros(ctx, shape, a);
    tcapi::zeros(ctx, shape, b);
    tcapi::zeros(ctx, shape, c);

    tcapi::set_elem(ctx, a, {0, 0}, cytnx::cytnx_complex128(1.0, 0.0));
    tcapi::set_elem(ctx, b, {0, 0}, cytnx::cytnx_complex128(2.0, 0.0));
    tcapi::set_elem(ctx, c, {0, 0}, cytnx::cytnx_complex128(3.0, 0.0));

    auto d = tcapi::stack<Ten>(ctx, {std::cref(a), std::cref(b), std::cref(c)}, 1);

    auto result_shape = tcapi::shape(ctx, d);
    CHECK(result_shape.size() == 3);
    CHECK(result_shape[0] == 2);
    CHECK(result_shape[1] == 3);
    CHECK(result_shape[2] == 4);

    auto el_a = tcapi::get_elem(ctx, a, {0, 0});
    auto el_d_a = tcapi::get_elem(ctx, d, {0, 0, 0});
    CHECK(std::abs(tcapi::real(el_a) - tcapi::real(el_d_a)) < 1e-10);

    auto el_b = tcapi::get_elem(ctx, b, {0, 0});
    auto el_d_b = tcapi::get_elem(ctx, d, {0, 1, 0});
    CHECK(std::abs(tcapi::real(el_b) - tcapi::real(el_d_b)) < 1e-10);
  }

  SUBCASE("Stack two 3x3 tensors along axis 0") {
    tcapi::shape_t<Ten> shape = {3, 3};
    Ten a, b;
    tcapi::zeros(ctx, shape, a);
    tcapi::zeros(ctx, shape, b);

    tcapi::set_elem(ctx, a, {1, 1}, cytnx::cytnx_complex128(5.0, 0.0));
    tcapi::set_elem(ctx, b, {2, 2}, cytnx::cytnx_complex128(6.0, 0.0));

    auto result = tcapi::stack<Ten>(ctx, {std::cref(a), std::cref(b)}, 0);

    auto result_shape = tcapi::shape(ctx, result);
    CHECK(result_shape.size() == 3);
    CHECK(result_shape[0] == 2);
    CHECK(result_shape[1] == 3);
    CHECK(result_shape[2] == 3);

    auto el_a11 = tcapi::get_elem(ctx, result, {0, 1, 1});
    auto el_b22 = tcapi::get_elem(ctx, result, {1, 2, 2});
    CHECK(std::abs(tcapi::real(el_a11) - 5.0) < 1e-10);
    CHECK(std::abs(tcapi::real(el_b22) - 6.0) < 1e-10);
  }

  SUBCASE("Stack along last dimension (axis 2)") {
    tcapi::shape_t<Ten> shape = {2, 3};
    Ten a, b;
    tcapi::zeros(ctx, shape, a);
    tcapi::zeros(ctx, shape, b);

    tcapi::set_elem(ctx, a, {1, 2}, cytnx::cytnx_complex128(7.0, 0.0));
    tcapi::set_elem(ctx, b, {1, 2}, cytnx::cytnx_complex128(8.0, 0.0));

    auto result = tcapi::stack<Ten>(ctx, {std::cref(a), std::cref(b)}, 2);

    auto result_shape = tcapi::shape(ctx, result);
    CHECK(result_shape.size() == 3);
    CHECK(result_shape[0] == 2);
    CHECK(result_shape[1] == 3);
    CHECK(result_shape[2] == 2);

    auto el_a = tcapi::get_elem(ctx, result, {1, 2, 0});
    auto el_b = tcapi::get_elem(ctx, result, {1, 2, 1});
    CHECK(std::abs(tcapi::real(el_a) - 7.0) < 1e-10);
    CHECK(std::abs(tcapi::real(el_b) - 8.0) < 1e-10);
  }

  SUBCASE("Error handling: empty tensor list") {
    tcapi::List<tcapi::CRef<Ten>> empty_tensors = {};
    CHECK_THROWS_AS(tcapi::stack(ctx, empty_tensors, 0), std::invalid_argument);
  }

  SUBCASE("Error handling: mismatched tensor shapes") {
    Ten a, b;
    tcapi::zeros(ctx, {2, 3}, a);
    tcapi::zeros(ctx, {2, 4}, b);

    CHECK_THROWS_AS(tcapi::stack<Ten>(ctx, {std::cref(a), std::cref(b)}, 0), std::invalid_argument);
  }

  tcapi::destroy_context(ctx);
}
