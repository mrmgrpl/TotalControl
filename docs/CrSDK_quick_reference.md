# CrSDK Quick Reference — v2.01.00

Extracted from SDK headers (`external/CrSDK/include/`). The `.h` files are the source of truth.

---

## API — main functions (CameraRemote_SDK.h)

```cpp
bool        Init(CrInt32u logtype = 0);
bool        Release();
CrError     EnumCameraObjects(ICrEnumCameraObjectInfo** ppEnum, CrInt8u timeInSec = 3);
CrError     Connect(ICrCameraObjectInfo* info, IDeviceCallback* cb, CrDeviceHandle* hDev,
                    CrSdkControlMode mode = CrSdkControlMode_Remote,
                    CrReconnectingSet reconnect = CrReconnecting_ON, ...);
CrError     Disconnect(CrDeviceHandle hDev);
CrError     ReleaseDevice(CrDeviceHandle hDev);
CrError     GetDeviceProperties(CrDeviceHandle hDev, CrDeviceProperty** props, CrInt32* num);
CrError     GetSelectDeviceProperties(CrDeviceHandle hDev, CrInt32u numCodes, CrInt32u* codes,
                                       CrDeviceProperty** props, CrInt32* num);
CrError     ReleaseDeviceProperties(CrDeviceHandle hDev, CrDeviceProperty* props);
CrError     SetDeviceProperty(CrDeviceHandle hDev, CrDeviceProperty* prop);
CrError     SendCommand(CrDeviceHandle hDev, CrInt32u commandId, CrCommandParam param);
CrInt32u    GetSDKVersion();
```

**Cleanup order (important!):**
```cpp
Cr::Disconnect(hDev);
Cr::ReleaseDevice(hDev);
Cr::Release();   // stops internal SDK threads
delete cb;       // only after Release()
```

**Note:** `Connect()` is **asynchronous** — result arrives in the `OnConnected` callback, not in the return value.

---

## Commands (CrCommandId) — SendCommand

```cpp
CrCommandId_Release            = 0   // shutter release (S1+S2)
CrCommandId_MovieRecord        = 1   // start/stop recording
CrCommandId_CancelShooting     = 2   // cancel shot
CrCommandId_MediaFormat        = 4
CrCommandId_S1andRelease       = 7   // AF + release
CrCommandId_PowerOff           = 19
CrCommandId_PowerOn            = 28
```

```cpp
CrCommandParam_Up   = 0x0000  // press / start
CrCommandParam_Down = 0x0001  // release / stop
```

**Trigger a shot:**
```cpp
SendCommand(hDev, CrCommandId_Release, CrCommandParam_Down);  // shutter down
SendCommand(hDev, CrCommandId_Release, CrCommandParam_Up);    // shutter up
```

---

## Device property codes (CrDevicePropertyCode)

### Get/Set (range 0x0100–0x04FF)

| Code   | Name                               | Type / notes                           |
|--------|------------------------------------|----------------------------------------|
| 0x0100 | `CrDeviceProperty_FNumber`         | `CrInt16u` × 100 (e.g. 560 = f/5.6)   |
| 0x0101 | `CrDeviceProperty_ExposureBiasCompensation` | `CrInt16u` × 1000               |
| 0x0103 | `CrDeviceProperty_ShutterSpeed`    | `CrInt32u` — upper16=num, lower16=den  |
| 0x0104 | `CrDeviceProperty_IsoSensitivity`  | `CrInt32u` — see CrISOMode             |
| 0x0105 | `CrDeviceProperty_ExposureProgramMode` | `CrExposureProgram`               |
| 0x0106 | `CrDeviceProperty_FileType`        | `CrFileType`                           |
| 0x0107 | `CrDeviceProperty_StillImageQuality` | `CrImageQuality`                     |
| 0x0108 | `CrDeviceProperty_WhiteBalance`    | `CrWhiteBalanceSetting`                |
| 0x0109 | `CrDeviceProperty_FocusMode`       | `CrFocusMode`                          |
| 0x010A | `CrDeviceProperty_MeteringMode`    | `CrMeteringMode`                       |
| 0x010E | `CrDeviceProperty_DriveMode`       | `CrDriveMode`                          |
| 0x0119 | `CrDeviceProperty_StillImageStoreDestination` | `CrStillImageStoreDestination` |

### Get-only (range 0x0700+)

| Code   | Name                               | Type / notes                           |
|--------|------------------------------------|----------------------------------------|
| 0x0702 | `CrDeviceProperty_BatteryRemain`   | `CrInt16u` 0–100%; 0xFFFF = unavailable |
| 0x0703 | `CrDeviceProperty_BatteryLevel`    | `CrBatteryLevel`                       |
| 0x0705 | `CrDeviceProperty_RecordingState`  | `CrMovie_Recording_State`              |
| 0x0706 | `CrDeviceProperty_LiveViewStatus`  | `CrLiveViewStatus`                     |
| 0x0707 | `CrDeviceProperty_FocusIndication` | `CrFocusIndicator`                     |
| 0x0708 | `CrDeviceProperty_MediaSLOT1_Status` | `CrSlotStatus`                       |
| 0x0709 | `CrDeviceProperty_MediaSLOT1_RemainingNumber` | remaining shot count         |
| 0x070D | `CrDeviceProperty_MediaSLOT2_Status` | `CrSlotStatus`                       |
| 0x070F | `CrDeviceProperty_MediaSLOT2_RemainingNumber` | remaining shot count         |

---

## ShutterSpeed — encoding

```
value = (numerator << 16) | denominator
Bulb    = 0x00000000
Nothing = 0xFFFFFFFF
```

Examples:
```
1/1000  = 0x000103E8
1/500   = 0x000101F4
1/100   = 0x00010064
1/25    = 0x00010019
1s      = 0x00010001
5s      = 0x00050001
30s     = 0x001E0001
```

---

## ISO — encoding (CrISOMode)

```
value = (extension_bits[28:31] << 28) | (mode[24:27] << 24) | iso_value[0:23]
```

| Constant          | Value      | Description             |
|-------------------|------------|-------------------------|
| `CrISO_Normal`    | 0x00       | normal mode (in mode field) |
| `CrISO_MultiFrameNR` | 0x01    | Multi Frame NR          |
| `CrISO_Ext`       | 0x10       | extended                |
| `CrISO_AUTO`      | 0xFFFFFF   | auto ISO (in iso_value field) |

Examples:
```
ISO 100  = 0x00000064
ISO 400  = 0x00000190
ISO 1600 = 0x00000640
ISO AUTO = 0x00FFFFFF
```

---

## FNumber — encoding

```
value = f_number * 100   (CrInt16u)
f/1.4  = 140
f/2.0  = 200
f/2.8  = 280
f/5.6  = 560
f/8.0  = 800
f/10   = 1000
CrFnumber_IrisClose = 0xFFFD
CrFnumber_Unknown   = 0xFFFE
CrFnumber_Nothing   = 0xFFFF
```

---

## DriveMode (CrDriveMode : CrInt32u)

| Value        | Constant                          | Description               |
|--------------|-----------------------------------|---------------------------|
| 0x00000001   | `CrDrive_Single`                  | Single shot               |
| 0x00010001   | `CrDrive_Continuous_Hi`           | Continuous Hi             |
| 0x00010002   | `CrDrive_Continuous_Hi_Plus`      | Continuous Hi+            |
| 0x00010003   | `CrDrive_Continuous_Hi_Live`      | Continuous Hi-Live        |
| 0x00010004   | `CrDrive_Continuous_Lo`           | Continuous Lo             |
| 0x00010005   | `CrDrive_Continuous`              | Continuous                |
| 0x00010008   | `CrDrive_Continuous_Mid`          | Continuous Mid            |
| 0x00020001   | `CrDrive_Timelapse`               | Timelapse                 |
| 0x00030001   | `CrDrive_Timer_2s`                | Self-timer 2s             |
| 0x00030002   | `CrDrive_Timer_5s`                | Self-timer 5s             |
| 0x00030003   | `CrDrive_Timer_10s`               | Self-timer 10s            |

### Continuous bracketing (selected)

| Value        | Constant                                  |
|--------------|-------------------------------------------|
| 0x00040301   | `CrDrive_Continuous_Bracket_03Ev_3pics`   |
| 0x00040302   | `CrDrive_Continuous_Bracket_03Ev_5pics`   |
| 0x00040303   | `CrDrive_Continuous_Bracket_03Ev_9pics`   |
| 0x00040304   | `CrDrive_Continuous_Bracket_05Ev_3pics`   |
| 0x00040305   | `CrDrive_Continuous_Bracket_05Ev_5pics`   |
| 0x00040306   | `CrDrive_Continuous_Bracket_05Ev_9pics`   |
| 0x00040307   | `CrDrive_Continuous_Bracket_07Ev_3pics`   |
| 0x00040308   | `CrDrive_Continuous_Bracket_07Ev_5pics`   |
| 0x00040309   | `CrDrive_Continuous_Bracket_07Ev_9pics`   |
| 0x0004030A   | `CrDrive_Continuous_Bracket_10Ev_3pics`   |
| 0x0004030B   | `CrDrive_Continuous_Bracket_10Ev_5pics`   |
| 0x0004030C   | `CrDrive_Continuous_Bracket_10Ev_9pics`   |
| 0x0004030D   | `CrDrive_Continuous_Bracket_20Ev_3pics`   |
| 0x0004030E   | `CrDrive_Continuous_Bracket_20Ev_5pics`   |
| 0x0004030F   | `CrDrive_Continuous_Bracket_30Ev_3pics`   |
| 0x00040310   | `CrDrive_Continuous_Bracket_30Ev_5pics`   |

### Single bracketing — starts at 0x00050001

Same numbering as Continuous Bracket, but in range `0x0005xxxx`.

---

## ExposureProgram (CrExposureProgram : CrInt32u)

| Value      | Constant                        |
|------------|---------------------------------|
| 0x00000001 | `CrExposure_M_Manual`           |
| 0x00000002 | `CrExposure_P_Auto`             |
| 0x00000003 | `CrExposure_A_AperturePriority` |
| 0x00000004 | `CrExposure_S_ShutterSpeedPriority` |
| 0x00008000 | `CrExposure_Auto`               |
| 0x00008050 | `CrExposure_Movie_P`            |
| 0x00008051 | `CrExposure_Movie_A`            |
| 0x00008052 | `CrExposure_Movie_S`            |
| 0x00008053 | `CrExposure_Movie_M`            |

---

## FocusMode (CrFocusMode : CrInt16u)

| Value | Constant       | Description         |
|-------|----------------|---------------------|
| 0x0001 | `CrFocus_MF`  | Manual Focus        |
| 0x0002 | `CrFocus_AF_S`| AF-S (single)       |
| 0x0003 | `CrFocus_AF_C`| AF-C (continuous)   |
| 0x0004 | `CrFocus_AF_A`| AF-A (auto)         |
| 0x0006 | `CrFocus_DMF` | DMF                 |

---

## WhiteBalance (CrWhiteBalanceSetting : CrInt16u)

| Value  | Constant                        |
|--------|---------------------------------|
| 0x0000 | `CrWhiteBalance_AWB`            |
| 0x0011 | `CrWhiteBalance_Daylight`       |
| 0x0012 | `CrWhiteBalance_Shadow`         |
| 0x0013 | `CrWhiteBalance_Cloudy`         |
| 0x0014 | `CrWhiteBalance_Tungsten`       |
| 0x0020 | `CrWhiteBalance_Fluorescent`    |
| 0x0100 | `CrWhiteBalance_ColorTemp`      |
| 0x0101 | `CrWhiteBalance_Custom_1`       |
| 0x0102 | `CrWhiteBalance_Custom_2`       |
| 0x0103 | `CrWhiteBalance_Custom_3`       |

---

## StillImageStoreDestination (CrStillImageStoreDestination : CrInt16u)

| Value  | Constant                                      | Note                                 |
|--------|-----------------------------------------------|--------------------------------------|
| 0x0001 | `CrStillImageStoreDestination_HostPC`         | ⚠ CrNotify_Captured_Event does NOT fire |
| 0x0002 | `CrStillImageStoreDestination_MemoryCard`     | standard                             |
| 0x0003 | `CrStillImageStoreDestination_HostPCAndMemoryCard` | both                            |

---

## BatteryLevel (CrBatteryLevel : CrInt32u)

| Value      | Constant                           | Description                  |
|------------|------------------------------------|------------------------------|
| 0x00000001 | `CrBatteryLevel_PreEndBattery`     | Critically low               |
| 0x00000002 | `CrBatteryLevel_1_4`               | 1/4                          |
| 0x00000003 | `CrBatteryLevel_2_4`               | 2/4                          |
| 0x00000004 | `CrBatteryLevel_3_4`               | 3/4                          |
| 0x00000005 | `CrBatteryLevel_4_4`               | Full                         |
| 0x00000006 | `CrBatteryLevel_1_3`               | 1/3                          |
| 0x00000007 | `CrBatteryLevel_2_3`               | 2/3                          |
| 0x00000008 | `CrBatteryLevel_3_3`               | 3/3 (full)                   |
| 0x00010000 | `CrBatteryLevel_USBPowerSupply`    | USB power supply (no battery)|
| 0x00010001 | `CrBatteryLevel_PreEnd_PowerSupply`| Critically low + USB         |
| 0x00010002 | `CrBatteryLevel_1_4_PowerSupply`   | 1/4 + USB                    |
| 0x00010003 | `CrBatteryLevel_2_4_PowerSupply`   | 2/4 + USB                    |
| 0x00010004 | `CrBatteryLevel_3_4_PowerSupply`   | 3/4 + USB                    |
| 0x00010005 | `CrBatteryLevel_4_4_PowerSupply`   | Full + USB                   |
| 0xFFFFFFFE | `CrBatteryLevel_BatteryNotInstalled` | No battery                 |

---

## SlotStatus (CrSlotStatus : CrInt16u)

⚠ **Pitfall:** `OK = 0x0000` (card present), `NoCard = 0x0001` — opposite of what you might expect!

| Value  | Constant                                  | Description                      |
|--------|-------------------------------------------|----------------------------------|
| 0x0000 | `CrSlotStatus_OK`                         | Card present and OK              |
| 0x0001 | `CrSlotStatus_NoCard`                     | No card                          |
| 0x0002 | `CrSlotStatus_CardError`                  | Card error                       |
| 0x0003 | `CrSlotStatus_RecognizingOrLockedError`   | Recognizing / locked             |
| 0x0004 | `CrSlotStatus_DBError`                    | Database error                   |
| 0x0005 | `CrSlotStatus_CardRecognizing`            | Recognition in progress          |
| 0x0006 | `CrSlotStatus_CardLockedAndDBError`       | Locked + DB error                |
| 0x0007 | `CrSlotStatus_DBError_CantRepairAndNeedFormat` | DB unrecoverable, needs format |
| 0x0008 | `CrSlotStatus_CardError_ReadOnlyMedia`    | Read-only media                  |

---

## FocusIndicator (CrFocusIndicator : CrInt32u)

| Value      | Constant                              |
|------------|---------------------------------------|
| 0x00000001 | `CrFocusIndicator_Unlocked`           |
| 0x00000102 | `CrFocusIndicator_Focused_AF_S`       |
| 0x00000202 | `CrFocusIndicator_NotFocused_AF_S`    |
| 0x00000103 | `CrFocusIndicator_Focused_AF_C`       |
| 0x00000203 | `CrFocusIndicator_NotFocused_AF_C`    |
| 0x00000303 | `CrFocusIndicator_TrackingSubject_AF_C` |

---

## PriorityKeySettings (CrPriorityKeySettings : CrInt16u)

| Value  | Constant                      | Description                    |
|--------|-------------------------------|--------------------------------|
| 0x0001 | `CrPriorityKey_CameraPosition`| Control from camera            |
| 0x0002 | `CrPriorityKey_PCRemote`      | Control from PC (SDK)          |

---

## Error codes — categories (CrError)

| Code   | Category                     |
|--------|------------------------------|
| 0x0000 | `CrError_None` — success     |
| 0x8000 | `CrError_Generic`            |
| 0x8001 | `CrError_Generic_Unknown`    |
| 0x8002 | `CrError_Generic_Notimpl`    |
| 0x8003 | `CrError_Generic_NotSupported` ← GetTimeZoneSetting over USB returns this |
| 0x8005 | `CrError_Generic_InvalidHandle` |
| 0x8006 | `CrError_Generic_InvalidParameter` |
| 0x8100 | `CrError_File`               |
| 0x8200 | `CrError_Connect`            |
| 0x8207 | `CrError_Connect_Disconnected` |
| 0x8208 | `CrError_Connect_TimeOut`    |
| 0x820A | `CrError_Connect_FailRejected` |
| 0x820B | `CrError_Connect_FailBusy`   |
| 0x8300 | `CrError_Memory`             |
| 0x8400 | `CrError_Api`                |
| 0x8500 | `CrError_Init`               |
| 0x8700 | `CrError_Adaptor`            |
| 0x8800 | `CrError_Device`             |

```cpp
#define CR_SUCCEEDED(e)  (SCRSDK::CrError_None == (e))
#define CR_FAILED(e)     (SCRSDK::CrError_None != (e))
```

### Notifications (OnWarning callback)

| Code       | Constant                      | Description                          |
|------------|-------------------------------|--------------------------------------|
| 0x00020000 | `CrWarning_Unknown`           |                                      |
| 0x00020001 | `CrWarning_Connect_Reconnected` |                                    |
| 0x00020010 | `CrNotify_Captured_Event`     | Shot captured (to card)              |
| 0x00020021 | `CrNotify_All_Download_Complete` | All files downloaded              |

---

## Flow: Connect

```
1. Init()
2. EnumCameraObjects(&pEnum, timeout=3s)
3. pEnum->GetCameraObjectInfo(i)   // ICrCameraObjectInfo*
4. Connect(const_cast<ICrCameraObjectInfo*>(info), callback, &hDev)
   → asynchronous: wait for OnConnected()
5. In OnConnected(): GetDeviceProperties() → WarmCache
6. SetDeviceProperty(PriorityKey = PCRemote)
```

---

## Flow: Shoot (single)

```
1. SetDeviceProperty(DriveMode = CrDrive_Single)        // + polling verify
2. SetDeviceProperty(ShutterSpeed = value)              // + polling verify
3. SetDeviceProperty(IsoSensitivity = value)            // cached
4. SetDeviceProperty(FNumber = value)                   // cached
5. SetDeviceProperty(ExposureProgramMode = M_Manual)    // cached
6. SendCommand(CrCommandId_Release, CrCommandParam_Down)
7. SendCommand(CrCommandId_Release, CrCommandParam_Up)
   → OnWarning(CrNotify_Captured_Event) fires when shot lands on card
   → does NOT fire when StoreDestination = HostPC
```

---

## Known pitfalls

| Problem | Description |
|---------|-------------|
| `DeviceConnectionVersioin` | Typo in SDK (double `i`) — intentional |
| `Connect()` async | Do not check `err==0`; wait for `OnConnected` |
| `ICrCameraObjectInfo*` | Requires `const_cast` when passed to `Connect()` |
| `GetId()` empty for USB | Use `GuidOrIdHex()` — fallback to `GetId()` as UTF-16LE |
| `GetTimeZoneSetting()` → USB | Returns `0x8003` (NotSupported) — camera time unavailable over USB |
| `CrSlotStatus_OK = 0x0000` | Card present = 0, no card = 1 (opposite of what you might expect) |
| `StoreDestination = HostPC` | `CrNotify_Captured_Event` does NOT fire |
| `CrAdapter/` | Transport DLLs must be in `CrAdapter/` subdirectory next to exe |
| SS/DriveMode delay | May be deferred while camera is writing RAW — poll via GetPropRaw |
| `EnumCameraObjects` | Second arg = timeout in seconds; omitting it may hang the thread |
