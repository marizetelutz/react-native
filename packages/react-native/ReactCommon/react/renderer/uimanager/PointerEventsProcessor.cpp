/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PointerEventsProcessor.h"

#include <react/renderer/components/view/ViewShadowNode.h>

namespace facebook::react {

static ShadowNode::Shared GetShadowNodeFromEventTarget(
    jsi::Runtime &runtime,
    EventTarget const &target) {
  auto instanceHandle = target.getInstanceHandle(runtime);
  if (instanceHandle.isObject()) {
    auto handleObj = instanceHandle.asObject(runtime);
    if (handleObj.hasProperty(runtime, "stateNode")) {
      auto stateNode = handleObj.getProperty(runtime, "stateNode");
      if (stateNode.isObject()) {
        auto stateNodeObj = stateNode.asObject(runtime);
        if (stateNodeObj.hasProperty(runtime, "node")) {
          auto node = stateNodeObj.getProperty(runtime, "node");
          return shadowNodeFromValue(runtime, node);
        }
      }
    }
  }
  return nullptr;
}

static bool IsViewListeningToEvents(
    ShadowNode const &shadowNode,
    std::initializer_list<ViewEvents::Offset> eventTypes) {
  if (auto viewShadowNode = traitCast<ViewShadowNode const *>(&shadowNode)) {
    auto &viewProps = viewShadowNode->getConcreteProps();
    for (const ViewEvents::Offset eventType : eventTypes) {
      if (viewProps.events[eventType]) {
        return true;
      }
    }
  }
  return false;
}

static bool IsAnyViewInPathToRootListeningToEvents(
    UIManager const &uiManager,
    ShadowNode const &shadowNode,
    std::initializer_list<ViewEvents::Offset> eventTypes) {
  // Check the target view first
  if (IsViewListeningToEvents(shadowNode, eventTypes)) {
    return true;
  }

  // Retrieve the node's root & a list of nodes between the target and the root
  auto owningRootShadowNode = ShadowNode::Shared{};
  uiManager.getShadowTreeRegistry().visit(
      shadowNode.getSurfaceId(),
      [&owningRootShadowNode](ShadowTree const &shadowTree) {
        owningRootShadowNode = shadowTree.getCurrentRevision().rootShadowNode;
      });

  if (owningRootShadowNode == nullptr) {
    return false;
  }

  auto &nodeFamily = shadowNode.getFamily();
  auto ancestors = nodeFamily.getAncestors(*owningRootShadowNode);

  // Check for listeners from the target's parent to the root
  for (auto it = ancestors.rbegin(); it != ancestors.rend(); it++) {
    auto &currentNode = it->first.get();
    if (IsViewListeningToEvents(currentNode, eventTypes)) {
      return true;
    }
  }

  return false;
}

static PointerEventTarget RetargetPointerEvent(
    PointerEvent const &event,
    ShadowNode const &nodeToTarget,
    UIManager const &uiManager) {
  PointerEvent retargetedEvent(event);

  // TODO: is dereferencing latestNodeToTarget without null checking safe?
  auto latestNodeToTarget = uiManager.getNewestCloneOfShadowNode(nodeToTarget);

  // Adjust offsetX/Y to be relative to the retargeted node
  // HACK: This is a basic/incomplete implementation which simply subtracts
  // the retargeted node's origin from the original event's client coordinates.
  // More work will be needed to properly take non-trival transforms into
  // account.
  auto layoutMetrics = uiManager.getRelativeLayoutMetrics(
      *latestNodeToTarget, nullptr, {/* .includeTransform */ true});
  retargetedEvent.offsetPoint = {
      event.clientPoint.x - layoutMetrics.frame.origin.x,
      event.clientPoint.y - layoutMetrics.frame.origin.y,
  };

  // Retrieve the event target of the retargeted node
  auto retargetedEventTarget =
      latestNodeToTarget->getEventEmitter()->getEventTarget();

  PointerEventTarget result = {};
  result.event = retargetedEvent;
  result.target = retargetedEventTarget;
  return result;
}

static ShadowNode::Shared getCaptureTargetOverride(
    PointerIdentifier pointerId,
    CaptureTargetOverrideRegistry &registry) {
  auto pendingPointerItr = registry.find(pointerId);
  if (pendingPointerItr == registry.end()) {
    return nullptr;
  }

  ShadowNode::Weak maybeTarget = pendingPointerItr->second;
  if (maybeTarget.expired()) {
    // target has expired so it should functionally behave the same as if it
    // was removed from the override list.
    registry.erase(pointerId);
    return nullptr;
  }

  return maybeTarget.lock();
}

/*
 * Centralized method which determines if an event should be sent to JS by
 * inspecing the listeners in the target's view path.
 */
static bool ShouldEmitPointerEvent(
    ShadowNode const &targetNode,
    std::string const &type,
    UIManager const &uiManager) {
  if (type == "topPointerDown") {
    return IsAnyViewInPathToRootListeningToEvents(
        uiManager,
        targetNode,
        {ViewEvents::Offset::PointerDown,
         ViewEvents::Offset::PointerDownCapture});
  } else if (type == "topPointerUp") {
    return IsAnyViewInPathToRootListeningToEvents(
        uiManager,
        targetNode,
        {ViewEvents::Offset::PointerUp, ViewEvents::Offset::PointerUpCapture});
  } else if (type == "topPointerMove") {
    return IsAnyViewInPathToRootListeningToEvents(
        uiManager,
        targetNode,
        {ViewEvents::Offset::PointerMove,
         ViewEvents::Offset::PointerMoveCapture});
  } else if (type == "topPointerEnter") {
    // This event goes through the capturing phase in full but only bubble
    // through the target and no futher up the tree
    return IsViewListeningToEvents(
               targetNode, {ViewEvents::Offset::PointerEnter}) ||
        IsAnyViewInPathToRootListeningToEvents(
               uiManager,
               targetNode,
               {ViewEvents::Offset::PointerEnterCapture});
  } else if (type == "topPointerLeave") {
    // This event goes through the capturing phase in full but only bubble
    // through the target and no futher up the tree
    return IsViewListeningToEvents(
               targetNode, {ViewEvents::Offset::PointerLeave}) ||
        IsAnyViewInPathToRootListeningToEvents(
               uiManager,
               targetNode,
               {ViewEvents::Offset::PointerLeaveCapture});
  } else if (type == "topPointerOver") {
    return IsAnyViewInPathToRootListeningToEvents(
        uiManager,
        targetNode,
        {ViewEvents::Offset::PointerOver,
         ViewEvents::Offset::PointerOverCapture});
  } else if (type == "topPointerOut") {
    return IsAnyViewInPathToRootListeningToEvents(
        uiManager,
        targetNode,
        {ViewEvents::Offset::PointerOut,
         ViewEvents::Offset::PointerOutCapture});
  } else if (type == "topClick") {
    return IsAnyViewInPathToRootListeningToEvents(
        uiManager,
        targetNode,
        {ViewEvents::Offset::Click, ViewEvents::Offset::ClickCapture});
  }
  // This is more of an optimization method so if we encounter a type which
  // has not been specifically addressed above we should just let it through.
  return true;
}

void PointerEventsProcessor::interceptPointerEvent(
    jsi::Runtime &runtime,
    EventTarget const *target,
    std::string const &type,
    ReactEventPriority priority,
    PointerEvent const &event,
    DispatchEvent const &eventDispatcher,
    UIManager const &uiManager) {
  // Process all pending pointer capture assignments
  processPendingPointerCapture(event, runtime, eventDispatcher, uiManager);

  PointerEvent pointerEvent(event);
  EventTarget const *eventTarget = target;

  // Retarget the event if it has a pointer capture override target
  auto overrideTarget = getCaptureTargetOverride(
      pointerEvent.pointerId, pendingPointerCaptureTargetOverrides_);
  if (overrideTarget != nullptr &&
      overrideTarget->getTag() != eventTarget->getTag()) {
    auto retargeted =
        RetargetPointerEvent(pointerEvent, *overrideTarget, uiManager);

    pointerEvent = retargeted.event;
    eventTarget = retargeted.target.get();
  }

  eventTarget->retain(runtime);
  auto shadowNode = GetShadowNodeFromEventTarget(runtime, *eventTarget);
  if (shadowNode != nullptr &&
      ShouldEmitPointerEvent(*shadowNode, type, uiManager)) {
    eventDispatcher(runtime, eventTarget, type, priority, pointerEvent);
  }
  eventTarget->release(runtime);

  // Implicit pointer capture release
  if (overrideTarget != nullptr &&
      (type == "topPointerUp" || type == "topPointerCancel")) {
    releasePointerCapture(pointerEvent.pointerId, overrideTarget.get());
    processPendingPointerCapture(
        pointerEvent, runtime, eventDispatcher, uiManager);
  }
}

void PointerEventsProcessor::setPointerCapture(
    PointerIdentifier pointerId,
    ShadowNode::Shared const &shadowNode) {
  // TODO: Throw DOMException with name "NotFoundError" when pointerId does not
  // match any of the active pointers
  pendingPointerCaptureTargetOverrides_[pointerId] = shadowNode;
}

void PointerEventsProcessor::releasePointerCapture(
    PointerIdentifier pointerId,
    ShadowNode const *shadowNode) {
  // TODO: Throw DOMException with name "NotFoundError" when pointerId does not
  // match any of the active pointers

  // We only clear the pointer's capture target override if release was called
  // on the shadowNode which has the capture override, otherwise the result
  // should no-op
  auto pendingTarget = getCaptureTargetOverride(
      pointerId, pendingPointerCaptureTargetOverrides_);
  if (pendingTarget != nullptr &&
      pendingTarget->getTag() == shadowNode->getTag()) {
    pendingPointerCaptureTargetOverrides_.erase(pointerId);
  }
}

bool PointerEventsProcessor::hasPointerCapture(
    PointerIdentifier pointerId,
    ShadowNode const *shadowNode) {
  ShadowNode::Shared pendingTarget = getCaptureTargetOverride(
      pointerId, pendingPointerCaptureTargetOverrides_);
  if (pendingTarget != nullptr) {
    return pendingTarget->getTag() == shadowNode->getTag();
  }
  return false;
}

void PointerEventsProcessor::processPendingPointerCapture(
    PointerEvent const &event,
    jsi::Runtime &runtime,
    DispatchEvent const &eventDispatcher,
    UIManager const &uiManager) {
  auto pendingOverride = getCaptureTargetOverride(
      event.pointerId, pendingPointerCaptureTargetOverrides_);
  bool hasPendingOverride = pendingOverride != nullptr;

  auto activeOverride = getCaptureTargetOverride(
      event.pointerId, activePointerCaptureTargetOverrides_);
  bool hasActiveOverride = activeOverride != nullptr;

  if (!hasPendingOverride && !hasActiveOverride) {
    return;
  }

  auto pendingOverrideTag =
      (hasPendingOverride) ? pendingOverride->getTag() : -1;
  auto activeOverrideTag = (hasActiveOverride) ? activeOverride->getTag() : -1;

  if (hasActiveOverride && activeOverrideTag != pendingOverrideTag) {
    auto retargeted = RetargetPointerEvent(event, *activeOverride, uiManager);

    retargeted.target->retain(runtime);
    auto shadowNode = GetShadowNodeFromEventTarget(runtime, *retargeted.target);
    if (shadowNode != nullptr &&
        ShouldEmitPointerEvent(
            *shadowNode, "topLostPointerCapture", uiManager)) {
      eventDispatcher(
          runtime,
          retargeted.target.get(),
          "topLostPointerCapture",
          ReactEventPriority::Discrete,
          retargeted.event);
    }
    retargeted.target->release(runtime);
  }

  if (hasPendingOverride && activeOverrideTag != pendingOverrideTag) {
    auto retargeted = RetargetPointerEvent(event, *pendingOverride, uiManager);

    retargeted.target->retain(runtime);
    auto shadowNode = GetShadowNodeFromEventTarget(runtime, *retargeted.target);
    if (shadowNode != nullptr &&
        ShouldEmitPointerEvent(
            *shadowNode, "topGotPointerCapture", uiManager)) {
      eventDispatcher(
          runtime,
          retargeted.target.get(),
          "topGotPointerCapture",
          ReactEventPriority::Discrete,
          retargeted.event);
    }
    retargeted.target->release(runtime);
  }

  if (!hasPendingOverride) {
    activePointerCaptureTargetOverrides_.erase(event.pointerId);
  } else {
    activePointerCaptureTargetOverrides_[event.pointerId] = pendingOverride;
  }
}

} // namespace facebook::react