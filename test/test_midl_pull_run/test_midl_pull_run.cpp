// instruments#113: a MIDL records pull opens its blanked-backlight window at the
// first flash write, never before a network fetch (include/midl_pull_run.h).

#include <unity.h>

#include <string>
#include <vector>

#include "midl_pull_run.h"

static std::vector<std::string> ev;

struct FakeGuard {
    bool armed = false;
    int arms = 0;
    void arm() {
        if (armed) return;
        armed = true;
        ++arms;
        ev.push_back("arm");
    }
};

void setUp(void) {
    ev.clear();
}
void tearDown(void) {
}

// fetch i: network first; `ok` = the record came back and is stored.
static auto fetcher(std::vector<bool> ok) {
    return [ok](uint8_t i, FakeGuard &g) {
        ev.push_back("net" + std::to_string(i));
        if (!ok[i]) return false;
        g.arm();
        ev.push_back("write" + std::to_string(i));
        return true;
    };
}
static auto remover() {
    return [](uint8_t i) { ev.push_back("rm" + std::to_string(i)); };
}
static auto indexer() {
    return [] { ev.push_back("index"); };
}

static void test_failed_fetches_never_blank() {
    FakeGuard g;
    unsigned n = midl::pull::run(g, 2, fetcher({false, false}), 0, remover(), indexer());
    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_EQUAL_INT(0, g.arms);
}

static void test_window_opens_after_the_first_network_fetch() {
    FakeGuard g;
    unsigned n = midl::pull::run(g, 2, fetcher({true, true}), 0, remover(), indexer());
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_INT(1, g.arms);  // one window for the whole pull
    const std::vector<std::string> want = {"net0", "arm", "write0", "net1", "write1", "index"};
    TEST_ASSERT_TRUE(ev == want);
}

static void test_a_failed_first_fetch_does_not_open_the_window() {
    FakeGuard g;
    midl::pull::run(g, 2, fetcher({false, true}), 0, remover(), indexer());
    const std::vector<std::string> want = {"net0", "net1", "arm", "write1", "index"};
    TEST_ASSERT_TRUE(ev == want);
}

static void test_removals_open_the_window_right_before_deleting() {
    FakeGuard g;
    midl::pull::run(g, 1, fetcher({false}), 2, remover(), indexer());
    const std::vector<std::string> want = {"net0", "arm", "rm0", "rm1", "index"};
    TEST_ASSERT_TRUE(ev == want);
    TEST_ASSERT_EQUAL_INT(1, g.arms);
}

static void test_noop_pull_never_blanks() {
    FakeGuard g;
    midl::pull::run(g, 0, fetcher({}), 0, remover(), indexer());
    TEST_ASSERT_EQUAL_INT(0, g.arms);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_failed_fetches_never_blank);
    RUN_TEST(test_window_opens_after_the_first_network_fetch);
    RUN_TEST(test_a_failed_first_fetch_does_not_open_the_window);
    RUN_TEST(test_removals_open_the_window_right_before_deleting);
    RUN_TEST(test_noop_pull_never_blanks);
    return UNITY_END();
}
