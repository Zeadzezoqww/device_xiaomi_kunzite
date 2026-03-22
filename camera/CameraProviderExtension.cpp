/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "CameraProviderExtension.h"

#include <fstream>

#define TORCH_BRIGHTNESS      "brightness"
#define TORCH_MAX_BRIGHTNESS  "max_brightness"
#define SWITCH_ON             255

static std::string kTorch0Path =
    "/sys/devices/platform/soc/c42d000.qcom,spmi/spmi-0/0-05/"
    "c42d000.qcom,spmi:qcom,pm6150l@5:qcom,leds@d300/leds/led:torch_0";
static std::string kTorch1Path =
    "/sys/devices/platform/soc/c42d000.qcom,spmi/spmi-0/0-05/"
    "c42d000.qcom,spmi:qcom,pm6150l@5:qcom,leds@d300/leds/led:torch_1";
static std::string kSwitch0Path =
    "/sys/devices/platform/soc/c42d000.qcom,spmi/spmi-0/0-05/"
    "c42d000.qcom,spmi:qcom,pm6150l@5:qcom,leds@d300/leds/led:switch_0";
static std::string kSwitch1Path =
    "/sys/devices/platform/soc/c42d000.qcom,spmi/spmi-0/0-05/"
    "c42d000.qcom,spmi:qcom,pm6150l@5:qcom,leds@d300/leds/led:switch_1";

template <typename T>
static void set(const std::string& path, const T& value) {
    std::ofstream file(path);
    file << value;
}

template <typename T>
static T get(const std::string& path, const T& def) {
    std::ifstream file(path);
    T result;
    file >> result;
    return file.fail() ? def : result;
}

bool supportsTorchStrengthControlExt() {
    return true;
}

bool supportsSetTorchModeExt() {
    return false;
}

int32_t getTorchDefaultStrengthLevelExt() {
    return 100;
}

int32_t getTorchMaxStrengthLevelExt() {
    return 500;
}

int32_t getTorchStrengthLevelExt() {
    return get(kTorch0Path + "/" + TORCH_BRIGHTNESS, 0);
}

void setTorchStrengthLevelExt(int32_t torchStrength, bool enabled) {
    set(kSwitch0Path + "/" + TORCH_BRIGHTNESS, 0);
    set(kSwitch1Path + "/" + TORCH_BRIGHTNESS, 0);

    set(kTorch0Path + "/" + TORCH_BRIGHTNESS, torchStrength);
    set(kTorch1Path + "/" + TORCH_BRIGHTNESS, torchStrength);

    if (enabled) {
        set(kSwitch0Path + "/" + TORCH_BRIGHTNESS, SWITCH_ON);
        set(kSwitch1Path + "/" + TORCH_BRIGHTNESS, SWITCH_ON);
    }
}

void setTorchModeExt(bool enabled) {
    int32_t strength = getTorchDefaultStrengthLevelExt();
    setTorchStrengthLevelExt(enabled ? strength : 0, enabled);
}
