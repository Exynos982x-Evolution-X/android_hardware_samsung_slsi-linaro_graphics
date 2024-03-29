/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ComposerCommandEngine.h"

#include <hardware/hwcomposer2.h>

#include "Util.h"

/// The command engine interface is not 'pure' aidl. Conversion to aidl
// structure is done within this class. Don't mix it with impl/translate.
// Expect to have an AIDL command interface in the future.
//
// The initial implementation is a combination of asop ComposerCommandEngine 2.1 to 2.4
// and adapt to aidl structures.
namespace aidl::android::hardware::graphics::composer3::impl {

#define DISPATCH_LAYER_COMMAND(display, layerCmd, field, funcName)               \
    do {                                                                         \
        if (layerCmd.field) {                                                    \
            executeSetLayer##funcName(display, layerCmd.layer, *layerCmd.field); \
        }                                                                        \
    } while (0)

#define DISPATCH_DISPLAY_COMMAND(displayCmd, field, funcName)                \
    do {                                                                     \
        if (displayCmd.field) {                                              \
            execute##funcName(displayCmd.display, *displayCmd.field);        \
        }                                                                    \
    } while (0)

#define DISPATCH_DISPLAY_BOOL_COMMAND(displayCmd, field, funcName)           \
    do {                                                                     \
        if (displayCmd.field) {                                              \
            execute##funcName(displayCmd.display);                           \
        }                                                                    \
    } while (0)

#define DISPATCH_DISPLAY_BOOL_COMMAND_AND_DATA(displayCmd, field, data, funcName) \
    do {                                                                          \
        if (displayCmd.field) {                                                   \
            execute##funcName(displayCmd.display, displayCmd.data);               \
        }                                                                         \
    } while (0)

int32_t ComposerCommandEngine::init() {
    mWriter = std::make_unique<ComposerServiceWriter>();
    return (mWriter != nullptr) ? ::android::NO_ERROR : ::android::NO_MEMORY;
}

int32_t ComposerCommandEngine::execute(const std::vector<DisplayCommand>& commands,
                                       std::vector<CommandResultPayload>* result) {
    mCommandIndex = 0;
    for (const auto& command : commands) {
        dispatchDisplayCommand(command);
        ++mCommandIndex;
    }

    *result = mWriter->getPendingCommandResults();
    return 0;
}

void ComposerCommandEngine::dispatchDisplayCommand(const DisplayCommand& command) {
    for (const auto& layerCmd : command.layers) {
        dispatchLayerCommand(command.display, layerCmd);
    }

    DISPATCH_DISPLAY_COMMAND(command, colorTransformMatrix, SetColorTransform);
    DISPATCH_DISPLAY_COMMAND(command, clientTarget, SetClientTarget);
    DISPATCH_DISPLAY_COMMAND(command, virtualDisplayOutputBuffer, SetOutputBuffer);
    DISPATCH_DISPLAY_COMMAND(command, brightness, SetDisplayBrightness);
    DISPATCH_DISPLAY_BOOL_COMMAND_AND_DATA(command, validateDisplay, expectedPresentTime,
                                           ValidateDisplay);
    DISPATCH_DISPLAY_BOOL_COMMAND(command, acceptDisplayChanges, AcceptDisplayChanges);
    DISPATCH_DISPLAY_BOOL_COMMAND(command, presentDisplay, PresentDisplay);
    DISPATCH_DISPLAY_BOOL_COMMAND_AND_DATA(command, presentOrValidateDisplay, expectedPresentTime,
                                           PresentOrValidateDisplay);
}

void ComposerCommandEngine::dispatchLayerCommand(int64_t display, const LayerCommand& command) {
    DISPATCH_LAYER_COMMAND(display, command, cursorPosition, CursorPosition);
    DISPATCH_LAYER_COMMAND(display, command, buffer, Buffer);
    DISPATCH_LAYER_COMMAND(display, command, damage, SurfaceDamage);
    DISPATCH_LAYER_COMMAND(display, command, blendMode, BlendMode);
    DISPATCH_LAYER_COMMAND(display, command, color, Color);
    DISPATCH_LAYER_COMMAND(display, command, composition, Composition);
    DISPATCH_LAYER_COMMAND(display, command, dataspace, Dataspace);
    DISPATCH_LAYER_COMMAND(display, command, displayFrame, DisplayFrame);
    DISPATCH_LAYER_COMMAND(display, command, planeAlpha, PlaneAlpha);
    DISPATCH_LAYER_COMMAND(display, command, sidebandStream, SidebandStream);
    DISPATCH_LAYER_COMMAND(display, command, sourceCrop, SourceCrop);
    DISPATCH_LAYER_COMMAND(display, command, transform, Transform);
    DISPATCH_LAYER_COMMAND(display, command, visibleRegion, VisibleRegion);
    DISPATCH_LAYER_COMMAND(display, command, z, ZOrder);
    DISPATCH_LAYER_COMMAND(display, command, colorTransform, ColorTransform);
    // TODO: (b/196171661) add support for mixed composition
    // DISPATCH_LAYER_COMMAND(display, command, whitePointNits, WhitePointNits);
    DISPATCH_LAYER_COMMAND(display, command, perFrameMetadata, PerFrameMetadata);
    DISPATCH_LAYER_COMMAND(display, command, perFrameMetadataBlob, PerFrameMetadataBlobs);
}

int32_t ComposerCommandEngine::executeValidateDisplayInternal(int64_t display) {
    std::vector<int64_t> changedLayers;
    std::vector<Composition> compositionTypes;
    uint32_t displayRequestMask = 0x0;
    std::vector<int64_t> requestedLayers;
    std::vector<int32_t> requestMasks;
    ClientTargetProperty clientTargetProperty{common::PixelFormat::RGBA_8888,
                                              common::Dataspace::UNKNOWN};
    auto err = mHal->validateDisplay(display, &changedLayers, &compositionTypes,
                                     &displayRequestMask, &requestedLayers, &requestMasks,
                                     &clientTargetProperty);
    mResources->setDisplayMustValidateState(display, false);
    if (err == HWC2_ERROR_NONE || err == HWC2_ERROR_HAS_CHANGES) {
        mWriter->setChangedCompositionTypes(display, changedLayers, compositionTypes);
        mWriter->setDisplayRequests(display, displayRequestMask, requestedLayers, requestMasks);
    } else {
        LOG(ERROR) << __func__ << ": err " << err;
        mWriter->setError(mCommandIndex, err);
    }
    return err;
}

void ComposerCommandEngine::executeSetColorTransform(int64_t display,
                                                     const std::vector<float>& matrix) {
    auto err = mHal->setColorTransform(display, matrix);
    if (err) {
        LOG(ERROR) << __func__ << ": err " << err;
        mWriter->setError(mCommandIndex, err);
    }
}

void ComposerCommandEngine::executeSetClientTarget(int64_t display, const ClientTarget& command) {
    bool useCache = !command.buffer.handle;
    buffer_handle_t handle = useCache
                             ? nullptr
                             : ::android::makeFromAidl(*command.buffer.handle);
    buffer_handle_t clientTarget;
    auto bufferReleaser = mResources->createReleaser(true);
    auto err = mResources->getDisplayClientTarget(display, command.buffer.slot, useCache, handle,
                                                  clientTarget, bufferReleaser.get());
    if (!err) {
        err = mHal->setClientTarget(display, clientTarget, command.buffer.fence,
                                    command.dataspace, command.damage);
        if (err) {
            LOG(ERROR) << __func__ << " setClientTarget: err " << err;
            mWriter->setError(mCommandIndex, err);
        }
    } else {
        LOG(ERROR) << __func__ << " getDisplayClientTarget : err " << err;
        mWriter->setError(mCommandIndex, err);
    }
}

void ComposerCommandEngine::executeSetOutputBuffer(uint64_t display, const Buffer& buffer) {
    bool useCache = !buffer.handle;
    buffer_handle_t handle = useCache
                             ? nullptr
                             : ::android::makeFromAidl(*buffer.handle);
    buffer_handle_t outputBuffer;
    auto bufferReleaser = mResources->createReleaser(true);
    auto err = mResources->getDisplayOutputBuffer(display, buffer.slot, useCache, handle,
                                                  outputBuffer, bufferReleaser.get());
    if (!err) {
        err = mHal->setOutputBuffer(display, outputBuffer, buffer.fence);
        if (err) {
            LOG(ERROR) << __func__ << " setOutputBuffer: err " << err;
            mWriter->setError(mCommandIndex, err);
        }
    } else {
        LOG(ERROR) << __func__ << " getDisplayOutputBuffer: err " << err;
        mWriter->setError(mCommandIndex, err);
    }
}

void ComposerCommandEngine::executeSetExpectedPresentTimeInternal(
        int64_t display, const std::optional<ClockMonotonicTimestamp> expectedPresentTime) {
    mHal->setExpectedPresentTime(display, expectedPresentTime);
}

void ComposerCommandEngine::executeValidateDisplay(
        int64_t display, const std::optional<ClockMonotonicTimestamp> expectedPresentTime) {
    executeSetExpectedPresentTimeInternal(display, expectedPresentTime);
    executeValidateDisplayInternal(display);
}

void ComposerCommandEngine::executeSetDisplayBrightness(uint64_t display,
                                        const DisplayBrightness& command) {
    auto err = mHal->setDisplayBrightness(display, command.brightness);
    if (err) {
        LOG(ERROR) << __func__ << ": err " << err;
        mWriter->setError(mCommandIndex, err);
    }
}

void ComposerCommandEngine::executePresentOrValidateDisplay(
        int64_t display, const std::optional<ClockMonotonicTimestamp> expectedPresentTime) {
    executeSetExpectedPresentTimeInternal(display, expectedPresentTime);
    // First try to Present as is.
    auto presentErr = mResources->mustValidateDisplay(display) ? IComposerClient::EX_NOT_VALIDATED
                                                               : executePresentDisplay(display);
    if (!presentErr) {
        mWriter->setPresentOrValidateResult(display, PresentOrValidate::Result::Presented);
        return;
    }

    // Fallback to validate
    auto validateErr = executeValidateDisplayInternal(display);
    if (validateErr != HWC2_ERROR_NONE && validateErr != HWC2_ERROR_HAS_CHANGES) return;

    bool hasClientComp = false;
    bool cannotPresentDirectly = (validateErr == HWC2_ERROR_HAS_CHANGES) ||
            (mHal->getHasClientComposition(display, hasClientComp) == HWC2_ERROR_NONE &&
             hasClientComp);
    if (cannotPresentDirectly) {
        mWriter->setPresentOrValidateResult(display, PresentOrValidate::Result::Validated);
        return;
    }

    // Try to call present again
    executeAcceptDisplayChanges(display);
    presentErr = executePresentDisplay(display);
    if (!presentErr) {
        mWriter->setPresentOrValidateResult(display, PresentOrValidate::Result::Presented);
    }
}

void ComposerCommandEngine::executeAcceptDisplayChanges(int64_t display) {
    auto err = mHal->acceptDisplayChanges(display);
    if (err) {
        LOG(ERROR) << __func__ << ": err " << err;
        mWriter->setError(mCommandIndex, err);
    }
}

int ComposerCommandEngine::executePresentDisplay(int64_t display) {
    ndk::ScopedFileDescriptor presentFence;
    std::vector<int64_t> layers;
    std::vector<ndk::ScopedFileDescriptor> fences;
    auto err = mHal->presentDisplay(display, presentFence, &layers, &fences);
    if (!err) {
        if (presentFence != ndk::ScopedFileDescriptor(-1))
            mWriter->setPresentFence(display, std::move(presentFence));
        mWriter->setReleaseFences(display, layers, std::move(fences));
    }
    return err;
}

void ComposerCommandEngine::executeSetLayerCursorPosition(int64_t display, int64_t layer,
                                       const common::Point& cursorPosition) {
    auto err = mHal->setLayerCursorPosition(display, layer, cursorPosition.x, cursorPosition.y);
    if (err) {
        LOG(ERROR) << __func__ << ": err " << err;
        mWriter->setError(mCommandIndex, err);
    }
}

void ComposerCommandEngine::executeSetLayerBuffer(int64_t display, int64_t layer,
                                                  const Buffer& buffer) {
    bool useCache = !buffer.handle;
    buffer_handle_t handle = useCache
                             ? nullptr
                             : ::android::makeFromAidl(*buffer.handle);
    buffer_handle_t hwcBuffer;
    auto bufferReleaser = mResources->createReleaser(true);
    auto err = mResources->getLayerBuffer(display, layer, buffer.slot, useCache,
                                          handle, hwcBuffer, bufferReleaser.get());
    if (!err) {
        err = mHal->setLayerBuffer(display, layer, hwcBuffer, buffer.fence);
        if (err) {
            LOG(ERROR) << __func__ << ": setLayerBuffer err " << err;
            mWriter->setError(mCommandIndex, err);
        }
    } else {
        LOG(ERROR) << __func__ << ": getLayerBuffer err " << err;
        mWriter->setError(mCommandIndex, err);
    }
}

void ComposerCommandEngine::executeSetLayerSurfaceDamage(int64_t display, int64_t layer,
                              const std::vector<std::optional<common::Rect>>& damage) {
    auto err = mHal->setLayerSurfaceDamage(display, layer, damage);
    if (err) {
        LOG(ERROR) << __func__ << ": err " << err;
        mWriter->setError(mCommandIndex, err);
    }
}

void ComposerCommandEngine::executeSetLayerBlendMode(int64_t display, int64_t layer,
                                                     const ParcelableBlendMode& blendMode) {
    auto err = mHal->setLayerBlendMode(display, layer, blendMode.blendMode);
    if (err) {
        LOG(ERROR) << __func__ << ": err " << err;
        mWriter->setError(mCommandIndex, err);
    }
}

void ComposerCommandEngine::executeSetLayerColor(int64_t display, int64_t layer,
                                                 const Color& color) {
    auto err = mHal->setLayerColor(display, layer, color);
    if (err) {
        LOG(ERROR) << __func__ << ": err " << err;
        mWriter->setError(mCommandIndex, err);
    }
}

void ComposerCommandEngine::executeSetLayerComposition(int64_t display, int64_t layer,
                                                       const ParcelableComposition& composition) {
    auto err = mHal->setLayerCompositionType(display, layer, composition.composition);
    if (err) {
        LOG(ERROR) << __func__ << ": err " << err;
        mWriter->setError(mCommandIndex, err);
    }
}

void ComposerCommandEngine::executeSetLayerDataspace(int64_t display, int64_t layer,
                                                     const ParcelableDataspace& dataspace) {
    auto err = mHal->setLayerDataspace(display, layer, dataspace.dataspace);
    if (err) {
        LOG(ERROR) << __func__ << ": err " << err;
        mWriter->setError(mCommandIndex, err);
    }
}

void ComposerCommandEngine::executeSetLayerDisplayFrame(int64_t display, int64_t layer,
                                                        const common::Rect& rect) {
    auto err = mHal->setLayerDisplayFrame(display, layer, rect);
    if (err) {
        LOG(ERROR) << __func__ << ": err " << err;
        mWriter->setError(mCommandIndex, err);
    }
}

void ComposerCommandEngine::executeSetLayerPlaneAlpha(int64_t display, int64_t layer,
                                                      const PlaneAlpha& planeAlpha) {
    auto err = mHal->setLayerPlaneAlpha(display, layer, planeAlpha.alpha);
    if (err) {
        LOG(ERROR) << __func__ << ": err " << err;
        mWriter->setError(mCommandIndex, err);
    }
}

void ComposerCommandEngine::executeSetLayerSidebandStream(int64_t display, int64_t layer,
                                                 const AidlNativeHandle& sidebandStream) {
    buffer_handle_t handle = ::android::makeFromAidl(sidebandStream);
    buffer_handle_t stream;

    auto bufferReleaser = mResources->createReleaser(false);
    auto err = mResources->getLayerSidebandStream(display, layer, handle,
                                                  stream, bufferReleaser.get());
    if (err) {
        err = mHal->setLayerSidebandStream(display, layer, stream);
    }
    if (err) {
        LOG(ERROR) << __func__ << ": err " << err;
        mWriter->setError(mCommandIndex, err);
    }
}

void ComposerCommandEngine::executeSetLayerSourceCrop(int64_t display, int64_t layer,
                                                      const common::FRect& sourceCrop) {
    auto err = mHal->setLayerSourceCrop(display, layer, sourceCrop);
    if (err) {
        LOG(ERROR) << __func__ << ": err " << err;
        mWriter->setError(mCommandIndex, err);
    }
}

void ComposerCommandEngine::executeSetLayerTransform(int64_t display, int64_t layer,
                                                     const ParcelableTransform& transform) {
    auto err = mHal->setLayerTransform(display, layer, transform.transform);
    if (err) {
        LOG(ERROR) << __func__ << ": err " << err;
        mWriter->setError(mCommandIndex, err);
    }
}

void ComposerCommandEngine::executeSetLayerVisibleRegion(int64_t display, int64_t layer,
                          const std::vector<std::optional<common::Rect>>& visibleRegion) {
    auto err = mHal->setLayerVisibleRegion(display, layer, visibleRegion);
    if (err) {
        LOG(ERROR) << __func__ << ": err " << err;
        mWriter->setError(mCommandIndex, err);
    }
}

void ComposerCommandEngine::executeSetLayerZOrder(int64_t display, int64_t layer,
                                                  const ZOrder& zOrder) {
    auto err = mHal->setLayerZOrder(display, layer, zOrder.z);
    if (err) {
        LOG(ERROR) << __func__ << ": err " << err;
        mWriter->setError(mCommandIndex, err);
    }
}

void ComposerCommandEngine::executeSetLayerPerFrameMetadata(int64_t display, int64_t layer,
                const std::vector<std::optional<PerFrameMetadata>>& perFrameMetadata) {
    auto err = mHal->setLayerPerFrameMetadata(display, layer, perFrameMetadata);
    if (err) {
        LOG(ERROR) << __func__ << ": err " << err;
        mWriter->setError(mCommandIndex, err);
    }
}

void ComposerCommandEngine::executeSetLayerColorTransform(int64_t display, int64_t layer,
                                                       const std::vector<float>& matrix) {
    auto err = mHal->setLayerColorTransform(display, layer, matrix);
    if (err) {
        LOG(ERROR) << __func__ << ": err " << err;
        mWriter->setError(mCommandIndex, err);
    }
}

void ComposerCommandEngine::executeSetLayerPerFrameMetadataBlobs(int64_t display, int64_t layer,
                      const std::vector<std::optional<PerFrameMetadataBlob>>& metadata) {
    auto err = mHal->setLayerPerFrameMetadataBlobs(display, layer, metadata);
    if (err) {
        LOG(ERROR) << __func__ << ": err " << err;
        mWriter->setError(mCommandIndex, err);
    }
}

} // namespace aidl::android::hardware::graphics::composer3::impl
