// A web password generated on a standalone display (include/web_pw_new.h).

#include <unity.h>

#include <string.h>

#include "web_pw_new.h"

using web_auth_policy::State;
using namespace web_pw_new;

void setUp(void) {
}
void tearDown(void) {
}

// Deterministic xorshift32 so the generator tests are repeatable.
static uint32_t s_x = 0x12345678u;
static uint32_t xorshift() {
    s_x ^= s_x << 13;
    s_x ^= s_x >> 17;
    s_x ^= s_x << 5;
    return s_x;
}

// Scripted RNG for the rejection-sampling test.
static const uint32_t *s_seq = nullptr;
static size_t s_seq_i = 0;
static uint32_t scripted() {
    return s_seq[s_seq_i++];
}

static void test_alphabet_is_unambiguous() {
    TEST_ASSERT_EQUAL_UINT32(56, ALPHABET_LEN);
    for (const char *bad = "0Oo1lI"; *bad; ++bad)
        TEST_ASSERT_NULL(memchr(ALPHABET, *bad, ALPHABET_LEN));
    // No duplicates (a duplicate would bias the draw).
    for (size_t i = 0; i < ALPHABET_LEN; ++i)
        TEST_ASSERT_NULL(memchr(ALPHABET + i + 1, ALPHABET[i], ALPHABET_LEN - i - 1));
}

static void test_generate_length_and_charset() {
    char pw[BUF_LEN];
    for (int k = 0; k < 200; ++k) {
        TEST_ASSERT_TRUE(generate(xorshift, pw, sizeof(pw)));
        TEST_ASSERT_EQUAL_UINT32(LENGTH, strlen(pw));
        TEST_ASSERT_TRUE(well_formed(pw));
    }
}

static void test_generate_differs_between_calls() {
    char a[BUF_LEN], b[BUF_LEN];
    TEST_ASSERT_TRUE(generate(xorshift, a, sizeof(a)));
    TEST_ASSERT_TRUE(generate(xorshift, b, sizeof(b)));
    TEST_ASSERT_TRUE(strcmp(a, b) != 0);
}

static void test_generate_rejects_biased_tail() {
    // 0xFFFFFFFF is above the largest multiple of 56 and must be redrawn;
    // 0 -> 'A', 54 -> '8'.
    static const uint32_t seq[] = {0xFFFFFFFFu, 0, 54, 0xFFFFFFFFu, 0xFFFFFFFFu, 1, 2, 3,
                                   4,           5, 6,  7,           8,           9, 10};
    s_seq = seq;
    s_seq_i = 0;
    char pw[BUF_LEN];
    TEST_ASSERT_TRUE(generate(scripted, pw, sizeof(pw)));
    TEST_ASSERT_EQUAL_STRING("A8BCDEFGHJKL", pw);
    TEST_ASSERT_EQUAL_UINT32(sizeof(seq) / sizeof(seq[0]), s_seq_i);
}

static void test_generate_bad_args() {
    char small[LENGTH];  // no room for the NUL
    memset(small, 'x', sizeof(small));
    TEST_ASSERT_FALSE(generate(xorshift, small, sizeof(small)));
    TEST_ASSERT_EQUAL_CHAR('\0', small[0]);
    char pw[BUF_LEN];
    TEST_ASSERT_FALSE(generate(nullptr, pw, sizeof(pw)));
    TEST_ASSERT_FALSE(generate(xorshift, nullptr, 0));
}

static void test_well_formed() {
    TEST_ASSERT_TRUE(well_formed("ABCDEFGHJKLM"));
    TEST_ASSERT_FALSE(well_formed("ABCDEFGHJKL"));    // short
    TEST_ASSERT_FALSE(well_formed("ABCDEFGHJKLMN"));  // long
    TEST_ASSERT_FALSE(well_formed("ABCDEFGHJKL0"));   // ambiguous char
    TEST_ASSERT_FALSE(well_formed(nullptr));
}

static void test_offer_gating() {
    // Managed: never offered, in any auth state (DM keeps the plaintext).
    TEST_ASSERT_FALSE(offer(true, State::Enforced).available);
    TEST_ASSERT_FALSE(offer(true, State::SetupOpen).available);
    TEST_ASSERT_FALSE(offer(true, State::DisabledByOwner).available);
    // Standalone, open: offered, and it turns login on.
    Offer o = offer(false, State::SetupOpen);
    TEST_ASSERT_TRUE(o.available);
    TEST_ASSERT_TRUE(o.turns_auth_on);
    // Standalone, already enforced: replaces the password.
    o = offer(false, State::Enforced);
    TEST_ASSERT_TRUE(o.available);
    TEST_ASSERT_FALSE(o.turns_auth_on);
    // Standalone, auth switched off: offered, and it turns login back on.
    o = offer(false, State::DisabledByOwner);
    TEST_ASSERT_TRUE(o.available);
    TEST_ASSERT_TRUE(o.turns_auth_on);
}

static void test_console_only() {
    TEST_ASSERT_TRUE(is_console_only("web-pass-new"));
    TEST_ASSERT_TRUE(is_console_only("  web-pass-new"));
    TEST_ASSERT_TRUE(is_console_only("web-pass-new now"));
    TEST_ASSERT_FALSE(is_console_only("web-pass"));
    TEST_ASSERT_FALSE(is_console_only("web-pass-newer"));
    TEST_ASSERT_FALSE(is_console_only("screen next"));
    TEST_ASSERT_FALSE(is_console_only(nullptr));
}

static void test_confirm_window() {
    Confirm c;
    TEST_ASSERT_FALSE(c.armed(1000));
    TEST_ASSERT_TRUE(c.press(1000) == Confirm::Press::Armed);
    TEST_ASSERT_TRUE(c.armed(1000 + CONFIRM_MS - 1));
    TEST_ASSERT_TRUE(c.press(1000 + CONFIRM_MS - 1) == Confirm::Press::Fire);
    // Fired: disarmed, the next press arms again.
    TEST_ASSERT_FALSE(c.armed(1000 + CONFIRM_MS));
    TEST_ASSERT_TRUE(c.press(20000) == Confirm::Press::Armed);
    // Too late: the window lapsed, so this press re-arms instead of firing.
    TEST_ASSERT_FALSE(c.armed(20000 + CONFIRM_MS));
    TEST_ASSERT_TRUE(c.press(20000 + CONFIRM_MS) == Confirm::Press::Armed);
    c.reset();
    TEST_ASSERT_FALSE(c.armed(20000 + CONFIRM_MS + 1));
}

static void test_confirm_survives_millis_wrap() {
    Confirm c;
    TEST_ASSERT_TRUE(c.press(0xFFFFF000u) == Confirm::Press::Armed);
    TEST_ASSERT_TRUE(c.press(0x00000200u) == Confirm::Press::Fire);  // 4.6 s later, wrapped
}

static void test_reveal_expires_and_wipes() {
    Reveal r;
    TEST_ASSERT_FALSE(r.visible(0));
    r.show("ABCDEFGHJKLM", 5000);
    TEST_ASSERT_TRUE(r.visible(5000));
    TEST_ASSERT_EQUAL_STRING("ABCDEFGHJKLM", r.password());
    TEST_ASSERT_EQUAL_UINT32(30, r.seconds_left(5000));
    TEST_ASSERT_EQUAL_UINT32(1, r.seconds_left(5000 + REVEAL_MS - 1));
    TEST_ASSERT_FALSE(r.expire(5000 + REVEAL_MS - 1));
    TEST_ASSERT_TRUE(r.expire(5000 + REVEAL_MS));
    TEST_ASSERT_FALSE(r.visible(5000 + REVEAL_MS));
    TEST_ASSERT_EQUAL_UINT32(0, r.seconds_left(5000 + REVEAL_MS));
    const char *p = r.password();
    for (size_t i = 0; i < BUF_LEN; ++i)
        TEST_ASSERT_EQUAL_CHAR('\0', p[i]);
    TEST_ASSERT_FALSE(r.expire(5000 + REVEAL_MS + 1));  // only once
}

static void test_reveal_hide_early_wipes() {
    Reveal r;
    r.show("ABCDEFGHJKLM", 0);
    r.hide();
    TEST_ASSERT_FALSE(r.visible(1));
    TEST_ASSERT_EQUAL_STRING("", r.password());
}

static void test_wipe() {
    char buf[8] = "secret!";
    wipe(buf, sizeof(buf));
    for (char c : buf)
        TEST_ASSERT_EQUAL_CHAR('\0', c);
    wipe(nullptr, 4);  // no-op
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_alphabet_is_unambiguous);
    RUN_TEST(test_generate_length_and_charset);
    RUN_TEST(test_generate_differs_between_calls);
    RUN_TEST(test_generate_rejects_biased_tail);
    RUN_TEST(test_generate_bad_args);
    RUN_TEST(test_well_formed);
    RUN_TEST(test_offer_gating);
    RUN_TEST(test_console_only);
    RUN_TEST(test_confirm_window);
    RUN_TEST(test_confirm_survives_millis_wrap);
    RUN_TEST(test_reveal_expires_and_wipes);
    RUN_TEST(test_reveal_hide_early_wipes);
    RUN_TEST(test_wipe);
    return UNITY_END();
}
