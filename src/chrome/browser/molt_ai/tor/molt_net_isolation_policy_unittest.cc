// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_isolation_policy.h"

#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace tor {
namespace {

TEST(MoltNetIsolationPolicyTest, ClassifiesExtensions) {
  EXPECT_EQ(ExtensionOrigin::kUnknown,
            ClassifyExtension(/*known=*/false,
                              /*is_component_location=*/false));
  // Without the extension the location is unknown too; the flag is ignored.
  EXPECT_EQ(ExtensionOrigin::kUnknown,
            ClassifyExtension(/*known=*/false, /*is_component_location=*/true));
  EXPECT_EQ(ExtensionOrigin::kComponent,
            ClassifyExtension(/*known=*/true, /*is_component_location=*/true));
  EXPECT_EQ(ExtensionOrigin::kInstalled,
            ClassifyExtension(/*known=*/true, /*is_component_location=*/false));
}

TEST(MoltNetIsolationPolicyTest, OnlyComponentExtensionsRunInMoltNet) {
  EXPECT_FALSE(IsKeptOutOfMoltNet(ExtensionOrigin::kComponent));
  EXPECT_TRUE(IsKeptOutOfMoltNet(ExtensionOrigin::kInstalled));
  // An ID with no loaded extension behind it is never let in.
  EXPECT_TRUE(IsKeptOutOfMoltNet(ExtensionOrigin::kUnknown));
  // A value outside the enum (a bad cast) fails closed.
  EXPECT_TRUE(IsKeptOutOfMoltNet(static_cast<ExtensionOrigin>(42)));
}

TEST(MoltNetIsolationPolicyTest, CertDecisionMemoryStartsEmpty) {
  CertDecisionMemory memory;
  EXPECT_TRUE(memory.empty());
  EXPECT_FALSE(memory.HasAnyFor("example.com"));
  EXPECT_FALSE(memory.IsAllowed("example.com", "-202cert"));
}

TEST(MoltNetIsolationPolicyTest, CertDecisionMemoryMatchesHostAndKey) {
  CertDecisionMemory memory;
  memory.Allow("example.com", "-202certA");
  EXPECT_FALSE(memory.empty());
  EXPECT_TRUE(memory.HasAnyFor("example.com"));
  EXPECT_TRUE(memory.IsAllowed("example.com", "-202certA"));
  // Another certificate, or the same certificate with another error.
  EXPECT_FALSE(memory.IsAllowed("example.com", "-202certB"));
  EXPECT_FALSE(memory.IsAllowed("example.com", "-201certA"));
  // Another host, including a subdomain or a differently-cased name: the
  // caller passes canonical hosts, and nothing is matched loosely.
  EXPECT_FALSE(memory.IsAllowed("www.example.com", "-202certA"));
  EXPECT_FALSE(memory.IsAllowed("EXAMPLE.COM", "-202certA"));
  EXPECT_FALSE(memory.HasAnyFor("example.org"));

  // Allowing twice is harmless.
  memory.Allow("example.com", "-202certA");
  EXPECT_TRUE(memory.IsAllowed("example.com", "-202certA"));
}

TEST(MoltNetIsolationPolicyTest, CertDecisionMemoryRevokesPerHost) {
  CertDecisionMemory memory;
  memory.Allow("a.test", "k1");
  memory.Allow("a.test", "k2");
  memory.Allow("b.test", "k1");
  memory.Revoke("a.test");
  EXPECT_FALSE(memory.HasAnyFor("a.test"));
  EXPECT_FALSE(memory.IsAllowed("a.test", "k1"));
  EXPECT_FALSE(memory.IsAllowed("a.test", "k2"));
  EXPECT_TRUE(memory.IsAllowed("b.test", "k1"));
  // Revoking a host with no exception changes nothing.
  memory.Revoke("c.test");
  EXPECT_TRUE(memory.IsAllowed("b.test", "k1"));
}

TEST(MoltNetIsolationPolicyTest, CertDecisionMemoryClears) {
  CertDecisionMemory memory;
  memory.Allow("a.test", "k1");
  memory.Allow("b.test", "k1");
  memory.Allow("c.test", "k1");
  memory.RemoveIf([](const std::string& host) {
    return host == "a.test" || host == "c.test";
  });
  EXPECT_FALSE(memory.HasAnyFor("a.test"));
  EXPECT_TRUE(memory.HasAnyFor("b.test"));
  EXPECT_FALSE(memory.HasAnyFor("c.test"));
  memory.RemoveIf([](const std::string&) { return false; });
  EXPECT_TRUE(memory.HasAnyFor("b.test"));
  memory.Clear();
  EXPECT_TRUE(memory.empty());
}

}  // namespace
}  // namespace tor
}  // namespace molt_ai
