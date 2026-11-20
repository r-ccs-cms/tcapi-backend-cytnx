#include <doctest/doctest.h>
#include <tcapi/tcapi.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cytnx.hpp>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <random>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <system_error>
#include <utility>

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
  // with everything in it when the guard goes out of scope. Its name is `stem`
  // followed by a tag that a per-process counter keeps distinct within this
  // process and std::random_device and the clock make unlikely to recur in
  // another, so that test runs sharing the temporary directory do not remove
  // each other's files. Should the name be taken all the same, the constructor
  // throws, so a guard never removes a directory it did not create.
  class ScratchDir {
  public:
    explicit ScratchDir(const std::string& stem)
        : path_(std::filesystem::temp_directory_path() / (stem + "_" + unique_tag())) {
      if (!std::filesystem::create_directories(path_)) {
        throw std::runtime_error("scratch directory already exists: " + path_.string());
      }
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    ~ScratchDir() {
      std::error_code ec;
      std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }

  private:
    static std::string unique_tag() {
      static std::atomic<unsigned long> counter{0};
      static const auto process_draw = std::random_device{}();
      return std::to_string(process_draw) + "_"
             + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_"
             + std::to_string(counter++);
    }

    std::filesystem::path path_;
  };

  // A directory created at `path` when nothing is there, and removed when the
  // guard goes out of scope if it was created and is still empty; anything
  // already at `path` is left alone.
  class DirectoryAt {
  public:
    explicit DirectoryAt(std::filesystem::path path) : path_(std::move(path)) {
      std::error_code ec;
      created_ = std::filesystem::create_directory(path_, ec);
    }
    DirectoryAt(const DirectoryAt&) = delete;
    DirectoryAt& operator=(const DirectoryAt&) = delete;
    ~DirectoryAt() {
      if (created_) {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
      }
    }
    bool created() const { return created_; }

  private:
    std::filesystem::path path_;
    bool created_ = false;
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

  // Checks that `dir` holds exactly one entry, named `name`.
  void check_only_entry(const std::filesystem::path& dir, const std::string& name) {
    std::size_t count = 0;
    std::string found;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      ++count;
      found = entry.path().filename().string();
    }
    CHECK(count == 1);
    CHECK(found == name);
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

  // A stream buffer that accepts its first `capacity` bytes and refuses the
  // rest, as a full device would.
  class LimitedBuffer : public std::streambuf {
  public:
    explicit LimitedBuffer(std::size_t capacity) : capacity_(capacity) {}
    std::size_t accepted() const { return accepted_; }

  protected:
    int_type overflow(int_type ch) override {
      if (traits_type::eq_int_type(ch, traits_type::eof())) {
        return traits_type::not_eof(ch);
      }
      if (accepted_ == capacity_) {
        return traits_type::eof();
      }
      ++accepted_;
      return ch;
    }

  private:
    std::size_t capacity_;
    std::size_t accepted_ = 0;
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
    check_only_entry(dir.path(), name);
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
    check_only_entry(path.parent_path(), "x.ten");
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

// A tensor larger than the block the stream save copies in reaches the stream
// whole.
TEST_CASE_FIXTURE(SaveFixture, "tcapi::save streams a tensor larger than one copy block") {
  // 20000 doubles, about 160 KB, spanning three 64 KiB blocks.
  const tcapi::elem_coors_t<SaveTensor> last = {19999};
  SaveTensor large = tcapi::zeros<SaveTensor>(ctx, {20000});
  tcapi::set_elem(ctx, large, {0}, 1.5);
  tcapi::set_elem(ctx, large, last, -2.5);

  std::ostringstream out;
  tcapi::save(ctx, large, out);
  std::istringstream in(out.str());
  const SaveTensor loaded = tcapi::load<SaveTensor>(ctx, in);

  CHECK(tcapi::shape(ctx, loaded) == tcapi::shape(ctx, large));
  CHECK(tcapi::get_elem(ctx, loaded, {0}) == 1.5);
  CHECK(tcapi::get_elem(ctx, loaded, last) == -2.5);
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
    // later stream saves can use the path. If something is already there,
    // perhaps another process's save, it is left alone and the subcase fails.
    const auto temporary = tcapi::detail::make_temp_cytn_file();
    const DirectoryAt blocker(temporary);
    REQUIRE(blocker.created());
    std::ostringstream out;
    const std::string message
        = thrown_message<std::runtime_error>([&] { tcapi::save(ctx, sample, out); });
    check_contains(message, {"could not open file for writing", temporary.string()});
    CHECK(out.str().empty());
    CHECK(std::filesystem::is_directory(temporary));
  }
}

// A stream that reports a failure as the tensor is copied to it makes the save
// throw, and the temporary file is removed all the same.
TEST_CASE_FIXTURE(SaveFixture, "tcapi::save reports a stream that fails") {
  const auto temporary = tcapi::detail::make_temp_cytn_file();

  SUBCASE("refusing every byte") {
    RefusingBuffer refusing;
    std::ostream out(&refusing);
    const std::string message
        = thrown_message<std::runtime_error>([&] { tcapi::save(ctx, sample, out); });
    check_contains(message, {"failed while streaming tensor contents"});
  }

  // Inserting a whole stream buffer reports no failure once some bytes went
  // through, so this case catches a copy that relies on that.
  SUBCASE("accepting only the first bytes") {
    LimitedBuffer limited(10);
    std::ostream out(&limited);
    const std::string message
        = thrown_message<std::runtime_error>([&] { tcapi::save(ctx, sample, out); });
    check_contains(message, {"failed while streaming tensor contents"});
    CHECK(limited.accepted() == 10);
  }

  CHECK_FALSE(std::filesystem::exists(temporary));
}

// A saved tensor is never empty, so finding the temporary file empty when it
// is copied to the stream is reported rather than passed on as nothing.
TEST_CASE("copy_file_to_stream rejects an empty temporary file") {
  const ScratchDir dir("tcapi_test_save_empty_temporary");
  const auto path = dir.path() / "empty.cytn";
  write_file(path, "");
  std::ostringstream out;

  const std::string message
      = thrown_message<std::runtime_error>([&] { tcapi::detail::copy_file_to_stream(path, out); });
  check_contains(message, {"read nothing from the temporary file"});
  CHECK(out.str().empty());
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
