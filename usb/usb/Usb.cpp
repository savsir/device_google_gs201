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

#define LOG_TAG "android.hardware.usb.aidl-service"

#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <assert.h>
#include <cstring>
#include <dirent.h>
#include <private/android_filesystem_config.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <usbhost/usbhost.h>
#include <regex>
#include <thread>
#include <unordered_map>

#include <cutils/uevent.h>
#include <sys/epoll.h>
#include <utils/Errors.h>
#include <utils/StrongPointer.h>
#include <utils/Vector.h>

#include "Usb.h"

#include <aidl/android/frameworks/stats/IStats.h>
#include <android_hardware_usb_flags.h>
#include <pixelstats/StatsHelper.h>
#include <pixelusb/I2cHelper.h>
#include <pixelusb/UsbGadgetAidlCommon.h>

namespace usb_flags = android::hardware::usb::flags;

using aidl::android::frameworks::stats::IStats;
using android::base::GetProperty;
using android::base::ParseInt;
using android::base::Tokenize;
using android::base::Trim;
using android::hardware::google::pixel::getStatsService;
using android::hardware::google::pixel::PixelAtoms::VendorUsbPortOverheat;
using android::hardware::google::pixel::reportUsbPortOverheat;
using android::hardware::google::pixel::usb::getI2cClientPath;
using android::String8;
using android::Vector;

namespace aidl {
namespace android {
namespace hardware {
namespace usb {
// Set by the signal handler to destroy the thread
volatile bool destroyThread;

string enabledPath;
constexpr char kHsi2cPath[] = "/sys/devices/platform/10d60000.hsi2c";
constexpr char kTcpcDevName[] = "i2c-max77759tcpc";
constexpr char kI2cClientId[] = "0025";
constexpr char kComplianceWarningsPath[] = "device/non_compliant_reasons";
constexpr char kComplianceWarningBC12[] = "bc12";
constexpr char kComplianceWarningDebugAccessory[] = "debug-accessory";
constexpr char kComplianceWarningMissingRp[] = "missing_rp";
constexpr char kComplianceWarningOther[] = "other";
constexpr char kComplianceWarningInputPowerLimited[] = "input_power_limited";
constexpr char kContaminantDetectionPath[] = "contaminant_detection";
constexpr char kStatusPath[] = "contaminant_detection_status";
constexpr char kSinkLimitEnable[] = "usb_limit_sink_enable";
constexpr char kSourceLimitEnable[] = "usb_limit_source_enable";
constexpr char kSinkLimitCurrent[] = "usb_limit_sink_current";
constexpr char kTypecPath[] = "/sys/class/typec";
constexpr char kDisableContatminantDetection[] = "vendor.usb.contaminantdisable";
constexpr char kOverheatStatsPath[] = "/sys/devices/platform/google,usbc_port_cooling_dev/";
constexpr char kOverheatStatsDev[] = "DRIVER=google,usbc_port_cooling_dev";
constexpr char kThermalZoneForTrip[] = "VIRTUAL-USB-THROTTLING";
constexpr char kThermalZoneForTempReadPrimary[] = "usb_pwr_therm2";
constexpr char kThermalZoneForTempReadSecondary1[] = "usb_pwr_therm";
constexpr char kThermalZoneForTempReadSecondary2[] = "qi_therm";
constexpr char kPogoUsbActive[] = "/sys/devices/platform/google,pogo/pogo_usb_active";
constexpr char kPogoEnableHub[] = "/sys/devices/platform/google,pogo/enable_hub";
constexpr char kInternalHubDevnum[] = "/sys/bus/usb/devices/1-1/devnum";
constexpr char KPogoMoveDataToUsb[] = "/sys/devices/platform/google,pogo/move_data_to_usb";
constexpr char kPowerSupplyUsbType[] = "/sys/class/power_supply/usb/usb_type";
constexpr char kUdcUeventRegex[] =
    "/devices/platform/11210000.usb/11210000.dwc3/udc/11210000.dwc3";
constexpr char kUdcStatePath[] =
    "/sys/devices/platform/11210000.usb/11210000.dwc3/udc/11210000.dwc3/state";
constexpr char kHost1UeventRegex[] =
    "/devices/platform/11210000.usb/11210000.dwc3/xhci-hcd-exynos.[0-9].auto/usb2/2-0:1.0";
constexpr char kHost1StatePath[] = "/sys/bus/usb/devices/usb2/2-0:1.0/usb2-port1/state";
constexpr char kHost2UeventRegex[] =
    "/devices/platform/11210000.usb/11210000.dwc3/xhci-hcd-exynos.[0-9].auto/usb3/3-0:1.0";
constexpr char kHost2StatePath[] = "/sys/bus/usb/devices/usb3/3-0:1.0/usb3-port1/state";
constexpr char kDataRolePath[] = "/sys/devices/platform/11210000.usb/new_data_role";

constexpr int kSamplingIntervalSec = 5;
void queryVersionHelper(android::hardware::usb::Usb *usb,
                        std::vector<PortStatus> *currentPortStatus);

#define CTRL_TRANSFER_TIMEOUT_MSEC 1000
#define GL852G_VENDOR_ID 0x05e3
#define GL852G_PRODUCT_ID1 0x0608
#define GL852G_PRODUCT_ID2 0x0610
#define GL852G_VENDOR_CMD_REQ 0xe3
// GL852G port 1 and port 2 JK level default settings
#define GL852G_VENDOR_CMD_VALUE_DEFAULT 0x0008
#define GL852G_VENDOR_CMD_INDEX_DEFAULT 0x0404

ScopedAStatus Usb::enableUsbData(const string& in_portName, bool in_enable,
        int64_t in_transactionId) {
    bool result = true;
    std::vector<PortStatus> currentPortStatus;
    string pullup;

    ALOGI("Userspace turn %s USB data signaling. opID:%ld", in_enable ? "on" : "off",
            in_transactionId);

    if (in_enable) {
        if (!mUsbDataEnabled) {
            if (ReadFileToString(PULLUP_PATH, &pullup)) {
                pullup = Trim(pullup);
                if (pullup != kGadgetName) {
                    if (!WriteStringToFile(kGadgetName, PULLUP_PATH)) {
                        ALOGE("Gadget cannot be pulled up");
                        result = false;
                    }
                }
            }

            if (!WriteStringToFile("1", USB_DATA_PATH)) {
                ALOGE("Not able to turn on usb connection notification");
                result = false;
            }
        }
    } else {
        if (ReadFileToString(PULLUP_PATH, &pullup)) {
            pullup = Trim(pullup);
            if (pullup == kGadgetName) {
                if (!WriteStringToFile("none", PULLUP_PATH)) {
                    ALOGE("Gadget cannot be pulled down");
                    result = false;
                }
            }
        }

        if (!WriteStringToFile("1", ID_PATH)) {
            ALOGE("Not able to turn off host mode");
            result = false;
        }

        if (!WriteStringToFile("0", VBUS_PATH)) {
            ALOGE("Not able to set Vbus state");
            result = false;
        }

        if (!WriteStringToFile("0", USB_DATA_PATH)) {
            ALOGE("Not able to turn off usb connection notification");
            result = false;
        }
    }

    if (result) {
        mUsbDataEnabled = in_enable;
    }
    pthread_mutex_lock(&mLock);
    if (mCallback != NULL) {
        ScopedAStatus ret = mCallback->notifyEnableUsbDataStatus(
            in_portName, in_enable, result ? Status::SUCCESS : Status::ERROR, in_transactionId);
        if (!ret.isOk())
            ALOGE("notifyEnableUsbDataStatus error %s", ret.getDescription().c_str());
    } else {
        ALOGE("Not notifying the userspace. Callback is not set");
    }
    pthread_mutex_unlock(&mLock);
    queryVersionHelper(this, &currentPortStatus);

    return ScopedAStatus::ok();
}

ScopedAStatus Usb::enableUsbDataWhileDocked(const string& in_portName,
        int64_t in_transactionId) {
    bool success = true;
    bool notSupported = true;
    std::vector<PortStatus> currentPortStatus;

    ALOGI("Userspace enableUsbDataWhileDocked  opID:%ld", in_transactionId);

    int flags = O_RDONLY;
    ::android::base::unique_fd fd(TEMP_FAILURE_RETRY(open(KPogoMoveDataToUsb, flags)));
    if (fd != -1) {
        notSupported = false;
        success = WriteStringToFile("1", KPogoMoveDataToUsb);
        if (!success) {
            ALOGE("Write to move_data_to_usb failed");
        }
    }

    pthread_mutex_lock(&mLock);
    if (mCallback != NULL) {
        ScopedAStatus ret = mCallback->notifyEnableUsbDataWhileDockedStatus(
                in_portName, notSupported ? Status::NOT_SUPPORTED :
                success ? Status::SUCCESS : Status::ERROR, in_transactionId);
        if (!ret.isOk())
            ALOGE("notifyEnableUsbDataStatus error %s", ret.getDescription().c_str());
    } else {
        ALOGE("Not notifying the userspace. Callback is not set");
    }
    pthread_mutex_unlock(&mLock);
    queryVersionHelper(this, &currentPortStatus);

    return ScopedAStatus::ok();
}

ScopedAStatus Usb::resetUsbPort(const std::string& in_portName, int64_t in_transactionId) {
    bool result = true;
    std::vector<PortStatus> currentPortStatus;

    ALOGI("Userspace reset USB Port. opID:%ld", in_transactionId);

    if (!WriteStringToFile("none", PULLUP_PATH)) {
        ALOGI("Gadget cannot be pulled down");
        result = false;
    }

    pthread_mutex_lock(&mLock);
    if (mCallback != NULL) {
        ::ndk::ScopedAStatus ret = mCallback->notifyResetUsbPortStatus(
            in_portName, result ? Status::SUCCESS : Status::ERROR, in_transactionId);
        if (!ret.isOk())
            ALOGE("notifyTransactionStatus error %s", ret.getDescription().c_str());
    } else {
        ALOGE("Not notifying the userspace. Callback is not set");
    }
    pthread_mutex_unlock(&mLock);

    return ::ndk::ScopedAStatus::ok();
}

Status queryMoistureDetectionStatus(android::hardware::usb::Usb *usb,
                                    std::vector<PortStatus> *currentPortStatus) {
    string enabled, status, DetectedPath;

    (*currentPortStatus)[0].supportedContaminantProtectionModes
            .push_back(ContaminantProtectionMode::FORCE_DISABLE);
    (*currentPortStatus)[0].contaminantProtectionStatus = ContaminantProtectionStatus::NONE;
    (*currentPortStatus)[0].contaminantDetectionStatus = ContaminantDetectionStatus::DISABLED;
    (*currentPortStatus)[0].supportsEnableContaminantPresenceDetection = true;
    (*currentPortStatus)[0].supportsEnableContaminantPresenceProtection = false;

    if (usb->mI2cClientPath.empty()) {
        usb->mI2cClientPath = getI2cClientPath(kHsi2cPath, kTcpcDevName, kI2cClientId);
        if (usb->mI2cClientPath.empty()) {
            ALOGE("%s: Unable to locate i2c bus node", __func__);
            return Status::ERROR;
        }
    }

    enabledPath = usb->mI2cClientPath + kContaminantDetectionPath;
    if (!ReadFileToString(enabledPath, &enabled)) {
        ALOGE("Failed to open moisture_detection_enabled");
        return Status::ERROR;
    }

    enabled = Trim(enabled);
    if (enabled == "1") {
        DetectedPath = usb->mI2cClientPath + kStatusPath;
        if (!ReadFileToString(DetectedPath, &status)) {
            ALOGE("Failed to open moisture_detected");
            return Status::ERROR;
        }
        status = Trim(status);
        if (status == "1") {
            (*currentPortStatus)[0].contaminantDetectionStatus =
                ContaminantDetectionStatus::DETECTED;
            (*currentPortStatus)[0].contaminantProtectionStatus =
                ContaminantProtectionStatus::FORCE_DISABLE;
        } else {
            (*currentPortStatus)[0].contaminantDetectionStatus =
                ContaminantDetectionStatus::NOT_DETECTED;
        }
    }

    ALOGI("ContaminantDetectionStatus:%d ContaminantProtectionStatus:%d",
            (*currentPortStatus)[0].contaminantDetectionStatus,
            (*currentPortStatus)[0].contaminantProtectionStatus);

    return Status::SUCCESS;
}

Status queryNonCompliantChargerStatus(std::vector<PortStatus> *currentPortStatus) {
    string reasons, path;

    for (int i = 0; i < currentPortStatus->size(); i++) {
        (*currentPortStatus)[i].supportsComplianceWarnings = true;
        path = string(kTypecPath) + "/" + (*currentPortStatus)[i].portName + "/" +
                string(kComplianceWarningsPath);
        if (ReadFileToString(path.c_str(), &reasons)) {
            std::vector<string> reasonsList = Tokenize(reasons.c_str(), "[], \n\0");
            for (string reason : reasonsList) {
                if (!strncmp(reason.c_str(), kComplianceWarningDebugAccessory,
                            strlen(kComplianceWarningDebugAccessory))) {
                    (*currentPortStatus)[i].complianceWarnings.push_back(ComplianceWarning::DEBUG_ACCESSORY);
                    continue;
                }
                if (!strncmp(reason.c_str(), kComplianceWarningBC12,
                            strlen(kComplianceWarningBC12))) {
                    (*currentPortStatus)[i].complianceWarnings.push_back(ComplianceWarning::BC_1_2);
                    continue;
                }
                if (!strncmp(reason.c_str(), kComplianceWarningMissingRp,
                            strlen(kComplianceWarningMissingRp))) {
                    (*currentPortStatus)[i].complianceWarnings.push_back(ComplianceWarning::MISSING_RP);
                    continue;
                }
                if (!strncmp(reason.c_str(), kComplianceWarningOther,
                             strlen(kComplianceWarningOther)) ||
                    !strncmp(reason.c_str(), kComplianceWarningInputPowerLimited,
                             strlen(kComplianceWarningInputPowerLimited))) {
                    if (usb_flags::enable_usb_data_compliance_warning() &&
                        usb_flags::enable_input_power_limited_warning()) {
                        ALOGI("Report through INPUT_POWER_LIMITED warning");
                        (*currentPortStatus)[i].complianceWarnings.push_back(
                            ComplianceWarning::INPUT_POWER_LIMITED);
                        continue;
                    } else {
                        (*currentPortStatus)[i].complianceWarnings.push_back(
                            ComplianceWarning::OTHER);
                        continue;
                    }
                }
            }
            if ((*currentPortStatus)[i].complianceWarnings.size() > 0 &&
                 (*currentPortStatus)[i].currentPowerRole == PortPowerRole::NONE) {
                (*currentPortStatus)[i].currentMode = PortMode::UFP;
                (*currentPortStatus)[i].currentPowerRole = PortPowerRole::SINK;
                (*currentPortStatus)[i].currentDataRole = PortDataRole::NONE;
                (*currentPortStatus)[i].powerBrickStatus = PowerBrickStatus::CONNECTED;
            }
        }
    }
    return Status::SUCCESS;
}

string appendRoleNodeHelper(const string &portName, PortRole::Tag tag) {
    string node("/sys/class/typec/" + portName);

    switch (tag) {
        case PortRole::dataRole:
            return node + "/data_role";
        case PortRole::powerRole:
            return node + "/power_role";
        case PortRole::mode:
            return node + "/port_type";
        default:
            return "";
    }
}

string convertRoletoString(PortRole role) {
    if (role.getTag() == PortRole::powerRole) {
        if (role.get<PortRole::powerRole>() == PortPowerRole::SOURCE)
            return "source";
        else if (role.get<PortRole::powerRole>() == PortPowerRole::SINK)
            return "sink";
    } else if (role.getTag() == PortRole::dataRole) {
        if (role.get<PortRole::dataRole>() == PortDataRole::HOST)
            return "host";
        if (role.get<PortRole::dataRole>() == PortDataRole::DEVICE)
            return "device";
    } else if (role.getTag() == PortRole::mode) {
        if (role.get<PortRole::mode>() == PortMode::UFP)
            return "sink";
        if (role.get<PortRole::mode>() == PortMode::DFP)
            return "source";
    }
    return "none";
}

void extractRole(string *roleName) {
    std::size_t first, last;

    first = roleName->find("[");
    last = roleName->find("]");

    if (first != string::npos && last != string::npos) {
        *roleName = roleName->substr(first + 1, last - first - 1);
    }
}

void switchToDrp(const string &portName) {
    string filename = appendRoleNodeHelper(string(portName.c_str()), PortRole::mode);
    FILE *fp;

    if (filename != "") {
        fp = fopen(filename.c_str(), "w");
        if (fp != NULL) {
            int ret = fputs("dual", fp);
            fclose(fp);
            if (ret == EOF)
                ALOGE("Fatal: Error while switching back to drp");
        } else {
            ALOGE("Fatal: Cannot open file to switch back to drp");
        }
    } else {
        ALOGE("Fatal: invalid node type");
    }
}

bool switchMode(const string &portName, const PortRole &in_role, struct Usb *usb) {
    string filename = appendRoleNodeHelper(string(portName.c_str()), in_role.getTag());
    string written;
    FILE *fp;
    bool roleSwitch = false;

    if (filename == "") {
        ALOGE("Fatal: invalid node type");
        return false;
    }

    fp = fopen(filename.c_str(), "w");
    if (fp != NULL) {
        // Hold the lock here to prevent loosing connected signals
        // as once the file is written the partner added signal
        // can arrive anytime.
        pthread_mutex_lock(&usb->mPartnerLock);
        usb->mPartnerUp = false;
        int ret = fputs(convertRoletoString(in_role).c_str(), fp);
        fclose(fp);

        if (ret != EOF) {
            struct timespec to;
            struct timespec now;

        wait_again:
            clock_gettime(CLOCK_MONOTONIC, &now);
            to.tv_sec = now.tv_sec + PORT_TYPE_TIMEOUT;
            to.tv_nsec = now.tv_nsec;

            int err = pthread_cond_timedwait(&usb->mPartnerCV, &usb->mPartnerLock, &to);
            // There are no uevent signals which implies role swap timed out.
            if (err == ETIMEDOUT) {
                ALOGI("uevents wait timedout");
                // Validity check.
            } else if (!usb->mPartnerUp) {
                goto wait_again;
                // Role switch succeeded since usb->mPartnerUp is true.
            } else {
                roleSwitch = true;
            }
        } else {
            ALOGI("Role switch failed while wrting to file");
        }
        pthread_mutex_unlock(&usb->mPartnerLock);
    }

    if (!roleSwitch)
        switchToDrp(string(portName.c_str()));

    return roleSwitch;
}

static int getInternalHubUniqueId() {
    string internalHubDevnum;
    int devnum = 0, internalHubUniqueId = -1;
    if (ReadFileToString(kInternalHubDevnum, &internalHubDevnum) &&
        ParseInt(Trim(internalHubDevnum).c_str(), &devnum))
        internalHubUniqueId = 1000 + devnum;
    return internalHubUniqueId;
}

static Status tuneInternalHub(const char *devname, void* client_data) {
    uint16_t vendorId, productId;
    struct usb_device *device;
    ::aidl::android::hardware::usb::Usb *usb;
    int value, index;

    device = usb_device_open(devname);
    if (!device) {
        ALOGE("usb_device_open failed\n");
        return Status::ERROR;
    }

    usb = (::aidl::android::hardware::usb::Usb *)client_data;
    value = usb->mUsbHubVendorCmdValue;
    index = usb->mUsbHubVendorCmdIndex;

    // The vendor cmd only applies to USB Hubs of Genesys Logic, Inc.
    // The request field of vendor cmd is fixed to 0xe3.
    vendorId = usb_device_get_vendor_id(device);
    productId = usb_device_get_product_id(device);
    if (vendorId == GL852G_VENDOR_ID &&
        (productId == GL852G_PRODUCT_ID1 || productId == GL852G_PRODUCT_ID2)) {
        int ret = usb_device_control_transfer(device,
            USB_DIR_OUT | USB_TYPE_VENDOR, GL852G_VENDOR_CMD_REQ, value, index,
            NULL, 0, CTRL_TRANSFER_TIMEOUT_MSEC);
        ALOGI("USB hub vendor cmd %s (wValue 0x%x, wIndex 0x%x, return %d)\n",
                ret? "failed" : "succeeded", value, index, ret);
    }

    usb_device_close(device);

    return Status::SUCCESS;
}

static int usbDeviceRemoved(const char *devname, void* client_data) {
    return 0;
}

static int usbDeviceAdded(const char *devname, void* client_data) {
    string pogoEnableHub;
    int uniqueId = 0;

    // Enable hub tuning when the pogo dock is connected.
    if (ReadFileToString(kPogoEnableHub, &pogoEnableHub) && Trim(pogoEnableHub) == "1") {
        // If enable_hub is set to 1, the internal hub is the first enumearted device on bus 1 and
        // port 1.
        uniqueId = usb_device_get_unique_id_from_name(devname);
        if (uniqueId == getInternalHubUniqueId())
            tuneInternalHub(devname, client_data);
    }

    return 0;
}

void *usbHostWork(void *param) {
    struct usb_host_context *ctx;

    ALOGI("creating USB host thread\n");

    ctx = usb_host_init();
    if (!ctx) {
        ALOGE("usb_host_init failed\n");
        return NULL;
    }

    // This will never return, it will keep monitoring USB sysfs inotify events
    usb_host_run(ctx, usbDeviceAdded, usbDeviceRemoved, NULL, param);

    return NULL;
}

void updatePortStatus(android::hardware::usb::Usb *usb) {
    std::vector<PortStatus> currentPortStatus;

    queryVersionHelper(usb, &currentPortStatus);
}

Usb::Usb()
    : mLock(PTHREAD_MUTEX_INITIALIZER),
      mRoleSwitchLock(PTHREAD_MUTEX_INITIALIZER),
      mPartnerLock(PTHREAD_MUTEX_INITIALIZER),
      mPartnerUp(false),
      mUsbDataSessionMonitor(kUdcUeventRegex, kUdcStatePath, kHost1UeventRegex, kHost1StatePath,
                             kHost2UeventRegex, kHost2StatePath, kDataRolePath,
                             std::bind(&updatePortStatus, this)),
      mOverheat(ZoneInfo(TemperatureType::USB_PORT, kThermalZoneForTrip,
                         ThrottlingSeverity::CRITICAL),
                {ZoneInfo(TemperatureType::UNKNOWN, kThermalZoneForTempReadPrimary,
                          ThrottlingSeverity::NONE),
                 ZoneInfo(TemperatureType::UNKNOWN, kThermalZoneForTempReadSecondary1,
                          ThrottlingSeverity::NONE),
                 ZoneInfo(TemperatureType::UNKNOWN, kThermalZoneForTempReadSecondary2,
                          ThrottlingSeverity::NONE)}, kSamplingIntervalSec),
      mUsbDataEnabled(true),
      mUsbHubVendorCmdValue(GL852G_VENDOR_CMD_VALUE_DEFAULT),
      mUsbHubVendorCmdIndex(GL852G_VENDOR_CMD_INDEX_DEFAULT),
      mI2cClientPath("") {
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr)) {
        ALOGE("pthread_condattr_init failed: %s", strerror(errno));
        abort();
    }
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC)) {
        ALOGE("pthread_condattr_setclock failed: %s", strerror(errno));
        abort();
    }
    if (pthread_cond_init(&mPartnerCV, &attr)) {
        ALOGE("pthread_cond_init failed: %s", strerror(errno));
        abort();
    }
    if (pthread_condattr_destroy(&attr)) {
        ALOGE("pthread_condattr_destroy failed: %s", strerror(errno));
        abort();
    }
    if (pthread_create(&mUsbHost, NULL, usbHostWork, this)) {
        ALOGE("pthread creation failed %d\n", errno);
        abort();
    }

    ALOGI("feature flag enable_usb_data_compliance_warning: %d",
          usb_flags::enable_usb_data_compliance_warning());
    ALOGI("feature flag enable_input_power_limited_warning: %d",
          usb_flags::enable_input_power_limited_warning());
}

ScopedAStatus Usb::switchRole(const string& in_portName, const PortRole& in_role,
        int64_t in_transactionId) {
    string filename = appendRoleNodeHelper(string(in_portName.c_str()), in_role.getTag());
    string written;
    FILE *fp;
    bool roleSwitch = false;

    if (filename == "") {
        ALOGE("Fatal: invalid node type");
        return ScopedAStatus::ok();
    }

    pthread_mutex_lock(&mRoleSwitchLock);

    ALOGI("filename write: %s role:%s", filename.c_str(), convertRoletoString(in_role).c_str());

    if (in_role.getTag() == PortRole::mode) {
        roleSwitch = switchMode(in_portName, in_role, this);
    } else {
        fp = fopen(filename.c_str(), "w");
        if (fp != NULL) {
            int ret = fputs(convertRoletoString(in_role).c_str(), fp);
            if (ret == EAGAIN) {
                ALOGI("role switch busy, retry in %d ms", ROLE_SWAP_RETRY_MS);
                std::this_thread::sleep_for(std::chrono::milliseconds(ROLE_SWAP_RETRY_MS));
                ret = fputs(convertRoletoString(in_role).c_str(), fp);
            }
            fclose(fp);
            if ((ret != EOF) && ReadFileToString(filename, &written)) {
                written = Trim(written);
                extractRole(&written);
                ALOGI("written: %s", written.c_str());
                if (written == convertRoletoString(in_role)) {
                    roleSwitch = true;
                } else {
                    ALOGE("Role switch failed");
                }
            } else {
                ALOGE("failed to update the new role");
            }
        } else {
            ALOGE("fopen failed");
        }
    }

    pthread_mutex_lock(&mLock);
    if (mCallback != NULL) {
         ScopedAStatus ret = mCallback->notifyRoleSwitchStatus(
            in_portName, in_role, roleSwitch ? Status::SUCCESS : Status::ERROR, in_transactionId);
        if (!ret.isOk())
            ALOGE("RoleSwitchStatus error %s", ret.getDescription().c_str());
    } else {
        ALOGE("Not notifying the userspace. Callback is not set");
    }
    pthread_mutex_unlock(&mLock);
    pthread_mutex_unlock(&mRoleSwitchLock);

    return ScopedAStatus::ok();
}

ScopedAStatus Usb::limitPowerTransfer(const string& in_portName, bool in_limit,
        int64_t in_transactionId) {
    bool sessionFail = false, success;
    std::vector<PortStatus> currentPortStatus;
    string sinkLimitEnablePath, currentLimitPath, sourceLimitEnablePath;

    if (mI2cClientPath.empty()) {
        mI2cClientPath = getI2cClientPath(kHsi2cPath, kTcpcDevName, kI2cClientId);
        if (mI2cClientPath.empty()) {
            ALOGE("%s: Unable to locate i2c bus node", __func__);
            return ScopedAStatus::ok();
        }
    }

    sinkLimitEnablePath = mI2cClientPath + kSinkLimitEnable;
    currentLimitPath = mI2cClientPath + kSinkLimitCurrent;
    sourceLimitEnablePath = mI2cClientPath + kSourceLimitEnable;

    pthread_mutex_lock(&mLock);
    if (in_limit) {
        success = WriteStringToFile("0", currentLimitPath);
        if (!success) {
            ALOGE("Failed to set sink current limit");
            sessionFail = true;
        }
    }
    success = WriteStringToFile(in_limit ? "1" : "0", sinkLimitEnablePath);
    if (!success) {
        ALOGE("Failed to %s sink current limit: %s", in_limit ? "enable" : "disable",
              sinkLimitEnablePath.c_str());
        sessionFail = true;
    }
    success = WriteStringToFile(in_limit ? "1" : "0", sourceLimitEnablePath);
    if (!success) {
        ALOGE("Failed to %s source current limit: %s", in_limit ? "enable" : "disable",
              sourceLimitEnablePath.c_str());
        sessionFail = true;
    }

    ALOGI("limitPowerTransfer limit:%c opId:%ld", in_limit ? 'y' : 'n', in_transactionId);
    if (mCallback != NULL && in_transactionId >= 0) {
        ScopedAStatus ret = mCallback->notifyLimitPowerTransferStatus(
                in_portName, in_limit, sessionFail ? Status::ERROR : Status::SUCCESS,
                in_transactionId);
        if (!ret.isOk())
            ALOGE("limitPowerTransfer error %s", ret.getDescription().c_str());
    } else {
        ALOGE("Not notifying the userspace. Callback is not set");
    }

    pthread_mutex_unlock(&mLock);
    queryVersionHelper(this, &currentPortStatus);

    return ScopedAStatus::ok();
}

Status queryPowerTransferStatus(android::hardware::usb::Usb *usb,
                                std::vector<PortStatus> *currentPortStatus) {
    string limitedPath, enabled;

    if (usb->mI2cClientPath.empty()) {
        usb->mI2cClientPath = getI2cClientPath(kHsi2cPath, kTcpcDevName, kI2cClientId);
        if (usb->mI2cClientPath.empty()) {
            ALOGE("%s: Unable to locate i2c bus node", __func__);
            return Status::ERROR;
        }
    }

    limitedPath = usb->mI2cClientPath + kSinkLimitEnable;
    if (!ReadFileToString(limitedPath, &enabled)) {
        ALOGE("Failed to open limit_sink_enable");
        return Status::ERROR;
    }

    enabled = Trim(enabled);
    (*currentPortStatus)[0].powerTransferLimited = enabled == "1";

    ALOGI("powerTransferLimited:%d", (*currentPortStatus)[0].powerTransferLimited ? 1 : 0);
    return Status::SUCCESS;
}

Status getAccessoryConnected(const string &portName, string *accessory) {
    string filename = "/sys/class/typec/" + portName + "-partner/accessory_mode";

    if (!ReadFileToString(filename, accessory)) {
        ALOGE("getAccessoryConnected: Failed to open filesystem node: %s", filename.c_str());
        return Status::ERROR;
    }
    *accessory = Trim(*accessory);

    return Status::SUCCESS;
}

Status getCurrentRoleHelper(const string &portName, bool connected, PortRole *currentRole) {
    string filename;
    string roleName;
    string accessory;

    // Mode

    if (currentRole->getTag() == PortRole::powerRole) {
        filename = "/sys/class/typec/" + portName + "/power_role";
        currentRole->set<PortRole::powerRole>(PortPowerRole::NONE);
    } else if (currentRole->getTag() == PortRole::dataRole) {
        filename = "/sys/class/typec/" + portName + "/data_role";
        currentRole->set<PortRole::dataRole>(PortDataRole::NONE);
    } else if (currentRole->getTag() == PortRole::mode) {
        filename = "/sys/class/typec/" + portName + "/data_role";
        currentRole->set<PortRole::mode>(PortMode::NONE);
    } else {
        return Status::ERROR;
    }

    if (!connected)
        return Status::SUCCESS;

    if (currentRole->getTag() == PortRole::mode) {
        if (getAccessoryConnected(portName, &accessory) != Status::SUCCESS) {
            return Status::ERROR;
        }
        if (accessory == "analog_audio") {
            currentRole->set<PortRole::mode>(PortMode::AUDIO_ACCESSORY);
            return Status::SUCCESS;
        } else if (accessory == "debug") {
            currentRole->set<PortRole::mode>(PortMode::DEBUG_ACCESSORY);
            return Status::SUCCESS;
        }
    }

    if (!ReadFileToString(filename, &roleName)) {
        ALOGE("getCurrentRole: Failed to open filesystem node: %s", filename.c_str());
        return Status::ERROR;
    }

    roleName = Trim(roleName);
    extractRole(&roleName);

    if (roleName == "source") {
        currentRole->set<PortRole::powerRole>(PortPowerRole::SOURCE);
    } else if (roleName == "sink") {
        currentRole->set<PortRole::powerRole>(PortPowerRole::SINK);
    } else if (roleName == "host") {
        if (currentRole->getTag() == PortRole::dataRole)
            currentRole->set<PortRole::dataRole>(PortDataRole::HOST);
        else
            currentRole->set<PortRole::mode>(PortMode::DFP);
    } else if (roleName == "device") {
        if (currentRole->getTag() == PortRole::dataRole)
            currentRole->set<PortRole::dataRole>(PortDataRole::DEVICE);
        else
            currentRole->set<PortRole::mode>(PortMode::UFP);
    } else if (roleName != "none") {
        /* case for none has already been addressed.
         * so we check if the role isn't none.
         */
        return Status::UNRECOGNIZED_ROLE;
    }
    return Status::SUCCESS;
}

Status getTypeCPortNamesHelper(std::unordered_map<string, bool> *names) {
    DIR *dp;

    dp = opendir(kTypecPath);
    if (dp != NULL) {
        struct dirent *ep;

        while ((ep = readdir(dp))) {
            if (ep->d_type == DT_LNK) {
                if (string::npos == string(ep->d_name).find("-partner")) {
                    std::unordered_map<string, bool>::const_iterator portName =
                        names->find(ep->d_name);
                    if (portName == names->end()) {
                        names->insert({ep->d_name, false});
                    }
                } else {
                    (*names)[std::strtok(ep->d_name, "-")] = true;
                }
            }
        }
        closedir(dp);
        return Status::SUCCESS;
    }

    ALOGE("Failed to open /sys/class/typec");
    return Status::ERROR;
}

bool canSwitchRoleHelper(const string &portName) {
    string filename = "/sys/class/typec/" + portName + "-partner/supports_usb_power_delivery";
    string supportsPD;

    if (ReadFileToString(filename, &supportsPD)) {
        supportsPD = Trim(supportsPD);
        if (supportsPD == "yes") {
            return true;
        }
    }

    return false;
}

Status getPortStatusHelper(android::hardware::usb::Usb *usb,
        std::vector<PortStatus> *currentPortStatus) {
    std::unordered_map<string, bool> names;
    Status result = getTypeCPortNamesHelper(&names);
    int i = -1;

    if (result == Status::SUCCESS) {
        currentPortStatus->resize(names.size());
        for (std::pair<string, bool> port : names) {
            i++;
            ALOGI("%s", port.first.c_str());
            (*currentPortStatus)[i].portName = port.first;

            PortRole currentRole;
            currentRole.set<PortRole::powerRole>(PortPowerRole::NONE);
            if (getCurrentRoleHelper(port.first, port.second, &currentRole) == Status::SUCCESS) {
                (*currentPortStatus)[i].currentPowerRole = currentRole.get<PortRole::powerRole>();
            } else {
                ALOGE("Error while retrieving portNames");
                goto done;
            }

            currentRole.set<PortRole::dataRole>(PortDataRole::NONE);
            if (getCurrentRoleHelper(port.first, port.second, &currentRole) == Status::SUCCESS) {
                (*currentPortStatus)[i].currentDataRole = currentRole.get<PortRole::dataRole>();
            } else {
                ALOGE("Error while retrieving current port role");
                goto done;
            }

            currentRole.set<PortRole::mode>(PortMode::NONE);
            if (getCurrentRoleHelper(port.first, port.second, &currentRole) == Status::SUCCESS) {
                (*currentPortStatus)[i].currentMode = currentRole.get<PortRole::mode>();
            } else {
                ALOGE("Error while retrieving current data role");
                goto done;
            }

            (*currentPortStatus)[i].canChangeMode = true;
            (*currentPortStatus)[i].canChangeDataRole =
                port.second ? canSwitchRoleHelper(port.first) : false;
            (*currentPortStatus)[i].canChangePowerRole =
                port.second ? canSwitchRoleHelper(port.first) : false;

            (*currentPortStatus)[i].supportedModes.push_back(PortMode::DRP);

            bool dataEnabled = true;
            string pogoUsbActive = "0";
            if (ReadFileToString(string(kPogoUsbActive), &pogoUsbActive) &&
                stoi(Trim(pogoUsbActive)) == 1) {
                /*
                 * Always signal USB device mode disabled irrespective of hub enabled while docked.
                 * Hub gets automatically enabled as needed. Signalling DISABLED_DOCK_HOST_MODE &
                 * DEVICE_MODE during pogo direct can cause notifications to show for brief windows
                 * when the state machine is still moving to steady state.
                 */
                (*currentPortStatus)[i].usbDataStatus.push_back(UsbDataStatus::DISABLED_DOCK_DEVICE_MODE);
                dataEnabled = false;
            }
            if (!usb->mUsbDataEnabled) {
                (*currentPortStatus)[i].usbDataStatus.push_back(UsbDataStatus::DISABLED_FORCE);
                dataEnabled = false;
            }
            if (dataEnabled) {
                (*currentPortStatus)[i].usbDataStatus.push_back(UsbDataStatus::ENABLED);
            }

            // When connected return powerBrickStatus
            if (port.second) {
                string usbType;
                if ((*currentPortStatus)[i].currentPowerRole == PortPowerRole::SOURCE) {
                    (*currentPortStatus)[i].powerBrickStatus = PowerBrickStatus::NOT_CONNECTED;
                } else if (ReadFileToString(string(kPowerSupplyUsbType), &usbType)) {
                    if (strstr(usbType.c_str(), "[D")) {
                        (*currentPortStatus)[i].powerBrickStatus = PowerBrickStatus::CONNECTED;
                    } else if (strstr(usbType.c_str(), "[U")) {
                        (*currentPortStatus)[i].powerBrickStatus = PowerBrickStatus::UNKNOWN;
                    } else {
                        (*currentPortStatus)[i].powerBrickStatus =
                                PowerBrickStatus::NOT_CONNECTED;
                    }
                } else {
                    ALOGE("Error while reading usb_type");
                }
            } else {
                (*currentPortStatus)[i].powerBrickStatus = PowerBrickStatus::NOT_CONNECTED;
            }

            ALOGI("%d:%s connected:%d canChangeMode:%d canChagedata:%d canChangePower:%d "
                  "usbDataEnabled:%d",
                i, port.first.c_str(), port.second,
                (*currentPortStatus)[i].canChangeMode,
                (*currentPortStatus)[i].canChangeDataRole,
                (*currentPortStatus)[i].canChangePowerRole,
                dataEnabled ? 1 : 0);
        }

        return Status::SUCCESS;
    }
done:
    return Status::ERROR;
}

void queryUsbDataSession(android::hardware::usb::Usb *usb,
                          std::vector<PortStatus> *currentPortStatus) {
    std::vector<ComplianceWarning> warnings;

    usb->mUsbDataSessionMonitor.getComplianceWarnings(
        (*currentPortStatus)[0].currentDataRole, &warnings);
    (*currentPortStatus)[0].complianceWarnings.insert(
        (*currentPortStatus)[0].complianceWarnings.end(),
        warnings.begin(),
        warnings.end());
}

void queryVersionHelper(android::hardware::usb::Usb *usb,
                        std::vector<PortStatus> *currentPortStatus) {
    Status status;
    pthread_mutex_lock(&usb->mLock);
    status = getPortStatusHelper(usb, currentPortStatus);
    queryMoistureDetectionStatus(usb, currentPortStatus);
    queryPowerTransferStatus(usb, currentPortStatus);
    queryNonCompliantChargerStatus(currentPortStatus);
    queryUsbDataSession(usb, currentPortStatus);
    if (usb->mCallback != NULL) {
        ScopedAStatus ret = usb->mCallback->notifyPortStatusChange(*currentPortStatus,
            status);
        if (!ret.isOk())
            ALOGE("queryPortStatus error %s", ret.getDescription().c_str());
    } else {
        ALOGI("Notifying userspace skipped. Callback is NULL");
    }
    pthread_mutex_unlock(&usb->mLock);
}

ScopedAStatus Usb::queryPortStatus(int64_t in_transactionId) {
    std::vector<PortStatus> currentPortStatus;

    queryVersionHelper(this, &currentPortStatus);
    pthread_mutex_lock(&mLock);
    if (mCallback != NULL) {
        ScopedAStatus ret = mCallback->notifyQueryPortStatus(
            "all", Status::SUCCESS, in_transactionId);
        if (!ret.isOk())
            ALOGE("notifyQueryPortStatus error %s", ret.getDescription().c_str());
    } else {
        ALOGE("Not notifying the userspace. Callback is not set");
    }
    pthread_mutex_unlock(&mLock);

    return ScopedAStatus::ok();
}

ScopedAStatus Usb::enableContaminantPresenceDetection(const string& in_portName,
        bool in_enable, int64_t in_transactionId) {
    string disable = GetProperty(kDisableContatminantDetection, "");
    std::vector<PortStatus> currentPortStatus;
    bool success = true;

    if (disable != "true")
        success = WriteStringToFile(in_enable ? "1" : "0", enabledPath);

    pthread_mutex_lock(&mLock);
    if (mCallback != NULL) {
        ScopedAStatus ret = mCallback->notifyContaminantEnabledStatus(
            in_portName, in_enable, success ? Status::SUCCESS : Status::ERROR, in_transactionId);
        if (!ret.isOk())
            ALOGE("notifyContaminantEnabledStatus error %s", ret.getDescription().c_str());
    } else {
        ALOGE("Not notifying the userspace. Callback is not set");
    }
    pthread_mutex_unlock(&mLock);

    queryVersionHelper(this, &currentPortStatus);
    return ScopedAStatus::ok();
}

void report_overheat_event(android::hardware::usb::Usb *usb) {
    VendorUsbPortOverheat overheat_info;
    string contents;

    overheat_info.set_plug_temperature_deci_c(usb->mPluggedTemperatureCelsius * 10);
    overheat_info.set_max_temperature_deci_c(usb->mOverheat.getMaxOverheatTemperature() * 10);
    if (ReadFileToString(string(kOverheatStatsPath) + "trip_time", &contents)) {
        overheat_info.set_time_to_overheat_secs(stoi(Trim(contents)));
    } else {
        ALOGE("Unable to read trip_time");
        return;
    }
    if (ReadFileToString(string(kOverheatStatsPath) + "hysteresis_time", &contents)) {
        overheat_info.set_time_to_hysteresis_secs(stoi(Trim(contents)));
    } else {
        ALOGE("Unable to read hysteresis_time");
        return;
    }
    if (ReadFileToString(string(kOverheatStatsPath) + "cleared_time", &contents)) {
        overheat_info.set_time_to_inactive_secs(stoi(Trim(contents)));
    } else {
        ALOGE("Unable to read cleared_time");
        return;
    }

    const shared_ptr<IStats> stats_client = getStatsService();
    if (!stats_client) {
        ALOGE("Unable to get AIDL Stats service");
    } else {
        reportUsbPortOverheat(stats_client, overheat_info);
    }
}

struct data {
    int uevent_fd;
    ::aidl::android::hardware::usb::Usb *usb;
};

static void uevent_event(uint32_t /*epevents*/, struct data *payload) {
    char msg[UEVENT_MSG_LEN + 2];
    char *cp;
    int n;

    n = uevent_kernel_multicast_recv(payload->uevent_fd, msg, UEVENT_MSG_LEN);
    if (n <= 0)
        return;
    if (n >= UEVENT_MSG_LEN) /* overflow -- discard */
        return;

    msg[n] = '\0';
    msg[n + 1] = '\0';
    cp = msg;

    while (*cp) {
        if (std::regex_match(cp, std::regex("(add)(.*)(-partner)"))) {
            ALOGI("partner added");
            pthread_mutex_lock(&payload->usb->mPartnerLock);
            payload->usb->mPartnerUp = true;
            pthread_cond_signal(&payload->usb->mPartnerCV);
            pthread_mutex_unlock(&payload->usb->mPartnerLock);
        } else if (!strncmp(cp, "DEVTYPE=typec_", strlen("DEVTYPE=typec_")) ||
                   !strncmp(cp, "DRIVER=max77759tcpc",
                            strlen("DRIVER=max77759tcpc")) ||
                   !strncmp(cp, "DRIVER=pogo-transport",
                            strlen("DRIVER=pogo-transport")) ||
                   !strncmp(cp, "POWER_SUPPLY_NAME=usb",
                            strlen("POWER_SUPPLY_NAME=usb"))) {
            std::vector<PortStatus> currentPortStatus;
            queryVersionHelper(payload->usb, &currentPortStatus);

            // Role switch is not in progress and port is in disconnected state
            if (!pthread_mutex_trylock(&payload->usb->mRoleSwitchLock)) {
                for (unsigned long i = 0; i < currentPortStatus.size(); i++) {
                    DIR *dp =
                        opendir(string("/sys/class/typec/" +
                                            string(currentPortStatus[i].portName.c_str()) +
                                            "-partner").c_str());
                    if (dp == NULL) {
                        switchToDrp(currentPortStatus[i].portName);
                    } else {
                        closedir(dp);
                    }
                }
                pthread_mutex_unlock(&payload->usb->mRoleSwitchLock);
            }
            break;
        } else if (!strncmp(cp, kOverheatStatsDev, strlen(kOverheatStatsDev))) {
            ALOGV("Overheat Cooling device suez update");
            report_overheat_event(payload->usb);
        }
        /* advance to after the next \0 */
        while (*cp++) {
        }
    }
}

void *work(void *param) {
    int epoll_fd, uevent_fd;
    struct epoll_event ev;
    int nevents = 0;
    struct data payload;

    ALOGE("creating thread");

    uevent_fd = uevent_open_socket(64 * 1024, true);

    if (uevent_fd < 0) {
        ALOGE("uevent_init: uevent_open_socket failed\n");
        return NULL;
    }

    payload.uevent_fd = uevent_fd;
    payload.usb = (::aidl::android::hardware::usb::Usb *)param;

    fcntl(uevent_fd, F_SETFL, O_NONBLOCK);

    ev.events = EPOLLIN;
    ev.data.ptr = (void *)uevent_event;

    epoll_fd = epoll_create(64);
    if (epoll_fd == -1) {
        ALOGE("epoll_create failed; errno=%d", errno);
        goto error;
    }

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, uevent_fd, &ev) == -1) {
        ALOGE("epoll_ctl failed; errno=%d", errno);
        goto error;
    }

    while (!destroyThread) {
        struct epoll_event events[64];

        nevents = epoll_wait(epoll_fd, events, 64, -1);
        if (nevents == -1) {
            if (errno == EINTR)
                continue;
            ALOGE("usb epoll_wait failed; errno=%d", errno);
            break;
        }

        for (int n = 0; n < nevents; ++n) {
            if (events[n].data.ptr)
                (*(void (*)(int, struct data *payload))events[n].data.ptr)(events[n].events,
                                                                           &payload);
        }
    }

    ALOGI("exiting worker thread");
error:
    close(uevent_fd);

    if (epoll_fd >= 0)
        close(epoll_fd);

    return NULL;
}

void sighandler(int sig) {
    if (sig == SIGUSR1) {
        destroyThread = true;
        ALOGI("destroy set");
        return;
    }
    signal(SIGUSR1, sighandler);
}

ScopedAStatus Usb::setCallback(const shared_ptr<IUsbCallback>& in_callback) {
    pthread_mutex_lock(&mLock);
    if ((mCallback == NULL && in_callback == NULL) ||
            (mCallback != NULL && in_callback != NULL)) {
        mCallback = in_callback;
        pthread_mutex_unlock(&mLock);
        return ScopedAStatus::ok();
    }

    mCallback = in_callback;
    ALOGI("registering callback");

    if (mCallback == NULL) {
        if  (!pthread_kill(mPoll, SIGUSR1)) {
            pthread_join(mPoll, NULL);
            ALOGI("pthread destroyed");
        }
        pthread_mutex_unlock(&mLock);
        return ScopedAStatus::ok();
    }

    destroyThread = false;
    signal(SIGUSR1, sighandler);

    /*
     * Create a background thread if the old callback value is NULL
     * and being updated with a new value.
     */
    if (pthread_create(&mPoll, NULL, work, this)) {
        ALOGE("pthread creation failed %d", errno);
        mCallback = NULL;
    }

    pthread_mutex_unlock(&mLock);
    return ScopedAStatus::ok();
}

status_t Usb::handleShellCommand(int in, int out, int err, const char** argv,
                                 uint32_t argc) {
    uid_t uid = AIBinder_getCallingUid();
    if (uid != AID_ROOT && uid != AID_SHELL) {
        return ::android::PERMISSION_DENIED;
    }

    Vector<String8> utf8Args;
    utf8Args.setCapacity(argc);
    for (uint32_t i = 0; i < argc; i++) {
        utf8Args.push(String8(argv[i]));
    }

    if (argc >= 1) {
        if (!utf8Args[0].compare(String8("hub-vendor-cmd"))) {
            if (utf8Args.size() < 3) {
                dprintf(out, "Incorrect number of argument supplied\n");
                return ::android::UNKNOWN_ERROR;
            }
            int value, index;
            if (!::android::base::ParseInt(utf8Args[1].c_str(), &value) ||
                !::android::base::ParseInt(utf8Args[2].c_str(), &index)) {
                dprintf(out, "Fail to parse arguments\n");
                return ::android::UNKNOWN_ERROR;
            }
            mUsbHubVendorCmdValue = value;
            mUsbHubVendorCmdIndex = index;
            ALOGI("USB hub vendor cmd update (wValue 0x%x, wIndex 0x%x)\n",
                  mUsbHubVendorCmdValue, mUsbHubVendorCmdIndex);
            return ::android::NO_ERROR;
        }
    }

    dprintf(out, "usage: adb shell cmd hub-vendor-cmd VALUE INDEX\n"
                 "  VALUE wValue field in hex format, e.g. 0xf321\n"
                 "  INDEX wIndex field in hex format, e.g. 0xf321\n"
                 "  The settings take effect next time the hub is enabled\n");

    return ::android::NO_ERROR;
}

} // namespace usb
} // namespace hardware
} // namespace android
} // aidl
