// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The manifest of Chromium's on-device models is asked for when a feature
// first asks the model broker for a model, not when the broker is created
// at startup (the "MoltBrowser:" hooks in manifest_monitor.cc and
// manifest_broker_state.cc, docs/NETWORK_TRAFFIC.md). In Chrome the
// delegate's ListenForManifestReady() is what registers the manifest
// component and requests it from the component updater, so these tests
// count its calls.

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/byte_size.h"
#include "base/callback_list.h"
#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/task_environment.h"
#include "components/optimization_guide/core/model_execution/manifest_broker/manifest_asset_manager.h"
#include "components/optimization_guide/core/model_execution/manifest_broker/manifest_broker_state.h"
#include "components/optimization_guide/core/model_execution/model_execution_prefs.h"
#include "components/optimization_guide/core/model_execution/on_device_capability.h"
#include "components/optimization_guide/core/optimization_guide_features.h"
#include "components/optimization_guide/public/mojom/model_broker.mojom.h"
#include "components/prefs/testing_pref_service.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/on_device_model/public/cpp/capabilities.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace optimization_guide {
namespace {

// Stands in for Chrome's delegate (optimization_guide_on_device_model_installer
// .cc) and counts the manifest requests.
class CountingDelegate final : public ManifestAssetManager::Delegate {
 public:
  explicit CountingDelegate(int* manifest_requests)
      : manifest_requests_(manifest_requests) {}

  base::CallbackListSubscription ListenForManifestReady(
      base::RepeatingCallback<void(base::FilePath)> on_ready) override {
    ++*manifest_requests_;
    return ready_callbacks_.Add(std::move(on_ready));
  }

  void GetFreeDiskSpace(
      base::OnceCallback<void(std::optional<base::ByteSize>)> callback)
      const override {
    std::move(callback).Run(std::nullopt);
  }

  void RegisterOnDemandComponent(
      const std::string& public_key_hex,
      const std::string& target_version,
      const std::string& component_name,
      base::WeakPtr<ManifestAssetManager> manager) override {}

  void Uninstall(const std::string& public_key_hex,
                 base::WeakPtr<ManifestAssetManager> manager) override {}

  void RequestUpdate(const std::string& public_key_hex,
                     bool is_background) override {}

 private:
  raw_ptr<int> manifest_requests_;
  base::RepeatingCallbackList<void(base::FilePath)> ready_callbacks_;
};

class NullAvailabilityObserver final
    : public OnDeviceModelAvailabilityObserver {
 public:
  void OnDeviceModelAvailabilityChanged(
      mojom::OnDeviceFeature feature,
      OnDeviceModelEligibilityReason reason) override {}
};

class MoltOnDeviceManifestTest : public testing::Test {
 protected:
  MoltOnDeviceManifestTest() {
    model_execution::prefs::RegisterLocalStatePrefs(local_state_.registry());
  }

  void EnableExecution(bool enabled) {
    if (enabled) {
      feature_list_.InitAndEnableFeature(
          features::kOptimizationGuideModelExecution);
    } else {
      feature_list_.InitAndDisableFeature(
          features::kOptimizationGuideModelExecution);
    }
  }

  // What OptimizationGuideGlobalState does at startup. The service launcher
  // does nothing: these tests never need the on-device model service.
  std::unique_ptr<ManifestBrokerState> CreateBroker() {
    return std::make_unique<ManifestBrokerState>(
        local_state_, std::make_unique<CountingDelegate>(&manifest_requests_),
        base::DoNothing(), /*component_update_service=*/nullptr);
  }

  base::test::TaskEnvironment task_environment_;
  base::test::ScopedFeatureList feature_list_;
  TestingPrefServiceSimple local_state_;
  int manifest_requests_ = 0;
};

TEST_F(MoltOnDeviceManifestTest, NotRequestedAtStartup) {
  EnableExecution(true);
  std::unique_ptr<ManifestBrokerState> broker = CreateBroker();
  task_environment_.RunUntilIdle();
  EXPECT_EQ(manifest_requests_, 0);
}

// Starting a session (Enhanced protection's on-device scam check does this)
// asks for it, once.
TEST_F(MoltOnDeviceManifestTest, StartSessionRequestsItOnce) {
  EnableExecution(true);
  std::unique_ptr<ManifestBrokerState> broker = CreateBroker();
  EXPECT_FALSE(broker->StartSession(mojom::OnDeviceFeature::kTest,
                                    SessionConfigParams{}, nullptr));
  EXPECT_EQ(manifest_requests_, 1);
  EXPECT_FALSE(broker->StartSession(mojom::OnDeviceFeature::kTest,
                                    SessionConfigParams{}, nullptr));
  EXPECT_EQ(manifest_requests_, 1);
}

TEST_F(MoltOnDeviceManifestTest, EligibilityCheckRequestsIt) {
  EnableExecution(true);
  std::unique_ptr<ManifestBrokerState> broker = CreateBroker();
  broker->GetOnDeviceModelEligibility(mojom::OnDeviceFeature::kTest);
  EXPECT_EQ(manifest_requests_, 1);
}

// The asynchronous check waits for the manifest, so it must ask for it.
TEST_F(MoltOnDeviceManifestTest, AsyncEligibilityCheckRequestsIt) {
  EnableExecution(true);
  std::unique_ptr<ManifestBrokerState> broker = CreateBroker();
  broker->GetOnDeviceModelEligibilityAsync(mojom::OnDeviceFeature::kTest,
                                           on_device_model::Capabilities(),
                                           base::DoNothing());
  EXPECT_EQ(manifest_requests_, 1);
}

TEST_F(MoltOnDeviceManifestTest, AvailabilityObserverRequestsIt) {
  EnableExecution(true);
  std::unique_ptr<ManifestBrokerState> broker = CreateBroker();
  NullAvailabilityObserver observer;
  broker->AddOnDeviceModelAvailabilityChangeObserver(
      mojom::OnDeviceFeature::kTest, &observer);
  EXPECT_EQ(manifest_requests_, 1);
  broker->RemoveOnDeviceModelAvailabilityChangeObserver(
      mojom::OnDeviceFeature::kTest, &observer);
}

// Model broker clients (Live Caption's expert model, on-device Web Speech)
// bind the broker first.
TEST_F(MoltOnDeviceManifestTest, BindingTheBrokerRequestsIt) {
  EnableExecution(true);
  std::unique_ptr<ManifestBrokerState> broker = CreateBroker();
  mojo::Remote<mojom::ModelBroker> remote(broker->BindAndPassRemoteBroker());
  EXPECT_EQ(manifest_requests_, 1);
}

// Live Caption's expert model un-requests its use case when it is turned
// off; that is not a demand.
TEST_F(MoltOnDeviceManifestTest, OnlyRequestingAUseCaseRequestsIt) {
  EnableExecution(true);
  std::unique_ptr<ManifestBrokerState> broker = CreateBroker();
  broker->SetUseCaseRequested("test", false);
  EXPECT_EQ(manifest_requests_, 0);
  broker->SetUseCaseRequested("test", true);
  EXPECT_EQ(manifest_requests_, 1);
}

TEST_F(MoltOnDeviceManifestTest, NothingIsRequestedWithExecutionOff) {
  EnableExecution(false);
  std::unique_ptr<ManifestBrokerState> broker = CreateBroker();
  EXPECT_FALSE(broker->StartSession(mojom::OnDeviceFeature::kTest,
                                    SessionConfigParams{}, nullptr));
  broker->GetOnDeviceModelEligibilityAsync(mojom::OnDeviceFeature::kTest,
                                           on_device_model::Capabilities(),
                                           base::DoNothing());
  mojo::Remote<mojom::ModelBroker> remote(broker->BindAndPassRemoteBroker());
  task_environment_.RunUntilIdle();
  EXPECT_EQ(manifest_requests_, 0);
}

}  // namespace
}  // namespace optimization_guide
