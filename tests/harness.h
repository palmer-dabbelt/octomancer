// A test harness small enough to not be a dependency.
#ifndef OCTO_TEST_HARNESS_H
#define OCTO_TEST_HARNESS_H

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace octotest {

inline int& failures() {
  static int n = 0;
  return n;
}

inline void fail(const char* file, int line, const std::string& what) {
  std::fprintf(stderr, "%s:%d: FAIL %s\n", file, line, what.c_str());
  ++failures();
}

inline int report(const char* name) {
  if (failures() == 0) {
    std::fprintf(stderr, "%s: ok\n", name);
    return 0;
  }
  std::fprintf(stderr, "%s: %d failure(s)\n", name, failures());
  return 1;
}

}  // namespace octotest

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) octotest::fail(__FILE__, __LINE__, "CHECK(" #cond ")");  \
  } while (0)

#define CHECK_EQ(a, b)                                                    \
  do {                                                                    \
    auto va_ = (a);                                                       \
    auto vb_ = (b);                                                       \
    if (!(va_ == vb_)) {                                                  \
      std::string m = std::string(#a) + " == " #b;                        \
      octotest::fail(__FILE__, __LINE__, m);                              \
    }                                                                     \
  } while (0)

#define CHECK_STR(a, b)                                                   \
  do {                                                                    \
    std::string va_ = (a);                                                \
    std::string vb_ = (b);                                                \
    if (va_ != vb_) {                                                     \
      octotest::fail(__FILE__, __LINE__,                                  \
                     std::string(#a) + ": got \"" + va_ +                 \
                         "\" want \"" + vb_ + "\"");                      \
    }                                                                     \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                             \
  do {                                                                    \
    double va_ = (a), vb_ = (b);                                          \
    if (!(std::fabs(va_ - vb_) <= (tol))) {                               \
      char buf_[160];                                                     \
      std::snprintf(buf_, sizeof buf_, "%s: got %.9f want %.9f (tol %g)", \
                    #a, va_, vb_, (double)(tol));                         \
      octotest::fail(__FILE__, __LINE__, buf_);                           \
    }                                                                     \
  } while (0)

#endif  // OCTO_TEST_HARNESS_H
