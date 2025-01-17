/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <ReactCommon/RuntimeExecutor.h>
#include <better/optional.h>
#include <better/set.h>
#include <react/renderer/animations/LayoutAnimationCallbackWrapper.h>
#include <react/renderer/animations/primitives.h>
#include <react/renderer/core/RawValue.h>
#include <react/renderer/debug/flags.h>
#include <react/renderer/mounting/MountingOverrideDelegate.h>
#include <react/renderer/mounting/MountingTransaction.h>
#include <react/renderer/mounting/ShadowViewMutation.h>
#include <react/renderer/uimanager/LayoutAnimationStatusDelegate.h>
#include <react/renderer/uimanager/UIManagerAnimationDelegate.h>

namespace facebook {
namespace react {

#ifdef LAYOUT_ANIMATION_VERBOSE_LOGGING
void PrintMutationInstruction(
    std::string message,
    ShadowViewMutation const &mutation);
void PrintMutationInstructionRelative(
    std::string message,
    ShadowViewMutation const &mutation,
    ShadowViewMutation const &relativeMutation);
#else
#define PrintMutationInstruction(a, b)
#define PrintMutationInstructionRelative(a, b, c)
#endif

class LayoutAnimationKeyFrameManager : public UIManagerAnimationDelegate,
                                       public MountingOverrideDelegate {
 public:
  LayoutAnimationKeyFrameManager(
      RuntimeExecutor runtimeExecutor,
      LayoutAnimationStatusDelegate *delegate);

#pragma mark - UIManagerAnimationDelegate methods

  void uiManagerDidConfigureNextLayoutAnimation(
      jsi::Runtime &runtime,
      RawValue const &config,
      const jsi::Value &successCallbackValue,
      const jsi::Value &failureCallbackValue) const override;

  void setComponentDescriptorRegistry(SharedComponentDescriptorRegistry const &
                                          componentDescriptorRegistry) override;

  // TODO: add SurfaceId to this API as well
  bool shouldAnimateFrame() const override;

  void stopSurface(SurfaceId surfaceId) override;

#pragma mark - MountingOverrideDelegate methods

  bool shouldOverridePullTransaction() const override;

  // This is used to "hijack" the diffing process to figure out which mutations
  // should be animated. The mutations returned by this function will be
  // executed immediately.
  better::optional<MountingTransaction> pullTransaction(
      SurfaceId surfaceId,
      MountingTransaction::Number number,
      TransactionTelemetry const &telemetry,
      ShadowViewMutationList mutations) const override;

  // Exposed for testing.
  void uiManagerDidConfigureNextLayoutAnimation(
      LayoutAnimation layoutAnimation) const;

  // LayoutAnimationStatusDelegate - this is for the platform to get
  // signal when animations start and complete. Setting and resetting this
  // delegate is protected by a mutex; ALL method calls into this delegate are
  // also protected by the mutex! The only way to set this without a mutex is
  // via a constructor.
  void setLayoutAnimationStatusDelegate(
      LayoutAnimationStatusDelegate *delegate) const;

  void setClockNow(std::function<uint64_t()> now);

 protected:
  SharedComponentDescriptorRegistry componentDescriptorRegistry_;
  mutable better::optional<LayoutAnimation> currentAnimation_{};
  mutable std::mutex currentAnimationMutex_;

  /**
   * All mutations of inflightAnimations_ are thread-safe as long as
   * we keep the contract of: only mutate it within the context of
   * `pullTransaction`. If that contract is held, this is implicitly protected
   * by the MountingCoordinator's mutex.
   */
  mutable std::vector<LayoutAnimation> inflightAnimations_{};

  bool hasComponentDescriptorForShadowView(ShadowView const &shadowView) const;
  ComponentDescriptor const &getComponentDescriptorForShadowView(
      ShadowView const &shadowView) const;
  std::pair<double, double> calculateAnimationProgress(
      uint64_t now,
      LayoutAnimation const &animation,
      AnimationConfig const &mutationConfig) const;

  /**
   * Given a `progress` between 0 and 1, a mutation and LayoutAnimation config,
   * return a ShadowView with mutated props and/or LayoutMetrics.
   *
   * @param progress
   * @param layoutAnimation
   * @param animatedMutation
   * @return
   */
  ShadowView createInterpolatedShadowView(
      double progress,
      ShadowView startingView,
      ShadowView finalView) const;

  void callCallback(const LayoutAnimationCallbackWrapper &callback) const;

  virtual void animationMutationsForFrame(
      SurfaceId surfaceId,
      ShadowViewMutation::List &mutationsList,
      uint64_t now) const = 0;

  /**
   * Queue (and potentially synthesize) final mutations for a finished keyframe.
   * Keyframe animation may have timed-out, or be canceled due to a conflict.
   */
  void queueFinalMutationsForCompletedKeyFrame(
      AnimationKeyFrame const &keyframe,
      ShadowViewMutation::List &mutationsList,
      bool interrupted,
      std::string logPrefix) const;

 private:
  RuntimeExecutor runtimeExecutor_;
  mutable std::mutex layoutAnimationStatusDelegateMutex_;
  mutable LayoutAnimationStatusDelegate *layoutAnimationStatusDelegate_{};
  mutable std::mutex surfaceIdsToStopMutex_;
  mutable better::set<SurfaceId> surfaceIdsToStop_{};

  // Function that returns current time in milliseconds
  std::function<uint64_t()> now_;

  void adjustImmediateMutationIndicesForDelayedMutations(
      SurfaceId surfaceId,
      ShadowViewMutation &mutation,
      bool skipLastAnimation = false,
      bool lastAnimationOnly = false) const;

  void adjustDelayedMutationIndicesForMutation(
      SurfaceId surfaceId,
      ShadowViewMutation const &mutation,
      bool skipLastAnimation = false) const;

  void getAndEraseConflictingAnimations(
      SurfaceId surfaceId,
      ShadowViewMutationList const &mutations,
      std::vector<AnimationKeyFrame> &conflictingAnimations) const;
};

static inline bool shouldFirstComeBeforeSecondRemovesOnly(
    ShadowViewMutation const &lhs,
    ShadowViewMutation const &rhs) noexcept {
  // Make sure that removes on the same level are sorted - highest indices must
  // come first.
  return (lhs.type == ShadowViewMutation::Type::Remove &&
          lhs.type == rhs.type) &&
      (lhs.parentShadowView.tag == rhs.parentShadowView.tag) &&
      (lhs.index > rhs.index);
}

static inline bool shouldFirstComeBeforeSecondMutation(
    ShadowViewMutation const &lhs,
    ShadowViewMutation const &rhs) noexcept {
  if (lhs.type != rhs.type) {
    // Deletes always come last
    if (lhs.type == ShadowViewMutation::Type::Delete) {
      return false;
    }
    if (rhs.type == ShadowViewMutation::Type::Delete) {
      return true;
    }

    // Remove comes before insert
    if (lhs.type == ShadowViewMutation::Type::Remove &&
        rhs.type == ShadowViewMutation::Type::Insert) {
      return true;
    }
    if (rhs.type == ShadowViewMutation::Type::Remove &&
        lhs.type == ShadowViewMutation::Type::Insert) {
      return false;
    }

    // Create comes before insert
    if (lhs.type == ShadowViewMutation::Type::Create &&
        rhs.type == ShadowViewMutation::Type::Insert) {
      return true;
    }
    if (rhs.type == ShadowViewMutation::Type::Create &&
        lhs.type == ShadowViewMutation::Type::Insert) {
      return false;
    }
  } else {
    // Make sure that removes on the same level are sorted - highest indices
    // must come first.
    if (lhs.type == ShadowViewMutation::Type::Remove &&
        lhs.parentShadowView.tag == rhs.parentShadowView.tag) {
      if (lhs.index > rhs.index) {
        return true;
      } else {
        return false;
      }
    }
  }

  return false;
}

} // namespace react
} // namespace facebook
