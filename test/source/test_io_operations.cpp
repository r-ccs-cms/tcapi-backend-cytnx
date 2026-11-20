#include <doctest/doctest.h>
#include <tcapi/tcapi.h>

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cytnx.hpp>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <system_error>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#  include <sys/resource.h>
#endif

#include <tcapict/fixture.h>

#include "message_check.h"
#include "sample_tensor.h"

TEST_CASE("tcapi::save file creation verification") {
  tcapi::context_handle_t<tcapi::CytnxTensor<cytnx::cytnx_complex128>> ctx;
  tcapi::create_context(ctx);

  SUBCASE("Save creates file on disk") {
    auto tensor = tcapi::eye<tcapi::CytnxTensor<cytnx::cytnx_complex128>>(ctx, 2);

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
    CHECK_THROWS_WITH(tcapi::load<tcapi::CytnxTensor<cytnx::cytnx_complex128>>(ctx, filepath),
                      doctest::Contains("could not find file"));
  }

  tcapi::destroy_context(ctx);
}

// Cytnx fails to write a default-constructed tensor only after the target file
// has been opened and truncated, so tcapi::save rejects one before opening
// anything.
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

namespace {

  using SaveTensor = tcapi::CytnxTensor<cytnx::cytnx_double>;

  // A fresh, empty directory under the system temporary directory, removed
  // with everything in it when the guard goes out of scope.
  class ScratchDir {
  public:
    explicit ScratchDir(const std::string& name)
        : path_(std::filesystem::temp_directory_path() / name) {
      std::filesystem::remove_all(path_);
      std::filesystem::create_directories(path_);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    ~ScratchDir() {
      std::error_code ec;
      std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
  };

  // Collects what is written to std::cerr while it is in scope.
  class CerrCapture {
  public:
    CerrCapture() : previous_(std::cerr.rdbuf(captured_.rdbuf())) {}
    CerrCapture(const CerrCapture&) = delete;
    CerrCapture& operator=(const CerrCapture&) = delete;
    ~CerrCapture() { std::cerr.rdbuf(previous_); }
    std::string str() const { return captured_.str(); }

  private:
    std::ostringstream captured_;
    std::streambuf* previous_;
  };

  // The names of the entries directly in `dir`, sorted.
  std::vector<std::string> entries(const std::filesystem::path& dir) {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
  }

  std::string file_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }

  void write_file(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
  }

  struct WriteRefused {};

  // A stream buffer that throws WriteRefused on every write. A stream catches
  // it and reports the failure instead, unless its exception mask holds the
  // state bit the failing operation sets.
  struct RefusingBuffer : std::streambuf {
    int_type overflow(int_type) override { throw WriteRefused{}; }
    std::streamsize xsputn(const char*, std::streamsize) override { throw WriteRefused{}; }
  };

  // A context and make_sample's tensor, for the save tests below.
  struct SaveFixture : tcapict::tcapi_test_fixture<SaveTensor> {
    const SaveTensor sample = make_sample<SaveTensor>(ctx);
  };

}  // namespace

// tcapi::load reads exactly the path it is given, so tcapi::save writes exactly
// that path too, whatever its extension, and no other file. The bytes written
// are Cytnx's, which tcapi::save never inspects, so one sample tensor serves
// every path.
TEST_CASE_FIXTURE(SaveFixture, "tcapi::save writes exactly the path it is given") {
  for (const std::string name : {"noext", "x.cytn", "x.ten", "a.b.cytn"}) {
    CAPTURE(name);
    const ScratchDir dir("tcapi_test_save_exact_path");
    const auto path = dir.path() / name;
    tcapi::save(ctx, sample, path);
    CHECK(entries(dir.path()) == std::vector<std::string>{name});
    SaveTensor loaded;
    REQUIRE_NOTHROW(loaded = tcapi::load<SaveTensor>(ctx, path));
    check_real_sample(ctx, loaded);
  }
}

TEST_CASE_FIXTURE(SaveFixture, "tcapi::save creates missing parents and replaces a longer file") {
  const ScratchDir dir("tcapi_test_save_parents_and_replace");

  SUBCASE("missing parent directories") {
    const auto path = dir.path() / "fresh" / "sub" / "x.ten";
    tcapi::save(ctx, sample, path);
    CHECK(entries(path.parent_path()) == std::vector<std::string>{"x.ten"});
    check_real_sample(ctx, tcapi::load<SaveTensor>(ctx, path));
  }

  SUBCASE("an existing file longer than the tensor") {
    const auto path = dir.path() / "x.ten";
    const std::string longer(4096, 'z');
    write_file(path, longer);
    tcapi::save(ctx, sample, path);
    CHECK(file_bytes(path).size() < longer.size());
    check_real_sample(ctx, tcapi::load<SaveTensor>(ctx, path));
  }
}

TEST_CASE_FIXTURE(SaveFixture,
                  "tcapi::save to a path without an extension leaves <path>.cytn alone") {
  const ScratchDir dir("tcapi_test_save_leaves_cytn");
  const auto unrelated = dir.path() / "p.cytn";
  write_file(unrelated, "unrelated");

  tcapi::save(ctx, sample, dir.path() / "p");

  CHECK(file_bytes(unrelated) == "unrelated");
}

TEST_CASE_FIXTURE(SaveFixture, "tcapi::save prints nothing to std::cerr") {
  const ScratchDir dir("tcapi_test_save_no_warning");

  for (const std::string name : {"x.cytn", "noext"}) {
    CAPTURE(name);
    std::string printed;
    {
      const CerrCapture capture;
      tcapi::save(ctx, sample, dir.path() / name);
      printed = capture.str();
    }
    CHECK(printed.empty());
  }
}

TEST_CASE_FIXTURE(SaveFixture, "tcapi::save and tcapi::load round-trip through streams") {
  std::ostringstream out;
  std::string printed;
  {
    const CerrCapture capture;
    tcapi::save(ctx, sample, out);
    printed = capture.str();
  }
  CHECK(printed.empty());

  std::istringstream in(out.str());
  check_real_sample(ctx, tcapi::load<SaveTensor>(ctx, in));
}

TEST_CASE_FIXTURE(SaveFixture, "tcapi::save names a path it cannot open") {
  const ScratchDir dir("tcapi_test_save_cannot_open");

  SUBCASE("a path naming a directory") {
    const auto target = dir.path() / "isdir.ten";
    std::filesystem::create_directory(target);
    const std::string message
        = thrown_message<std::runtime_error>([&] { tcapi::save(ctx, sample, target); });
    check_contains(message, {"could not open file for writing", target.string()});
  }

  SUBCASE("a path whose parent is a regular file") {
    write_file(dir.path() / "file", "not a directory");
    const auto target = dir.path() / "file" / "child.ten";
    const std::string message
        = thrown_message<std::runtime_error>([&] { tcapi::save(ctx, sample, target); });
    check_contains(message, {"failed to create directories", target.string()});
  }

  // The stream save writes through a temporary file; when that cannot be
  // opened, the save created nothing there, so it removes nothing.
  SUBCASE("the temporary file of a stream save") {
    // A directory at the temporary file's path, removed on every exit so that
    // later stream saves can use the path.
    const auto temporary = tcapi::detail::make_temp_cytn_file();
    const ScratchDir blocker(temporary.filename().string());
    REQUIRE(blocker.path() == temporary);
    std::ostringstream out;
    const std::string message
        = thrown_message<std::runtime_error>([&] { tcapi::save(ctx, sample, out); });
    check_contains(message, {"could not open file for writing"});
    CHECK(out.str().empty());
    CHECK(std::filesystem::is_directory(temporary));
  }
}

// A stream that reports a failure as the tensor is copied to it makes the save
// throw, and the temporary file is removed all the same.
TEST_CASE_FIXTURE(SaveFixture, "tcapi::save reports a stream that fails") {
  RefusingBuffer refusing;
  std::ostream out(&refusing);
  const auto temporary = tcapi::detail::make_temp_cytn_file();

  const std::string message
      = thrown_message<std::runtime_error>([&] { tcapi::save(ctx, sample, out); });
  check_contains(message, {"failed while streaming tensor contents"});
  CHECK_FALSE(std::filesystem::exists(temporary));
}

// Cytnx throws while writing a default-constructed tensor; the stream is
// closed all the same, so a caller can remove the partial file, and the error
// is a std::runtime_error naming the file.
TEST_CASE("write_and_close closes its file and names it when Cytnx throws") {
  const ScratchDir dir("tcapi_test_save_write_throws");
  const auto path = dir.path() / "x.ten";
  auto out = tcapi::detail::open_for_save(path);
  const cytnx::Tensor empty;

  const std::string message = thrown_message<std::runtime_error>(
      [&] { tcapi::detail::write_and_close(empty, out, path); });
  check_contains(message, {"failed while writing", path.string()});
  CHECK_FALSE(out.is_open());
}

#if defined(__unix__) || defined(__APPLE__)

namespace {

  // Lowers the process's file size limit to `bytes` and ignores SIGXFSZ, so a
  // write past the limit fails with EFBIG instead of ending the process, and
  // restores both when it goes out of scope. The limit binds every file the
  // process writes, so no assertion may run while it is in scope.
  class FileSizeLimit {
  public:
    explicit FileSizeLimit(rlim_t bytes) {
      if (getrlimit(RLIMIT_FSIZE, &previous_limit_) != 0) {
        return;
      }
      previous_handler_ = std::signal(SIGXFSZ, SIG_IGN);
      rlimit lowered = previous_limit_;
      lowered.rlim_cur = bytes;
      active_ = setrlimit(RLIMIT_FSIZE, &lowered) == 0;
    }
    FileSizeLimit(const FileSizeLimit&) = delete;
    FileSizeLimit& operator=(const FileSizeLimit&) = delete;
    ~FileSizeLimit() {
      if (active_) {
        setrlimit(RLIMIT_FSIZE, &previous_limit_);
      }
      if (previous_handler_ != SIG_ERR) {
        std::signal(SIGXFSZ, previous_handler_);
      }
    }
    bool active() const { return active_; }

  private:
    rlimit previous_limit_{};
    void (*previous_handler_)(int) = SIG_ERR;
    bool active_ = false;
  };

}  // namespace

// A save whose write fails partway reports it, naming the file, and a stream
// save removes the partial temporary file and passes nothing on.
TEST_CASE_FIXTURE(SaveFixture, "tcapi::save names a path it could not fully write") {
  const ScratchDir dir("tcapi_test_save_write_failure");
  const auto target = dir.path() / "x.ten";
  const auto temporary = tcapi::detail::make_temp_cytn_file();
  std::ostringstream out;

  bool limited = false;
  std::string to_path;
  std::string to_stream;
  {
    // Smaller than any saved tensor, whose header alone is longer.
    const FileSizeLimit limit(16);
    limited = limit.active();
    to_path = thrown_message<std::runtime_error>([&] { tcapi::save(ctx, sample, target); });
    to_stream = thrown_message<std::runtime_error>([&] { tcapi::save(ctx, sample, out); });
  }

  REQUIRE(limited);
  check_contains(to_path, {"failed while writing", target.string()});
  check_contains(to_stream, {"failed while writing"});
  CHECK(out.str().empty());
  CHECK_FALSE(std::filesystem::exists(temporary));
}

// An exception other than cytnx::error passes through unchanged, and the file
// is closed. Writes go to a RefusingBuffer, and badbit in the exception mask
// makes std::ostream rethrow its WriteRefused rather than swallow it. The file
// buffer still holds a byte that the zero size limit keeps from being flushed,
// so the close fails, and with the mask still set would throw in place of
// WriteRefused.
TEST_CASE("write_and_close passes other exceptions through") {
  const ScratchDir dir("tcapi_test_save_write_other_exception");
  const auto path = dir.path() / "x.ten";
  const cytnx::Tensor tensor = cytnx::zeros({2}, cytnx::Type.Double);
  RefusingBuffer refusing;
  auto out = tcapi::detail::open_for_save(path);
  out << 'x';
  static_cast<std::ios&>(out).rdbuf(&refusing);
  out.exceptions(std::ios::badbit);

  bool limited = false;
  bool passed_through = false;
  {
    const FileSizeLimit limit(0);
    limited = limit.active();
    try {
      tcapi::detail::write_and_close(tensor, out, path);
    } catch (const WriteRefused&) {
      passed_through = true;
    } catch (...) {
    }
  }

  REQUIRE(limited);
  CHECK(passed_through);
  CHECK_FALSE(out.is_open());
}

#endif
