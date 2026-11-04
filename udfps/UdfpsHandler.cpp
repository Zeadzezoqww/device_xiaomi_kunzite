/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "UdfpsHandler.kunzite"

#include <aidl/android/hardware/biometrics/fingerprint/BnFingerprint.h>
#include <android-base/logging.h>
#include <android-base/unique_fd.h>

#include <atomic>
#include <bitset>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

#include <display/drm/mi_disp.h>

#include "UdfpsHandler.h"

#define CMD_DATA_BUF_SIZE 256

#define COMMON_DATA_CMD 0
#define SELECT_TOUCH_ID 3
#define SET_CUR_VALUE 0

#define Touch_Fod_Enable 10
#define THP_FOD_DOWNUP_CTL 1001

#define COMMAND_NIT 10
#define PARAM_NIT_FOD 1
#define PARAM_NIT_NONE 0

#define COMMAND_FOD_PRESS_STATUS 1
#define PARAM_FOD_PRESSED 1
#define PARAM_FOD_RELEASED 0

#define FOD_STATUS_OFF 0
#define FOD_STATUS_ON 1

#define TOUCH_DEV_PATH "/dev/xiaomi-touch"
#define TOUCH_MAGIC 0x54

#define DISP_FEATURE_PATH "/dev/mi_display/disp_feature"

// vendorCode values from onAcquired()
#define VENDOR_ACQUIRED_WAITING_FOR_FINGER 21

typedef struct {
    int8_t touch_id;
    uint8_t cmd;
    uint16_t mode;
    uint16_t data_len;
    int32_t data_buf[CMD_DATA_BUF_SIZE];
} touch_base;

#define TOUCH_IOC_SELECT_TOUCH_ID _IOW(TOUCH_MAGIC, SELECT_TOUCH_ID, int)
#define TOUCH_IOC_COMMON_DATA _IOW(TOUCH_MAGIC, COMMON_DATA_CMD, touch_base)

using ::aidl::android::hardware::biometrics::fingerprint::AcquiredInfo;

namespace {

// A late onFingerDown() arriving within this window after a successful
// authentication belongs to the attempt that just finished. After it, any
// onFingerDown() is a new attempt.
constexpr int64_t kLateDownWindowMs = 300;

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

disp_event_resp* parseDispEvent(int fd) {
    static char event_data[1024];
    memset(event_data, 0, sizeof(event_data));

    ssize_t size = read(fd, event_data, sizeof(event_data));
    if (size < 0) {
        LOG(ERROR) << "read fod event failed";
        return nullptr;
    }
    if (static_cast<size_t>(size) < sizeof(struct disp_event)) {
        LOG(ERROR) << "Invalid event size " << size << ", expect at least "
                   << sizeof(struct disp_event);
        return nullptr;
    }

    return reinterpret_cast<disp_event_resp*>(event_data);
}

struct disp_base displayBasePrimary = {
        .flag = 0,
        .disp_id = MI_DISP_PRIMARY,
};

touch_base touchDataPrimary = {
        .touch_id = MI_DISP_PRIMARY,
        .cmd = SET_CUR_VALUE,
        .mode = 0,
        .data_len = 1,
        .data_buf = {},
};

}  // anonymous namespace

class XiaomiKunziteUdfpsHandler : public UdfpsHandler {
  public:
    void init(fingerprint_device_t* device) {
        mDevice = device;
        touch_fd_ = android::base::unique_fd(open(TOUCH_DEV_PATH, O_RDWR));
        disp_fd_ = android::base::unique_fd(open(DISP_FEATURE_PATH, O_RDWR));

        std::thread([this]() { listenFodEvents(); }).detach();
    }

    void onFingerDown(uint32_t /*x*/, uint32_t /*y*/, float /*minor*/, float /*major*/) {
        // Runs on the binder/input thread and can race with authentication
        // completing on the HAL callback thread, so drop a late "down" right
        // after a success. Past the window this is a new attempt: clear the
        // flag here, since the FPC HAL neither emits vendorCode 21 nor gets
        // cancel() called between attempts.
        if (mAuthSuccess.load(std::memory_order_acquire)) {
            int64_t elapsed = nowMs() - mAuthSuccessMs.load(std::memory_order_relaxed);
            if (elapsed < kLateDownWindowMs) {
                LOG(INFO) << __func__ << " ignored: late down " << elapsed
                          << "ms after auth success";
                return;
            }
            mAuthSuccess.store(false, std::memory_order_release);
        }

        LOG(INFO) << __func__;
        mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS, PARAM_FOD_PRESSED);
        setLocalHbm(LHBM_TARGET_BRIGHTNESS_WHITE_1000NIT);
        setTouchMode(THP_FOD_DOWNUP_CTL, 1);
    }

    void onFingerUp() {
        LOG(INFO) << __func__;
        mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS, PARAM_FOD_RELEASED);
        setLocalHbm(LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP);
        setTouchMode(THP_FOD_DOWNUP_CTL, 0);
    }

    void onAcquired(int32_t result, int32_t vendorCode) {
        LOG(INFO) << __func__ << " result: " << result << " vendorCode: " << vendorCode;

        switch (static_cast<AcquiredInfo>(result)) {
            case AcquiredInfo::GOOD:
            case AcquiredInfo::PARTIAL:
            case AcquiredInfo::INSUFFICIENT:
            case AcquiredInfo::SENSOR_DIRTY:
            case AcquiredInfo::TOO_SLOW:
            case AcquiredInfo::TOO_FAST:
            case AcquiredInfo::TOO_DARK:
            case AcquiredInfo::TOO_BRIGHT:
            case AcquiredInfo::IMMOBILE:
            case AcquiredInfo::LIFT_TOO_SOON:
                setLocalHbm(LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP);
                break;
            default:
                break;
        }

        if (vendorCode == VENDOR_ACQUIRED_WAITING_FOR_FINGER) {
            mAuthSuccess.store(false, std::memory_order_release);
            setTouchMode(Touch_Fod_Enable, FOD_STATUS_ON);
        }
    }

    void onAuthenticationSucceeded() {
        LOG(INFO) << __func__;
        // Timestamp first, so a reader that sees the flag (acquire) also sees it.
        mAuthSuccessMs.store(nowMs(), std::memory_order_relaxed);
        mAuthSuccess.store(true, std::memory_order_release);
        onFingerUp();
    }

    void onAuthenticationFailed() {
        LOG(INFO) << __func__;
        mAuthSuccess.store(false, std::memory_order_release);
        onFingerUp();
    }

    void cancel() {
        LOG(INFO) << __func__;
        mAuthSuccess.store(false, std::memory_order_release);
        setTouchMode(Touch_Fod_Enable, FOD_STATUS_OFF);
        setLocalHbm(LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP);
    }

  private:
    fingerprint_device_t* mDevice;
    android::base::unique_fd touch_fd_;
    android::base::unique_fd disp_fd_;

    // Shared between the binder/input thread and the HAL callback thread.
    std::atomic<bool> mAuthSuccess{false};
    std::atomic<int64_t> mAuthSuccessMs{0};

    void listenFodEvents() {
        android::base::unique_fd fd(open(DISP_FEATURE_PATH, O_RDWR));
        if (fd < 0) {
            LOG(ERROR) << "failed to open " << DISP_FEATURE_PATH;
            return;
        }

        struct disp_event_req displayEventRequest = {
                .base = displayBasePrimary,
                .type = MI_DISP_EVENT_FOD,
        };
        if (ioctl(fd.get(), MI_DISP_IOCTL_REGISTER_EVENT, &displayEventRequest) < 0) {
            LOG(ERROR) << "failed to register FOD event";
            return;
        }

        struct pollfd dispEventPoll = {
                .fd = fd.get(),
                .events = POLLIN,
                .revents = 0,
        };

        while (true) {
            if (poll(&dispEventPoll, 1, -1) < 0) {
                LOG(ERROR) << "failed to poll " << DISP_FEATURE_PATH;
                usleep(10 * 1000);  // avoid a busy loop if poll() keeps failing
                continue;
            }

            struct disp_event_resp* response = parseDispEvent(fd.get());
            if (response == nullptr) {
                continue;
            }
            if (response->base.type != MI_DISP_EVENT_FOD) {
                LOG(ERROR) << "unexpected display event: " << response->base.type;
                continue;
            }

            int value = response->data[0];
            LOG(DEBUG) << "received data: " << std::bitset<8>(value);

            bool localHbmUiReady = value & LOCAL_HBM_UI_READY;
            mDevice->extCmd(mDevice, COMMAND_NIT,
                            localHbmUiReady ? PARAM_NIT_FOD : PARAM_NIT_NONE);
        }
    }

    void setLocalHbm(int value) {
        struct disp_local_hbm_req request = {};
        request.base = displayBasePrimary;
        request.local_hbm_value = value;
        ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &request);
    }

    void setTouchMode(uint16_t mode, int value) {
        ioctl(touch_fd_.get(), TOUCH_IOC_SELECT_TOUCH_ID, MI_DISP_PRIMARY);
        touch_base data = touchDataPrimary;
        data.mode = mode;
        data.data_buf[0] = value;
        ioctl(touch_fd_.get(), TOUCH_IOC_COMMON_DATA, &data);
    }
};

static UdfpsHandler* create() {
    return new XiaomiKunziteUdfpsHandler();
}

static void destroy(UdfpsHandler* handler) {
    delete handler;
}

extern "C" UdfpsHandlerFactory UDFPS_HANDLER_FACTORY = {
        .create = create,
        .destroy = destroy,
};
