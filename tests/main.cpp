#include "framework.hpp"

namespace testing {

std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

namespace {
int gFailures = 0;
std::string gCurrent;
} // namespace

void fail(const std::string& file, int line, const std::string& what) {
    ++gFailures;
    std::cerr << "  FAIL " << gCurrent << " at " << file << ":" << line << ": " << what << "\n";
}

int runAll() { return runAll({}); }

int runAll(const std::vector<std::string>& patterns) {
    int failed = 0;
    int ran = 0;
    for (auto& t : registry()) {
        // A name filter, because the suite runs a dozen ten-year worlds and
        // takes minutes: checking one behaviour should not cost all of them.
        if (!patterns.empty()) {
            bool wanted = false;
            for (const auto& p : patterns)
                if (t.name.find(p) != std::string::npos) wanted = true;
            if (!wanted) continue;
        }
        ++ran;
        gCurrent = t.name;
        const int before = gFailures;
        t.fn();
        if (gFailures > before) {
            ++failed;
            std::cout << "FAIL " << t.name << "\n";
        } else {
            std::cout << "ok   " << t.name << "\n";
        }
    }
    std::cout << "\n" << (ran - failed) << "/" << ran << " tests passed\n";
    return failed == 0 ? 0 : 1;
}

} // namespace testing

int main(int argc, char** argv) {
    std::vector<std::string> patterns;
    for (int i = 1; i < argc; ++i) patterns.emplace_back(argv[i]);
    return testing::runAll(patterns);
}
