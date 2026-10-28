// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What one tab's or site's sessionStorage work can make others wait for in
// MoltNet (components/services/storage/dom_storage/molt_dom_storage_waits.h,
// std-only): a namespace job waits only for the jobs before it that name
// one of its namespaces, and a copy's wait for its sites' turns ends once,
// at its last turn or when its time is up.

#include "components/services/storage/dom_storage/molt_dom_storage_waits.h"

#include <cstdint>
#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace storage {
namespace {

using Ids = std::vector<uint64_t>;

// Another tab's job doesn't wait for a tab's, however many came before it.
TEST(MoltNetDomStorageWaitsTest, JobsOfOtherNamespacesDontWait) {
  MoltDomStorageNamespaceJobs jobs;
  const uint64_t copy = jobs.Add("tab", "popup");
  const uint64_t deletion = jobs.Add("closed", "closed");
  const uint64_t other_copy = jobs.Add("other", "other popup");
  EXPECT_TRUE(jobs.MayRun(copy));
  EXPECT_TRUE(jobs.MayRun(deletion));
  EXPECT_TRUE(jobs.MayRun(other_copy));
  EXPECT_TRUE(jobs.Names("tab"));
  EXPECT_TRUE(jobs.Names("popup"));
  EXPECT_FALSE(jobs.Names("elsewhere"));
  // Done with one: none was waiting for it.
  EXPECT_EQ(jobs.Done(deletion), Ids());
  EXPECT_FALSE(jobs.Names("closed"));
}

// Jobs that share a namespace run one at a time, in the order they came: a
// popup's copy, then the tab's deletion and a copy of the popup, each after
// the copy; and a job that names two waits for both.
TEST(MoltNetDomStorageWaitsTest, JobsOfOneNamespaceKeepTheirOrder) {
  MoltDomStorageNamespaceJobs jobs;
  const uint64_t copy = jobs.Add("tab", "popup");
  const uint64_t tab_goes = jobs.Add("tab", "tab");
  const uint64_t copy_of_popup = jobs.Add("popup", "popup's popup");
  const uint64_t other = jobs.Add("other", "other");
  const uint64_t needs_both = jobs.Add("other", "popup's popup");
  EXPECT_TRUE(jobs.MayRun(copy));
  EXPECT_FALSE(jobs.MayRun(tab_goes));
  EXPECT_FALSE(jobs.MayRun(copy_of_popup));
  EXPECT_TRUE(jobs.MayRun(other));
  EXPECT_FALSE(jobs.MayRun(needs_both));

  EXPECT_EQ(jobs.Done(copy), Ids({tab_goes, copy_of_popup}));
  EXPECT_TRUE(jobs.MayRun(tab_goes));
  EXPECT_TRUE(jobs.MayRun(copy_of_popup));
  // Still behind the copy of the popup (its destination) and `other`.
  EXPECT_FALSE(jobs.MayRun(needs_both));
  EXPECT_EQ(jobs.Done(other), Ids());
  EXPECT_FALSE(jobs.MayRun(needs_both));
  EXPECT_EQ(jobs.Done(copy_of_popup), Ids({needs_both}));
  EXPECT_EQ(jobs.Done(tab_goes), Ids());
  EXPECT_EQ(jobs.Done(needs_both), Ids());
  EXPECT_EQ(jobs.size(), 0u);
  EXPECT_FALSE(jobs.Names("tab"));
  EXPECT_FALSE(jobs.Names("popup's popup"));
}

// A job is reported ready once, whichever of its namespaces frees last; an
// unknown or finished job changes nothing.
TEST(MoltNetDomStorageWaitsTest, AJobIsReadyOnce) {
  MoltDomStorageNamespaceJobs jobs;
  const uint64_t a = jobs.Add("a", "a");
  const uint64_t b = jobs.Add("b", "b");
  const uint64_t both = jobs.Add("a", "b");
  EXPECT_EQ(jobs.Done(a), Ids());
  EXPECT_EQ(jobs.Done(b), Ids({both}));
  EXPECT_EQ(jobs.Done(b), Ids());
  EXPECT_EQ(jobs.Done(12345), Ids());
  EXPECT_FALSE(jobs.MayRun(12345));
  EXPECT_TRUE(jobs.MayRun(both));
  // A deletion naming its namespace twice frees it once.
  const uint64_t later = jobs.Add("a", "a");
  EXPECT_FALSE(jobs.MayRun(later));
  EXPECT_EQ(jobs.Done(both), Ids({later}));
}

// Many jobs of one namespace, and many of others: each namespace's run in
// the order they came, and the others never wait for them.
TEST(MoltNetDomStorageWaitsTest, ManyJobs) {
  MoltDomStorageNamespaceJobs jobs;
  Ids busy;
  for (int i = 0; i < 1000; ++i) {
    busy.push_back(jobs.Add("busy", "copy " + std::to_string(i)));
  }
  for (int i = 0; i < 1000; ++i) {
    const uint64_t other = jobs.Add("other " + std::to_string(i), "x" +
                                    std::to_string(i));
    EXPECT_TRUE(jobs.MayRun(other));
    EXPECT_EQ(jobs.Done(other), Ids());
  }
  for (size_t i = 0; i < busy.size(); ++i) {
    EXPECT_TRUE(jobs.MayRun(busy[i]));
    if (i + 1 < busy.size()) {
      EXPECT_FALSE(jobs.MayRun(busy[i + 1]));
      EXPECT_EQ(jobs.Done(busy[i]), Ids({busy[i + 1]}));
    } else {
      EXPECT_EQ(jobs.Done(busy[i]), Ids());
    }
  }
  EXPECT_EQ(jobs.size(), 0u);
}

// A copy's wait ends at its last turn, or when its time is up, whichever
// comes first, and only once; once forgotten, late turns count for nothing.
TEST(MoltNetDomStorageWaitsTest, AWaitEndsOnce) {
  EXPECT_EQ(kMoltDomStorageTurnsWaitMs, 1000);
  MoltDomStorageTurnWaits waits;
  waits.Wait(1, 3);
  EXPECT_FALSE(waits.TurnTaken(1));
  EXPECT_FALSE(waits.TurnTaken(1));
  EXPECT_TRUE(waits.TurnTaken(1));
  EXPECT_FALSE(waits.TimedOut(1));
  waits.Forget(1);
  EXPECT_FALSE(waits.TurnTaken(1));

  // A site with a backlog: the time is up before its turn comes.
  waits.Wait(2, 2);
  EXPECT_FALSE(waits.TurnTaken(2));
  EXPECT_TRUE(waits.TimedOut(2));
  EXPECT_FALSE(waits.TurnTaken(2));
  EXPECT_FALSE(waits.TimedOut(2));
  waits.Forget(2);
  EXPECT_EQ(waits.size(), 0u);
  EXPECT_FALSE(waits.TimedOut(2));
  EXPECT_FALSE(waits.TimedOut(99));
}

// Waits are independent of each other.
TEST(MoltNetDomStorageWaitsTest, WaitsDontShareTurns) {
  MoltDomStorageTurnWaits waits;
  waits.Wait(1, 1);
  waits.Wait(2, 1);
  EXPECT_TRUE(waits.TimedOut(1));
  EXPECT_TRUE(waits.TurnTaken(2));
  EXPECT_FALSE(waits.TurnTaken(1));
}

}  // namespace
}  // namespace storage
