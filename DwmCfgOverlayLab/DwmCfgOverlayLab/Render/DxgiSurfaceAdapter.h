#pragma once
#include "FrameTarget.h"
namespace dwm_overlay {
class DxgiSurfaceAdapter final {
public:
    bool Acquire(void* presentationObject, FrameTarget& frame) noexcept;
};
} // namespace dwm_overlay
