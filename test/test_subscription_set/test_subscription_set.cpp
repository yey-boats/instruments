#include "subscription_set.h"

#include <stdio.h>
#include <string.h>
#include <unity.h>

void setUp() {
}
void tearDown() {
}

// --- basic add / has / clear / size --------------------------------------

static void test_empty() {
    sk::SubscriptionSet s;
    TEST_ASSERT_EQUAL_INT(0, s.size());
    TEST_ASSERT_FALSE(s.has("navigation.position"));
    TEST_ASSERT_NULL(s.at(0));
}

static void test_add_then_has() {
    sk::SubscriptionSet s;
    TEST_ASSERT_TRUE(s.add("navigation.speedOverGround"));
    TEST_ASSERT_TRUE(s.has("navigation.speedOverGround"));
    TEST_ASSERT_EQUAL_INT(1, s.size());
    TEST_ASSERT_EQUAL_STRING("navigation.speedOverGround", s.at(0));
}

static void test_clear() {
    sk::SubscriptionSet s;
    s.add("a");
    s.add("b");
    TEST_ASSERT_EQUAL_INT(2, s.size());
    s.clear();
    TEST_ASSERT_EQUAL_INT(0, s.size());
    TEST_ASSERT_FALSE(s.has("a"));
}

static void test_reject_null_and_empty() {
    sk::SubscriptionSet s;
    TEST_ASSERT_FALSE(s.add(nullptr));
    TEST_ASSERT_FALSE(s.add(""));
    TEST_ASSERT_EQUAL_INT(0, s.size());
}

// --- dedup ---------------------------------------------------------------

static void test_dedup() {
    sk::SubscriptionSet s;
    TEST_ASSERT_TRUE(s.add("environment.depth.belowTransducer"));
    TEST_ASSERT_TRUE(s.add("environment.depth.belowTransducer"));  // no-op, returns true
    TEST_ASSERT_EQUAL_INT(1, s.size());
}

// --- full-capacity rejection --------------------------------------------

static void test_full_capacity_rejection() {
    sk::SubscriptionSet s;
    char buf[16];
    for (int i = 0; i < sk::SubscriptionSet::CAP; ++i) {
        snprintf(buf, sizeof(buf), "p%d", i);
        TEST_ASSERT_TRUE(s.add(buf));
    }
    TEST_ASSERT_EQUAL_INT(sk::SubscriptionSet::CAP, s.size());
    TEST_ASSERT_TRUE(s.full());
    // A NEW path is rejected when full...
    TEST_ASSERT_FALSE(s.add("overflow"));
    TEST_ASSERT_EQUAL_INT(sk::SubscriptionSet::CAP, s.size());
    // ...but an EXISTING path still "succeeds" (dedup no-op) even when full.
    TEST_ASSERT_TRUE(s.add("p0"));
    TEST_ASSERT_EQUAL_INT(sk::SubscriptionSet::CAP, s.size());
}

// --- diff ----------------------------------------------------------------

static void test_diff_basic() {
    // desired = {A, B, C}; active = {B, C, D}
    //   toAdd    = desired - active = {A}
    //   toRemove = active - desired = {D}
    //   B, C shared -> in neither.
    sk::SubscriptionSet desired, active, toAdd, toRemove;
    desired.add("A");
    desired.add("B");
    desired.add("C");
    active.add("B");
    active.add("C");
    active.add("D");

    sk::diff(desired, active, toAdd, toRemove);

    TEST_ASSERT_EQUAL_INT(1, toAdd.size());
    TEST_ASSERT_TRUE(toAdd.has("A"));
    TEST_ASSERT_FALSE(toAdd.has("B"));
    TEST_ASSERT_FALSE(toAdd.has("C"));

    TEST_ASSERT_EQUAL_INT(1, toRemove.size());
    TEST_ASSERT_TRUE(toRemove.has("D"));
    TEST_ASSERT_FALSE(toRemove.has("B"));
    TEST_ASSERT_FALSE(toRemove.has("C"));
}

static void test_diff_clears_outputs_first() {
    sk::SubscriptionSet desired, active, toAdd, toRemove;
    toAdd.add("stale");
    toRemove.add("stale");
    desired.add("X");  // active empty -> toAdd={X}, toRemove={}
    sk::diff(desired, active, toAdd, toRemove);
    TEST_ASSERT_EQUAL_INT(1, toAdd.size());
    TEST_ASSERT_TRUE(toAdd.has("X"));
    TEST_ASSERT_FALSE(toAdd.has("stale"));
    TEST_ASSERT_EQUAL_INT(0, toRemove.size());
}

static void test_diff_identical_sets_no_churn() {
    sk::SubscriptionSet desired, active, toAdd, toRemove;
    desired.add("A");
    desired.add("B");
    active.add("A");
    active.add("B");
    sk::diff(desired, active, toAdd, toRemove);
    TEST_ASSERT_EQUAL_INT(0, toAdd.size());
    TEST_ASSERT_EQUAL_INT(0, toRemove.size());
}

static void test_diff_all_new_all_removed() {
    sk::SubscriptionSet desired, active, toAdd, toRemove;
    desired.add("A");
    desired.add("B");
    active.add("C");
    active.add("D");
    sk::diff(desired, active, toAdd, toRemove);
    TEST_ASSERT_EQUAL_INT(2, toAdd.size());
    TEST_ASSERT_TRUE(toAdd.has("A"));
    TEST_ASSERT_TRUE(toAdd.has("B"));
    TEST_ASSERT_EQUAL_INT(2, toRemove.size());
    TEST_ASSERT_TRUE(toRemove.has("C"));
    TEST_ASSERT_TRUE(toRemove.has("D"));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_empty);
    RUN_TEST(test_add_then_has);
    RUN_TEST(test_clear);
    RUN_TEST(test_reject_null_and_empty);
    RUN_TEST(test_dedup);
    RUN_TEST(test_full_capacity_rejection);
    RUN_TEST(test_diff_basic);
    RUN_TEST(test_diff_clears_outputs_first);
    RUN_TEST(test_diff_identical_sets_no_churn);
    RUN_TEST(test_diff_all_new_all_removed);
    return UNITY_END();
}
