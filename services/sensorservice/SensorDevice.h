/*
 * Copyright (C) 2010 The Android Open Source Project
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

#ifndef ANDROID_SENSOR_DEVICE_H
#define ANDROID_SENSOR_DEVICE_H

#include "SensorServiceUtils.h"

#include <sensor/Sensor.h>
#include <stdint.h>
#include <sys/types.h>
#include <utils/KeyedVector.h>
#include <utils/Singleton.h>
#include <utils/String8.h>

#include <string>
#include <map>

#include "android/hardware/sensors/1.0/ISensors.h"

// ---------------------------------------------------------------------------

namespace android {

// ---------------------------------------------------------------------------
using SensorServiceUtil::Dumpable;
using hardware::Return;

class SensorDevice : public Singleton<SensorDevice>, public Dumpable {
public:
    ssize_t getSensorList(sensor_t const** list);

    void handleDynamicSensorConnection(int handle, bool connected);
    status_t initCheck() const;
    int getHalDeviceVersion() const;

    ssize_t poll(sensors_event_t* buffer, size_t count);

    status_t activate(void* ident, int handle, int enabled);
    status_t batch(void* ident, int handle, int flags, int64_t samplingPeriodNs,
                   int64_t maxBatchReportLatencyNs);
    // Call batch with timeout zero instead of calling setDelay() for newer devices.
    status_t setDelay(void* ident, int handle, int64_t ns);
    status_t flush(void* ident, int handle);
    status_t setMode(uint32_t mode);

    bool isDirectReportSupported() const;
    int32_t registerDirectChannel(const sensors_direct_mem_t *memory);
    void unregisterDirectChannel(int32_t channelHandle);
    int32_t configureDirectChannel(int32_t sensorHandle,
            int32_t channelHandle, const struct sensors_direct_cfg_t *config);

    void disableAllSensors();
    void enableAllSensors();
    void autoDisable(void *ident, int handle);

    status_t injectSensorData(const sensors_event_t *event);
    void notifyConnectionDestroyed(void *ident);

    // Dumpable
    virtual std::string dump() const;
private:
    friend class Singleton<SensorDevice>;

    sp<android::hardware::sensors::V1_0::ISensors> mSensors;
    Vector<sensor_t> mSensorList;
    std::map<int32_t, sensor_t*> mConnectedDynamicSensors;

    static const nsecs_t MINIMUM_EVENTS_PERIOD =   1000000; // 1000 Hz
    mutable Mutex mLock; // protect mActivationCount[].batchParams
    // fixed-size array after construction

    // Struct to store all the parameters(samplingPeriod, maxBatchReportLatency and flags) from
    // batch call. For continous mode clients, maxBatchReportLatency is set to zero.
    struct BatchParams {
      // TODO: Get rid of flags parameter everywhere.
      int flags;
      nsecs_t batchDelay, batchTimeout;
      BatchParams() : flags(0), batchDelay(0), batchTimeout(0) {}
      BatchParams(int flag, nsecs_t delay, nsecs_t timeout): flags(flag), batchDelay(delay),
          batchTimeout(timeout) { }
      bool operator != (const BatchParams& other) {
          return other.batchDelay != batchDelay || other.batchTimeout != batchTimeout ||
                 other.flags != flags;
      }
    };

    // Store batch parameters in the KeyedVector and the optimal batch_rate and timeout in
    // bestBatchParams. For every batch() call corresponding params are stored in batchParams
    // vector. A continuous mode request is batch(... timeout=0 ..) followed by activate(). A batch
    // mode request is batch(... timeout > 0 ...) followed by activate().
    // Info is a per-sensor data structure which contains the batch parameters for each client that
    // has registered for this sensor.
    struct Info {
        BatchParams bestBatchParams;
        // Key is the unique identifier(ident) for each client, value is the batch parameters
        // requested by the client.
        KeyedVector<void*, BatchParams> batchParams;

        Info() : bestBatchParams(0, -1, -1) {}
        // Sets batch parameters for this ident. Returns error if this ident is not already present
        // in the KeyedVector above.
        status_t setBatchParamsForIdent(void* ident, int flags, int64_t samplingPeriodNs,
                                        int64_t maxBatchReportLatencyNs);
        // Finds the optimal parameters for batching and stores them in bestBatchParams variable.
        void selectBatchParams();
        // Removes batchParams for an ident and re-computes bestBatchParams. Returns the index of
        // the removed ident. If index >=0, ident is present and successfully removed.
        ssize_t removeBatchParamsForIdent(void* ident);

        int numActiveClients();
    };
    DefaultKeyedVector<int, Info> mActivationCount;

    // Use this vector to determine which client is activated or deactivated.
    SortedVector<void *> mDisabledClients;
    SensorDevice();

    static void handleHidlDeath(const std::string &detail);
    template<typename T>
    static Return<T> checkReturn(Return<T> &&ret) {
        if (!ret.isOk()) {
            handleHidlDeath(ret.description());
        }
        return std::move(ret);
    }

    bool isClientDisabled(void* ident);
    bool isClientDisabledLocked(void* ident);

    using Event = hardware::sensors::V1_0::Event;
    using SensorInfo = hardware::sensors::V1_0::SensorInfo;

    void convertToSensorEvent(const Event &src, sensors_event_t *dst);

    void convertToSensorEvents(
            const hardware::hidl_vec<Event> &src,
            const hardware::hidl_vec<SensorInfo> &dynamicSensorsAdded,
            sensors_event_t *dst);

    bool mIsDirectReportSupported;
};

// ---------------------------------------------------------------------------
}; // namespace android

#endif // ANDROID_SENSOR_DEVICE_H
