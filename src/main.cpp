#include "CameraRemote_SDK.h"
#include "IDeviceCallback.h"
#include "CrError.h"
#include "CrControlCode.h"
#include "CrDeviceProperty.h"
#include "ICrCameraObjectInfo.h"

#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>

namespace Cr = SCRSDK;

// ─── Log ──────────────────────────────────────────────────────────────────────
static std::ofstream g_log;
static std::wstring  g_logPath;

static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    wchar_t* s = wcsrchr(buf, L'\\');
    if (s) *(s + 1) = L'\0';
    return buf;
}
static std::string WtoU8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}
static std::wstring Timestamp() {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t b[16];
    swprintf_s(b, L"%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return b;
}
static void Log(const wchar_t* msg) {
    std::wstring line = Timestamp() + L"  " + msg;
    g_log << WtoU8(line) << "\n"; g_log.flush();
    wprintf(L"%s\n", line.c_str());
    ::OutputDebugStringW((line + L"\n").c_str());
}
static void Logf(const wchar_t* fmt, ...) {
    wchar_t buf[1024]; va_list a;
    va_start(a, fmt); _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, a); va_end(a);
    Log(buf);
}
static void LogSep(const wchar_t* title) {
    Logf(L"──── %s ", title);
}

// ─── Callback ─────────────────────────────────────────────────────────────────
class CameraCallback : public Cr::IDeviceCallback {
public:
    std::atomic<bool>       connected{ false };
    std::atomic<bool>       shotFired{ false };
    std::mutex              mtx;
    std::condition_variable cv;

    void OnConnected(Cr::DeviceConnectionVersioin ver) override {
        Logf(L"  [CB] OnConnected  ver=%d", (int)ver);
        connected = true; cv.notify_all();
    }
    void OnDisconnected(CrInt32u r) override {
        Logf(L"  [CB] OnDisconnected  code=0x%08X", r);
        connected = false;
    }
    void OnPropertyChanged() override {
        Log(L"  [CB] PropertyChanged");
    }
    void OnPropertyChangedCodes(CrInt32u n, CrInt32u* codes) override {
        std::wstring s = L"  [CB] PropertyChangedCodes  n=";
        wchar_t tmp[16]; swprintf_s(tmp, L"%u  codes:", n); s += tmp;
        for (CrInt32u i = 0; i < n && i < 8; ++i) {
            swprintf_s(tmp, L" 0x%04X", codes[i]); s += tmp;
        }
        Log(s.c_str());
    }
    void OnWarning(CrInt32u c) override {
        const wchar_t* name = nullptr;
        switch (c) {
        case Cr::CrNotify_Captured_Event:                    name = L"CrNotify_Captured_Event"; break;
        case Cr::CrNotify_All_Download_Complete:             name = L"CrNotify_All_Download_Complete"; break;
        case Cr::CrWarning_Connect_Reconnected:              name = L"Connect_Reconnected"; break;
        case Cr::CrWarning_Connect_Reconnecting:             name = L"Connect_Reconnecting"; break;
        case Cr::CrWarning_File_StorageFull:                 name = L"File_StorageFull"; break;
        case Cr::CrWarning_Frame_NotUpdated:                 name = L"Frame_NotUpdated"; break;
        case Cr::CrNotify_ContentsTransfer_Start:            name = L"ContentsTransfer_Start"; break;
        case Cr::CrNotify_ContentsTransfer_Complete:         name = L"ContentsTransfer_Complete"; break;
        case Cr::CrWarning_LensInformationChanged:           name = L"LensInformationChanged"; break;
        case Cr::CrWarning_MediaProfileChanged_Slot1:        name = L"MediaProfileChanged_Slot1"; break;
        case Cr::CrWarning_MediaProfileChanged_Slot2:        name = L"MediaProfileChanged_Slot2"; break;
        case Cr::CrWarning_Reserved1:                        name = L"Reserved1 (undocumented)"; break;
        default:                                             break;
        }

        if (c == Cr::CrNotify_Captured_Event) {
            Log(L"  [CB] *** CrNotify_Captured_Event — ekspozycja / zapis na kartę ***");
            shotFired = true; cv.notify_all();
        } else if (c == Cr::CrNotify_All_Download_Complete) {
            Log(L"  [CB] *** CrNotify_All_Download_Complete — transfer do PC gotowy ***");
            shotFired = true; cv.notify_all();
        } else if (name) {
            Logf(L"  [CB] Warning  0x%08X  [%s]", c, name);
        } else {
            Logf(L"  [CB] Warning  0x%08X  [?unknown]", c);
        }
    }
    void OnWarningExt(CrInt32u w, CrInt32 p1, CrInt32 p2, CrInt32 p3) override {
        if (w == Cr::CrWarningExt_AFStatus) {
            const wchar_t* s = L"Unknown";
            switch (p1) {
            case Cr::CrWarningExt_AFStatusParam_Unlocked:             s = L"Unlocked";       break;
            case Cr::CrWarningExt_AFStatusParam_Focused_AF_S:         s = L"Focused(AF-S)";  break;
            case Cr::CrWarningExt_AFStatusParam_NotFocused_AF_S:      s = L"NotFocused(AF-S)"; break;
            case Cr::CrWarningExt_AFStatusParam_TrackingSubject_AF_C: s = L"Tracking(AF-C)"; break;
            case Cr::CrWarningExt_AFStatusParam_Focused_AF_C:         s = L"Focused(AF-C)";  break;
            case Cr::CrWarningExt_AFStatusParam_NotFocused_AF_C:      s = L"NotFocused(AF-C)"; break;
            }
            Logf(L"  [CB] AFStatus: %s", s);
        } else {
            Logf(L"  [CB] WarningExt  w=0x%08X  p1=%d p2=%d p3=%d", w, p1, p2, p3);
        }
    }
    void OnError(CrInt32u c) override { Logf(L"  [CB] Error  0x%08X", c); }
    void OnCompleteOperation(CrInt32u code, Cr::CrOperationResultData*) override {
        Logf(L"  [CB] CompleteOperation  code=0x%04X", code);
        shotFired = true; cv.notify_all();
    }
    void OnNotifyPostViewImage(CrChar* fn, CrInt32u sz) override {
        Logf(L"  [CB] PostViewImage  %s  %u B", fn ? fn : L"(null)", sz);
        shotFired = true; cv.notify_all();
    }
    void OnCompleteDownload(CrChar* fn, CrInt32u) override {
        Logf(L"  [CB] Download  %s", fn ? fn : L"?");
        shotFired = true; cv.notify_all();
    }
    void OnLvPropertyChanged()                                       override {}
    void OnLvPropertyChangedCodes(CrInt32u, CrInt32u*)              override {}
    void OnNotifyContentsTransfer(CrInt32u, Cr::CrContentHandle, CrChar*) override {}
    void OnNotifyFTPTransferResult(CrInt32u, CrInt32u, CrInt32u)    override {}
    void OnNotifyRemoteTransferResult(CrInt32u, CrInt32u, CrChar*)  override {}
    void OnNotifyRemoteTransferResult(CrInt32u, CrInt32u, CrInt8u*, CrInt64u) override {}
    void OnNotifyRemoteTransferContentsListChanged(CrInt32u, CrInt32u, CrInt32u) override {}
    void OnNotifyRemoteFirmwareUpdateResult(CrInt32u, const void*)   override {}
    void OnReceivePlaybackTimeCode(CrInt32u)                         override {}
    void OnReceivePlaybackData(CrInt8u, CrInt32, CrInt8u*, CrInt64, CrInt64, CrInt32, CrInt32) override {}
    void OnNotifyMonitorUpdated(CrInt32u, CrInt32u)                  override {}

    bool wait_connected(int ms) {
        std::unique_lock<std::mutex> lk(mtx);
        return cv.wait_for(lk, std::chrono::milliseconds(ms), [this] { return connected.load(); });
    }
    bool wait_shot(int ms) {
        std::unique_lock<std::mutex> lk(mtx);
        return cv.wait_for(lk, std::chrono::milliseconds(ms), [this] { return shotFired.load(); });
    }
};

// ─── Nazwy właściwości ────────────────────────────────────────────────────────
static const wchar_t* PropName(CrInt32u c) {
    switch (c) {
    case Cr::CrDeviceProperty_S1:                        return L"S1";
    case Cr::CrDeviceProperty_AEL:                       return L"AEL";
    case Cr::CrDeviceProperty_FEL:                       return L"FEL";
    case Cr::CrDeviceProperty_AWBL:                      return L"AWBL";
    case Cr::CrDeviceProperty_FNumber:                   return L"FNumber";
    case Cr::CrDeviceProperty_ExposureBiasCompensation:  return L"ExposureBiasComp";
    case Cr::CrDeviceProperty_FlashCompensation:         return L"FlashComp";
    case Cr::CrDeviceProperty_ShutterSpeed:              return L"ShutterSpeed";
    case Cr::CrDeviceProperty_IsoSensitivity:            return L"ISO";
    case Cr::CrDeviceProperty_ExposureProgramMode:       return L"ExposureMode";
    case Cr::CrDeviceProperty_FileType:                  return L"FileType";
    case Cr::CrDeviceProperty_StillImageQuality:         return L"ImageQuality";
    case Cr::CrDeviceProperty_WhiteBalance:              return L"WhiteBalance";
    case Cr::CrDeviceProperty_FocusMode:                 return L"FocusMode";
    case Cr::CrDeviceProperty_MeteringMode:              return L"MeteringMode";
    case Cr::CrDeviceProperty_FlashMode:                 return L"FlashMode";
    case Cr::CrDeviceProperty_WirelessFlash:             return L"WirelessFlash";
    case Cr::CrDeviceProperty_RedEyeReduction:           return L"RedEyeReduction";
    case Cr::CrDeviceProperty_DriveMode:                 return L"DriveMode";
    case Cr::CrDeviceProperty_DRO:                       return L"DRO";
    case Cr::CrDeviceProperty_ImageSize:                 return L"ImageSize";
    case Cr::CrDeviceProperty_AspectRatio:               return L"AspectRatio";
    case Cr::CrDeviceProperty_PictureEffect:             return L"PictureEffect";
    case Cr::CrDeviceProperty_FocusArea:                 return L"FocusArea";
    case Cr::CrDeviceProperty_Colortemp:                 return L"ColorTemp";
    case Cr::CrDeviceProperty_ColorTuningAB:             return L"ColorTuningAB";
    case Cr::CrDeviceProperty_ColorTuningGM:             return L"ColorTuningGM";
    case Cr::CrDeviceProperty_LiveViewDisplayEffect:     return L"LVDisplayEffect";
    case Cr::CrDeviceProperty_StillImageStoreDestination:return L"StoreDestination";
    case Cr::CrDeviceProperty_PriorityKeySettings:       return L"PriorityKeySettings";
    case Cr::CrDeviceProperty_AFTrackingSensitivity:     return L"AFTrackSensitivity";
    case Cr::CrDeviceProperty_Focus_Magnifier_Setting:   return L"FocusMagnifierSetting";
    case Cr::CrDeviceProperty_DateTime_Settings:         return L"DateTimeSettings";
    case Cr::CrDeviceProperty_NearFar:                   return L"NearFar";
    case Cr::CrDeviceProperty_AF_Area_Position:          return L"AFAreaPosition";
    case Cr::CrDeviceProperty_Zoom_Scale:                return L"ZoomScale";
    case Cr::CrDeviceProperty_Zoom_Setting:              return L"ZoomSetting";
    case Cr::CrDeviceProperty_Zoom_Operation:            return L"ZoomOperation";
    case Cr::CrDeviceProperty_Movie_File_Format:         return L"MovieFileFormat";
    case Cr::CrDeviceProperty_Movie_Recording_Setting:   return L"MovieRecordingSetting";
    case Cr::CrDeviceProperty_IrisModeSetting:           return L"IrisModeSetting";
    case Cr::CrDeviceProperty_ShutterModeSetting:        return L"ShutterModeSetting";
    case Cr::CrDeviceProperty_GainControlSetting:        return L"GainControlSetting";
    case Cr::CrDeviceProperty_ExposureIndex:             return L"ExposureIndex";
    case Cr::CrDeviceProperty_PlaybackMedia:             return L"PlaybackMedia";
    case Cr::CrDeviceProperty_BodyKeyLock:               return L"BodyKeyLock";
    case Cr::CrDeviceProperty_ExposureCtrlType:          return L"ExposureCtrlType";
    case Cr::CrDeviceProperty_FocalDistanceInMeter:      return L"FocalDist_m";
    case Cr::CrDeviceProperty_FocalDistanceInFeet:       return L"FocalDist_ft";
    case Cr::CrDeviceProperty_FocusIndication:           return L"FocusIndication";
    case Cr::CrDeviceProperty_MediaSLOT1_Status:         return L"SlotA_Status";
    case Cr::CrDeviceProperty_MediaSLOT2_Status:         return L"SlotB_Status";
    case Cr::CrDeviceProperty_MediaSLOT1_RemainingNumber:return L"SlotA_Remaining";
    case Cr::CrDeviceProperty_MediaSLOT2_RemainingNumber:return L"SlotB_Remaining";
    case Cr::CrDeviceProperty_BatteryRemain:             return L"BatteryRemain";
    case Cr::CrDeviceProperty_BatteryLevel:              return L"BatteryLevel";
    case Cr::CrDeviceProperty_LiveViewStatus:            return L"LiveViewStatus";
    case Cr::CrDeviceProperty_FocusModeStatus:           return L"FocusModeStatus";
    case Cr::CrDeviceProperty_ShutterSpeedCurrentValue:  return L"ShutterSpeedCurrent";
    case Cr::CrDeviceProperty_FocusModeSetting:          return L"FocusModeSetting";
    case Cr::CrDeviceProperty_SilentMode:                return L"SilentMode";
    case Cr::CrDeviceProperty_FocusMap:                  return L"FocusMap";
    case Cr::CrDeviceProperty_AFWithShutter:             return L"AFWithShutter";
    case Cr::CrDeviceProperty_ReleaseWithoutLens:        return L"ReleaseWithoutLens";
    default:                                             return nullptr;
    }
}

// ─── Nazwy Control Codes ──────────────────────────────────────────────────────
static const wchar_t* CtrlName(CrInt32u c) {
    switch (c) {
    case Cr::CrControlCode_S1AndS2Button:            return L"S1AndS2Button";
    case Cr::CrControlCode_Release:                  return L"Release";
    case Cr::CrControlCode_MovieRecButton:           return L"MovieRecButton";
    case Cr::CrControlCode_MovieRecButtonToggle:     return L"MovieRecButtonToggle";
    case Cr::CrControlCode_MovieRecButtonToggle2:    return L"MovieRecButtonToggle2";
    case Cr::CrControlCode_SelectedMediaFormat:      return L"SelectedMediaFormat";
    case Cr::CrControlCode_CancelMediaFormat:        return L"CancelMediaFormat";
    case Cr::CrControlCode_RECSettingsReset:         return L"RECSettingsReset";
    case Cr::CrControlCode_APS_C_or_Full_Switching:  return L"APS-C/FullSwitch";
    case Cr::CrControlCode_CancelRemoteTouchOperation: return L"CancelRemoteTouch";
    case Cr::CrControlCode_PixelMapping:             return L"PixelMapping";
    case Cr::CrControlCode_TimeCodePresetReset:      return L"TimeCodePresetReset";
    case Cr::CrControlCode_UserBitPresetReset:       return L"UserBitPresetReset";
    case Cr::CrControlCode_SensorCleaning:           return L"SensorCleaning";
    case Cr::CrControlCode_ResetPictureProfile:      return L"ResetPictureProfile";
    case Cr::CrControlCode_ResetCreativeLook:        return L"ResetCreativeLook";
    case Cr::CrControlCode_StreamButton:             return L"StreamButton";
    case Cr::CrControlCode_FlickerScan:              return L"FlickerScan";
    case Cr::CrControlCode_ContinuousShootingSpotBoostButton: return L"ContinuousSpotBoost";
    case Cr::CrControlCode_TrackingOnAndAFOnButton:  return L"TrackingOnAndAFOn";
    case Cr::CrControlCode_ForcedFileNumberReset:    return L"ForcedFileNumberReset";
    case Cr::CrControlCode_CameraStandBy:            return L"CameraStandBy";
    case Cr::CrControlCode_PowerOff:                 return L"PowerOff";
    case Cr::CrControlCode_PowerOn:                  return L"PowerOn";
    case Cr::CrControlCode_RemoteKeyUp:              return L"RemoteKey_Up";
    case Cr::CrControlCode_RemoteKeyDown:            return L"RemoteKey_Down";
    case Cr::CrControlCode_RemoteKeyLeft:            return L"RemoteKey_Left";
    case Cr::CrControlCode_RemoteKeyRight:           return L"RemoteKey_Right";
    case Cr::CrControlCode_RemoteKeyCancelBackButton:return L"RemoteKey_Back";
    case Cr::CrControlCode_RemoteKeyDisplayButton:   return L"RemoteKey_Display";
    case Cr::CrControlCode_RemoteKeySet:             return L"RemoteKey_Set";
    case Cr::CrControlCode_RemoteKeyRightUp:         return L"RemoteKey_RightUp";
    case Cr::CrControlCode_RemoteKeyRightDown:       return L"RemoteKey_RightDown";
    case Cr::CrControlCode_RemoteKeyLeftUp:          return L"RemoteKey_LeftUp";
    case Cr::CrControlCode_RemoteKeyLeftDown:        return L"RemoteKey_LeftDown";
    case Cr::CrControlCode_RemoteKeyMenuButton:      return L"RemoteKey_Menu";
    case Cr::CrControlCode_ResetMultiMatrix:         return L"ResetMultiMatrix";
    case Cr::CrControlCode_CancelFocusPosition:      return L"CancelFocusPosition";
    case Cr::CrControlCode_CancelZoomPosition:       return L"CancelZoomPosition";
    case Cr::CrControlCode_CancelContentsTransfer:   return L"CancelContentsTransfer";
    case Cr::CrControlCode_S1Button:                 return L"S1Button (half-press AF)";
    case Cr::CrControlCode_S2Button:                 return L"S2Button (full-press SHUTTER)";
    case Cr::CrControlCode_AELButton:                return L"AEL Button";
    case Cr::CrControlCode_FELButton:                return L"FEL Button";
    case Cr::CrControlCode_AWBLButton:               return L"AWBL Button";
    case Cr::CrControlCode_NearFar:                  return L"NearFar (focus)";
    case Cr::CrControlCode_AFAreaPosition:           return L"AFAreaPosition";
    case Cr::CrControlCode_ZoomOperation:            return L"ZoomOperation";
    case Cr::CrControlCode_CustomWBCaptureStandby:   return L"CustomWB_Standby";
    case Cr::CrControlCode_CustomWBCaptureStandbyCancel: return L"CustomWB_StandbyCancel";
    case Cr::CrControlCode_CustomWBCapture:          return L"CustomWB_Capture";
    case Cr::CrControlCode_FocusOperation:           return L"FocusOperation";
    case Cr::CrControlCode_RemoteTouchOperation:     return L"RemoteTouchOperation";
    case Cr::CrControlCode_SaveZoomAndFocusPosition: return L"SaveZoomFocusPos";
    case Cr::CrControlCode_LoadZoomAndFocusPosition: return L"LoadZoomFocusPos";
    case Cr::CrControlCode_ColorTemperatureStep:     return L"ColorTempStep";
    case Cr::CrControlCode_WhiteBalanceTintStep:     return L"WBTintStep";
    case Cr::CrControlCode_CreateNewFolder:          return L"CreateNewFolder";
    case Cr::CrControlCode_USBConnectionModeRequest: return L"USBConnectionModeRequest";
    default:                                         return nullptr;
    }
}

// ─── DataType jako string ──────────────────────────────────────────────────────
static std::wstring DataTypeName(CrInt32u t) {
    switch (t) {
    case Cr::CrDataType_UInt8:       return L"UInt8";
    case Cr::CrDataType_UInt16:      return L"UInt16";
    case Cr::CrDataType_UInt32:      return L"UInt32";
    case Cr::CrDataType_UInt64:      return L"UInt64";
    case Cr::CrDataType_Int8:        return L"Int8";
    case Cr::CrDataType_Int16:       return L"Int16";
    case Cr::CrDataType_Int32:       return L"Int32";
    case Cr::CrDataType_Int64:       return L"Int64";
    case Cr::CrDataType_UInt16Array: return L"UInt16[]";
    case Cr::CrDataType_UInt32Array: return L"UInt32[]";
    case Cr::CrDataType_UInt16Range: return L"UInt16Range";
    case Cr::CrDataType_UInt32Range: return L"UInt32Range";
    case Cr::CrDataType_Button:      return L"Button(Up/Down)";
    default: {
        wchar_t b[16]; swprintf_s(b, L"0x%05X", t); return b;
    }
    }
}

// ─── Dump: informacje o kamerze ────────────────────────────────────────────────
static void DumpCameraInfo(const Cr::ICrCameraObjectInfo* info) {
    LogSep(L"INFORMACJE O KAMERZE");
    Logf(L"  Model          : %s", info->GetModel() ? info->GetModel() : L"?");
    Logf(L"  Name           : %s", info->GetName() ? info->GetName() : L"?");
    Logf(L"  Połączenie     : %s", info->GetConnectionTypeName() ? info->GetConnectionTypeName() : L"?");
    Logf(L"  Adapter        : %s", info->GetAdaptorName() ? info->GetAdaptorName() : L"?");
    Logf(L"  GUID           : %s", info->GetGuid() ? info->GetGuid() : L"?");
    Logf(L"  USB PID        : 0x%04X", (unsigned)info->GetUsbPid());
    Logf(L"  PairingNeeded  : %s", info->GetPairingNecessity() ? info->GetPairingNecessity() : L"?");
    // ID bytes jako hex
    const CrInt8u* id = info->GetId();
    CrInt32u idSz = info->GetIdSize();
    if (id && idSz > 0) {
        std::wstring hex = L"  Device ID      : ";
        wchar_t b[8];
        for (CrInt32u i = 0; i < idSz && i < 32; ++i) {
            swprintf_s(b, L"%02X", (unsigned)id[i]); hex += b;
        }
        Log(hex.c_str());
    }
}

// ─── Dump: wszystkie właściwości ───────────────────────────────────────────────
static void DumpAllProperties(Cr::CrDeviceHandle hDev) {
    LogSep(L"WŁAŚCIWOŚCI URZĄDZENIA (GetDeviceProperties)");
    Cr::CrDeviceProperty* props = nullptr;
    CrInt32 num = 0;
    Log(L"  >> GetDeviceProperties...");
    auto t0 = std::chrono::steady_clock::now();
    Cr::CrError err = Cr::GetDeviceProperties(hDev, &props, &num);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    Logf(L"  << err=0x%04X  count=%d  (%.0f ms)", err, num, ms);
    if (err != 0 || !props || num == 0) return;

    Logf(L"  Łącznie właściwości: %d", num);
    Log(L"");
    for (CrInt32 i = 0; i < num; ++i) {
        CrInt32u code = props[i].GetCode();
        CrInt64u cur  = props[i].GetCurrentValue();
        CrInt32u type = static_cast<CrInt32u>(props[i].GetValueType());
        const wchar_t* name = PropName(code);

        wchar_t nameBuf[64];
        if (name)
            swprintf_s(nameBuf, L"%-30s", name);
        else
            swprintf_s(nameBuf, L"0x%04X                        ", code);

        // Dostępne wartości
        CrInt32u setBytes = props[i].GetSetValueSize();
        std::wstring setStr;
        if (setBytes > 0) {
            if ((type & 0xFF) == Cr::CrDataType_UInt16) {
                CrInt32u cnt = setBytes / 2;
                wchar_t b[24];
                swprintf_s(b, L"[%u vals]", cnt); setStr = b;
            } else if ((type & 0xFF) == Cr::CrDataType_UInt32) {
                CrInt32u cnt = setBytes / 4;
                wchar_t b[24];
                swprintf_s(b, L"[%u vals]", cnt); setStr = b;
            }
        }

        Logf(L"  0x%04X  %s  type=%-18s  cur=0x%08llX  %s",
             code, nameBuf, DataTypeName(type).c_str(),
             (unsigned long long)cur, setStr.c_str());
    }
    Cr::ReleaseDeviceProperties(hDev, props);
    Log(L"");
}

// ─── Dump: obsługiwane Control Codes ──────────────────────────────────────────
static void DumpControlCodes(Cr::CrDeviceHandle hDev) {
    LogSep(L"OBSŁUGIWANE CONTROL CODES (GetSupportedControlCodes)");
    Cr::CrControlCodeInfo* infos = nullptr;
    CrInt32u num = 0;
    Log(L"  >> GetSupportedControlCodes...");
    auto t0 = std::chrono::steady_clock::now();
    Cr::CrError err = Cr::GetSupportedControlCodes(hDev, &infos, &num);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    Logf(L"  << err=0x%04X  count=%u  (%.0f ms)", err, num, ms);
    if (err != 0 || !infos || num == 0) return;

    Logf(L"  Łącznie poleceń: %u", num);
    Log(L"");
    for (CrInt32u i = 0; i < num; ++i) {
        CrInt32u code = static_cast<CrInt32u>(infos[i].GetCode());
        CrInt32u type = static_cast<CrInt32u>(infos[i].GetValueType());
        const wchar_t* name = CtrlName(code);
        wchar_t nameBuf[64];
        if (name) swprintf_s(nameBuf, L"%-40s", name);
        else       swprintf_s(nameBuf, L"0x%08X                              ", code);
        Logf(L"  0x%08X  %s  type=%s", code, nameBuf, DataTypeName(type).c_str());
    }
    Cr::ReleaseControlCodes(hDev, infos);
    Log(L"");
}

// ─── Helper: ExecuteControlCodeValue z logiem ─────────────────────────────────
static bool ExecCtrl(Cr::CrDeviceHandle hDev, Cr::CrControlCode code,
                     CrInt64u value, const wchar_t* desc) {
    const wchar_t* valName = (value == Cr::CrControlButton_Down) ? L"Down" : L"Up";
    Logf(L"  >> ExecuteControlCodeValue(code=0x%08X [%s], val=0x%X [%s])...",
         (unsigned)code, desc, (unsigned)value, valName);
    auto t0 = std::chrono::steady_clock::now();
    Cr::CrError err = Cr::ExecuteControlCodeValue(hDev, code, value);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    Logf(L"  << err=0x%04X  (%.1f ms)  %s", err, ms, err == 0 ? L"OK" : L"FAIL");
    return err == 0;
}

// ─── Helper: SendCommand z logiem ─────────────────────────────────────────────
static bool SendCmd(Cr::CrDeviceHandle hDev, Cr::CrCommandId cmd,
                    Cr::CrCommandParam param, const wchar_t* desc) {
    const wchar_t* paramName = (param == Cr::CrCommandParam_Down) ? L"Down" : L"Up";
    Logf(L"  >> SendCommand(cmd=0x%04X [%s], param=0x%04X [%s])...",
         (unsigned)cmd, desc, (unsigned)param, paramName);
    auto t0 = std::chrono::steady_clock::now();
    Cr::CrError err = Cr::SendCommand(hDev, cmd, param);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    Logf(L"  << err=0x%04X  (%.1f ms)  %s", err, ms, err == 0 ? L"OK" : L"FAIL");
    return err == 0;
}

// ─── Helper: SetDeviceProperty z logiem ───────────────────────────────────────
static bool SetProp(Cr::CrDeviceHandle hDev, Cr::CrDevicePropertyCode code,
                    Cr::CrDataType type, CrInt64u value, const wchar_t* desc) {
    Logf(L"  >> SetDeviceProperty(code=0x%04X [%s], type=%s, val=0x%llX)...",
         (unsigned)code, desc, DataTypeName(type).c_str(), (unsigned long long)value);
    Cr::CrDeviceProperty prop;
    prop.SetCode(code); prop.SetValueType(type); prop.SetCurrentValue(value);
    auto t0 = std::chrono::steady_clock::now();
    Cr::CrError err = Cr::SetDeviceProperty(hDev, &prop);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    Logf(L"  << err=0x%04X  (%.1f ms)  %s", err, ms, err == 0 ? L"OK" : L"FAIL");
    return err == 0;
}

// ─── Helper: najbliższy FNumber ───────────────────────────────────────────────
static CrInt32u NearestFNumber(Cr::CrDeviceHandle hDev, CrInt32u target) {
    CrInt32u code = Cr::CrDeviceProperty_FNumber;
    Cr::CrDeviceProperty* props = nullptr; CrInt32 num = 0;
    Cr::CrError err = Cr::GetSelectDeviceProperties(hDev, 1, &code, &props, &num);
    if (err != 0 || !props || num == 0) {
        if (props) Cr::ReleaseDeviceProperties(hDev, props);
        return target;
    }
    CrInt32u setBytes = props[0].GetSetValueSize();
    auto* setVals = reinterpret_cast<CrInt16u*>(props[0].GetSetValues());
    CrInt32u count = setBytes / sizeof(CrInt16u);
    CrInt32u best = target, bestDiff = UINT32_MAX;
    for (CrInt32u i = 0; i < count; ++i) {
        CrInt32u v = setVals[i];
        CrInt32u d = (v > target) ? (v - target) : (target - v);
        if (d < bestDiff) { bestDiff = d; best = v; }
    }
    Cr::ReleaseDeviceProperties(hDev, props);
    return best;
}

// ─── Helper: najbliższy ShutterSpeed ──────────────────────────────────────────
// Kodowanie: górne 2 bajty = licznik, dolne 2 bajty = mianownik (1/X lub Xs)
// Szuka wartości z listy kamery minimalnie odległej w skali logarytmicznej.
static CrInt32u NearestShutterSpeed(Cr::CrDeviceHandle hDev, CrInt32u target) {
    CrInt32u code = Cr::CrDeviceProperty_ShutterSpeed;
    Cr::CrDeviceProperty* props = nullptr; CrInt32 num = 0;
    Cr::CrError err = Cr::GetSelectDeviceProperties(hDev, 1, &code, &props, &num);
    if (err != 0 || !props || num == 0) {
        if (props) Cr::ReleaseDeviceProperties(hDev, props);
        return target;
    }
    CrInt32u setBytes = props[0].GetSetValueSize();
    auto* setVals = reinterpret_cast<CrInt32u*>(props[0].GetSetValues());
    CrInt32u count = setBytes / sizeof(CrInt32u);

    auto toSeconds = [](CrInt32u v) -> double {
        if (v == 0 || v == 0xFFFFFFFF) return -1.0; // Bulb / Nothing
        CrInt32u n = (v >> 16) & 0xFFFF;
        CrInt32u d = v & 0xFFFF;
        return (d > 0) ? (double)n / d : (double)n;
    };

    double tSec = toSeconds(target);
    if (tSec <= 0.0) {
        Cr::ReleaseDeviceProperties(hDev, props);
        return target;
    }

    CrInt32u best = target;
    double bestDiff = 1e18;
    for (CrInt32u i = 0; i < count; ++i) {
        double vSec = toSeconds(setVals[i]);
        if (vSec <= 0.0) continue;
        double diff = std::abs(std::log(vSec) - std::log(tSec));
        if (diff < bestDiff) { bestDiff = diff; best = setVals[i]; }
    }

    // Loguj całą dostępną listę — żeby widzieć co kamera akceptuje
    Logf(L"  ShutterSpeed — dostępne %u wartości:", count);
    for (CrInt32u i = 0; i < count; ++i) {
        CrInt32u v = setVals[i];
        double s = toSeconds(v);
        if (s > 0.0 && s < 1.0)
            Logf(L"    [%2u] 0x%08X = 1/%.0f s", i, v, 1.0 / s);
        else if (s >= 1.0)
            Logf(L"    [%2u] 0x%08X = %.1f s", i, v, s);
        else
            Logf(L"    [%2u] 0x%08X = Bulb/Nothing", i, v);
    }

    Cr::ReleaseDeviceProperties(hDev, props);
    return best;
}

// ─── main ─────────────────────────────────────────────────────────────────────
int main() {
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT);

    g_logPath = ExeDir() + L"tc_log.txt";
    g_log.open(g_logPath, std::ios::out | std::ios::trunc | std::ios::binary);
    g_log.write("\xEF\xBB\xBF", 3);

    Log(L"╔══════════════════════════════════════════════════════════╗");
    Log(L"║  TotalControl — diagnostyka + wyzwolenie migawki         ║");
    Log(L"╚══════════════════════════════════════════════════════════╝");
    Logf(L"  Log: %s", g_logPath.c_str());
    Log(L"");

    // ── [1] Init ──────────────────────────────────────────────────────────────
    LogSep(L"[1] INIT SDK");
    Log(L"  >> Cr::Init(0)...");
    auto t0 = std::chrono::steady_clock::now();
    bool initOk = Cr::Init(0);
    double initMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    Logf(L"  << %s  (%.0f ms)", initOk ? L"OK" : L"FAIL", initMs);
    if (!initOk) return -1;
    Log(L"");

    // ── [2] Enumerate ─────────────────────────────────────────────────────────
    LogSep(L"[2] SZUKAM KAMER (EnumCameraObjects, timeout=5s)");
    Cr::ICrEnumCameraObjectInfo* camList = nullptr;
    Log(L"  >> EnumCameraObjects(timeout=5)...");
    t0 = std::chrono::steady_clock::now();
    Cr::CrError err = Cr::EnumCameraObjects(&camList, 5);
    double enumMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    Logf(L"  << err=0x%04X  count=%d  (%.0f ms)",
         err, camList ? (int)camList->GetCount() : 0, enumMs);

    if (err != 0 || !camList || camList->GetCount() == 0) {
        Log(L"  Brak kamery. Koniec.");
        if (camList) camList->Release();
        Cr::Release(); return 1;
    }

    const Cr::ICrCameraObjectInfo* rawInfo = camList->GetCameraObjectInfo(0);
    auto* info = const_cast<Cr::ICrCameraObjectInfo*>(rawInfo);
    DumpCameraInfo(rawInfo);
    Log(L"");

    // ── [3] Connect ───────────────────────────────────────────────────────────
    LogSep(L"[3] ŁĄCZĘ");
    auto* cb = new CameraCallback();
    Cr::CrDeviceHandle hDev = 0;
    Log(L"  >> Cr::Connect...");
    t0 = std::chrono::steady_clock::now();
    err = Cr::Connect(info, cb, &hDev);
    double connMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    Logf(L"  << err=0x%04X  hDev=%lld  (%.1f ms)", err, (long long)hDev, connMs);

    camList->Release(); camList = nullptr;

    if (err != 0 || hDev == 0) {
        Log(L"  FAIL: Connect error");
        Cr::Release(); delete cb; return -1;
    }
    Log(L"  Czekam na OnConnected (max 8s)...");
    if (!cb->wait_connected(8000)) {
        Log(L"  FAIL: timeout OnConnected");
        Cr::Disconnect(hDev); Cr::ReleaseDevice(hDev); Cr::Release(); delete cb; return -1;
    }
    Log(L"  Połączono!");
    Log(L"");

    // ── [4] Stabilizacja + dump ────────────────────────────────────────────────
    Log(L"  Stabilizacja 1.5s...");
    Sleep(1500);
    Log(L"");

    DumpAllProperties(hDev);
    DumpControlCodes(hDev);

    // ── [5] Ustawiam parametry ────────────────────────────────────────────────
    LogSep(L"[5] USTAWIAM PARAMETRY EKSPOZYCJI");
    // Official SDK tip: set PriorityKey=PCRemote FIRST — required for ExposureMode/FocusMode/Shutter
    SetProp(hDev, Cr::CrDeviceProperty_PriorityKeySettings, Cr::CrDataType_UInt16,
            Cr::CrPriorityKey_PCRemote, L"PriorityKeySettings=PCRemote");
    Sleep(300);
    SetProp(hDev, Cr::CrDeviceProperty_ExposureProgramMode, Cr::CrDataType_UInt32,
            Cr::CrExposure_M_Manual, L"ExposureMode=Manual");
    Sleep(300);
    SetProp(hDev, Cr::CrDeviceProperty_FocusMode, Cr::CrDataType_UInt16,
            Cr::CrFocus_MF, L"FocusMode=MF");
    SetProp(hDev, Cr::CrDeviceProperty_IsoSensitivity, Cr::CrDataType_UInt32,
            100, L"ISO=100");
    CrInt32u fnum = NearestFNumber(hDev, 400);
    Logf(L"  Wybrane FNumber: %u (F/%.1f)", fnum, fnum / 100.0);
    SetProp(hDev, Cr::CrDeviceProperty_FNumber, Cr::CrDataType_UInt16,
            fnum, L"FNumber=F4.0");

    // Cel: 1/100s — szukamy najbliższej wartości z listy kamery
    CrInt32u ssTarget = (1u << 16) | 100u;  // 1/100s jako punkt startowy
    CrInt32u ssVal = NearestShutterSpeed(hDev, ssTarget);
    CrInt32u ssNum = (ssVal >> 16) & 0xFFFF;
    CrInt32u ssDen = ssVal & 0xFFFF;
    if (ssDen > 0)
        Logf(L"  Wybrana ShutterSpeed: 0x%08X = %u/%u s (%.4f s)", ssVal, ssNum, ssDen, (double)ssNum / ssDen);
    else
        Logf(L"  Wybrana ShutterSpeed: 0x%08X = %u s", ssVal, ssNum);
    SetProp(hDev, Cr::CrDeviceProperty_ShutterSpeed, Cr::CrDataType_UInt32,
            ssVal, L"ShutterSpeed~1/100s");
    Sleep(800);  // daj kamerze czas na zatwierdzenie nowej wartości

    // HostPC mode suppresses CrNotify_Captured_Event (no card write).
    // Force MemoryCard so the capture callback fires reliably.
    SetProp(hDev, Cr::CrDeviceProperty_StillImageStoreDestination, Cr::CrDataType_UInt16,
            Cr::CrStillImageStoreDestination_MemoryCard, L"StoreDestination=MemoryCard");
    Log(L"  Czekam 500ms na zastosowanie...");
    Sleep(500);
    Log(L"");

    // ── [6] Wyzwalanie migawki ─────────────────────────────────────────────────
    // Oficjalna metoda z dokumentacji SDK (sample_afShooting.html + op_send_a_control_command.html):
    // SendCommand(Release, Down) → 35ms → SendCommand(Release, Up)
    // Tryb MF → pomijamy oczekiwanie na AF (CrDeviceProperty_S1 / FocusIndication)
    LogSep(L"[6] WYZWALAM MIGAWKĘ  (SendCommand CrCommandId_Release — oficjalna metoda SDK)");
    cb->shotFired = false;
    auto tShot0 = std::chrono::steady_clock::now();

    SendCmd(hDev, Cr::CrCommandId_Release, Cr::CrCommandParam_Down, L"Release");
    Sleep(35);  // oficjalny sample: 35ms między Down a Up
    SendCmd(hDev, Cr::CrCommandId_Release, Cr::CrCommandParam_Up,   L"Release");

    Log(L"  Czekam na potwierdzenie ekspozycji (max 5s)...");
    bool shotOk = cb->wait_shot(5000);
    auto tShotEnd = std::chrono::steady_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(tShotEnd - tShot0).count();

    Log(L"");
    Logf(L"  Wynik           : %s", shotOk ? L"✓ OK" : L"✗ TIMEOUT");
    Logf(L"  Czas total      : %.0f ms", totalMs);
    Log(L"");

    // ── [7] Rozłącz ───────────────────────────────────────────────────────────
    LogSep(L"[7] ROZŁĄCZAM");
    Log(L"  >> Cr::Disconnect...");
    Cr::Disconnect(hDev);
    Cr::ReleaseDevice(hDev);
    Cr::Release();
    delete cb;
    Log(L"  << Gotowe");
    Log(L"");

    Log(shotOk ? L"╔══ ZAKOŃCZONO POMYŚLNIE ══╗" : L"╔══ ZAKOŃCZONO Z BŁĘDEM ══╗");
    g_log.close();
    return shotOk ? 0 : 1;
}
