#pragma once
// A test harness small enough to not be a dependency. Registration happens at
// static-init time; main() runs everything and reports.

#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

std::vector<TestCase>& registry();
void fail(const std::string& file, int line, const std::string& what);
int runAll();
// Only the tests whose name contains one of the patterns. `asr_tests harvest`
// runs the harvest tests and nothing else.
int runAll(const std::vector<std::string>& patterns);

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

} // namespace testing

#define TEST(name)                                                            \
    static void name();                                                       \
    static ::testing::Registrar registrar_##name(#name, name);                \
    static void name()

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) ::testing::fail(__FILE__, __LINE__, "CHECK failed: " #cond); \
    } while (0)

#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        auto _a = (a);                                                        \
        auto _b = (b);                                                        \
        if (!(_a == _b))                                                      \
            ::testing::fail(__FILE__, __LINE__,                               \
                            "CHECK_EQ failed: " #a " != " #b);                \
    } while (0)
