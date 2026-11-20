#pragma once

// Checks on the message of an exception, shared by the backend tests.

#include <doctest/doctest.h>

#include <initializer_list>
#include <string>

// Runs `f` and returns the message of the E it throws, or an empty string
// when it throws none.
template <typename E, typename F> std::string thrown_message(F&& f) {
  try {
    f();
  } catch (const E& e) {
    return e.what();
  }
  return {};
}

// Checks that `message` contains each of `parts`.
inline void check_contains(const std::string& message, std::initializer_list<std::string> parts) {
  CAPTURE(message);
  for (const auto& part : parts) {
    CAPTURE(part);
    CHECK(message.find(part) != std::string::npos);
  }
}
