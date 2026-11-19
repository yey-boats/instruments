// One MIDL records pull's store actions (manager.cpp pull_midl_records), kept
// platform-free so the dark-window rule is host-tested (test/test_midl_pull_run).
//
// A pull fetches changed records over HTTP, writes each to flash, deletes the
// records the manager dropped and writes the index. All of it shares ONE
// blanked-backlight window (#106/#107: several writes would otherwise blink the
// panel between fetches), and that window opens at the first flash write, never
// before a network fetch (#113): a slow, failed or refused fetch keeps the
// screen lit. `fetch(i, blank)` must call blank.arm() right before it writes.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <stdint.h>

namespace midl {
namespace pull {

template <class Guard, class Fetch, class Remove, class Index>
unsigned run(Guard &blank, uint8_t fetch_count, Fetch fetch, uint8_t remove_count, Remove remove,
             Index write_index) {
    unsigned fetched = 0;
    for (uint8_t i = 0; i < fetch_count; ++i)
        if (fetch(i, blank)) ++fetched;  // arms right before its put_record
    if (remove_count > 0) blank.arm();   // the deletes write flash
    for (uint8_t i = 0; i < remove_count; ++i)
        remove(i);
    write_index();
    return fetched;
}

}  // namespace pull
}  // namespace midl
