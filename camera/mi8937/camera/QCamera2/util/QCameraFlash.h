/* Copyright (c) 2015-2016, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef __QCAMERA_FLASH_H__
#define __QCAMERA_FLASH_H__

// Camera dependencies
#include "hardware/camera_common.h"

extern "C" {
#include "mm_camera_interface.h"
}

namespace qcamera {

/* Torch strength levels, exposed to the framework as
 * ANDROID_FLASH_INFO_STRENGTH_MAXIMUM_LEVEL / _DEFAULT_LEVEL and driven from
 * the SystemUI flashlight slider via ICameraDevice::turnOnTorchWithStrengthLevel.
 *
 * Levels map to a torch current in mA, which msm_flash passes straight through
 * to the PMI8950 torch LED (led:torch_0).  Two hard constraints from the kernel
 * and DTS:
 *
 *   - pepito's qcom,torch_0 has qcom,max-current = <200> and qcom,current =
 *     <120> (the operating/fallback current).
 *   - msm_flash_low() honours a requested current only when
 *     `req >= 0 && req < max_current` -- STRICTLY less than.  Anything else
 *     silently falls back to qcom,current.
 *
 * ⭐ That is why the stock QCAMERA_TORCH_CURRENT_VALUE of 200 never took
 * effect: 200 < 200 is false, so every torch-on landed on the 120 mA fallback.
 * The top level therefore asks for 199, not 200.  Level 3 is the default and
 * reproduces the historical 120 mA exactly, so an untouched slider behaves
 * like the old build.
 */
#define QCAMERA_TORCH_LEVEL_MAX     5
#define QCAMERA_TORCH_LEVEL_DEFAULT 3

/* Level (1..QCAMERA_TORCH_LEVEL_MAX) -> torch current in mA. The table lives in
 * QCameraFlash.cpp rather than here: this header is pulled into four TUs, two of
 * which build with -Werror and would not reference it. */
int32_t qcameraTorchLevelToCurrentMa(int32_t level);

class QCameraFlash {
public:
    static QCameraFlash& getInstance();

    int32_t registerCallbacks(const camera_module_callbacks_t* callbacks);
    int32_t initFlash(const int camera_id);
    int32_t setFlashMode(const int camera_id, const bool on);
    int32_t deinitFlash(const int camera_id);
    int32_t reserveFlashForCamera(const int camera_id);
    int32_t releaseFlashFromCamera(const int camera_id);

    /* Torch strength. Level is 1..QCAMERA_TORCH_LEVEL_MAX; setTorchLevel()
     * re-issues CFG_FLASH_LOW at the new current when the torch is already on,
     * so the slider updates live. */
    int32_t setTorchLevel(const int camera_id, const int level);
    int32_t getTorchLevel(const int camera_id);

private:
    QCameraFlash();
    virtual ~QCameraFlash();
    QCameraFlash(const QCameraFlash&);
    QCameraFlash& operator=(const QCameraFlash&);

    /* Issue CFG_FLASH_LOW/CFG_FLASH_OFF at the camera's current torch level. */
    int32_t applyFlashState(const int camera_id, const bool on);

    const camera_module_callbacks_t *m_callbacks;
    int32_t m_flashFds[MM_CAMERA_MAX_NUM_SENSORS];
    bool m_flashOn[MM_CAMERA_MAX_NUM_SENSORS];
    bool m_cameraOpen[MM_CAMERA_MAX_NUM_SENSORS];
    int32_t m_torchLevel[MM_CAMERA_MAX_NUM_SENSORS];
};

}; // namespace qcamera

#endif /* __QCAMERA_FLASH_H__ */
