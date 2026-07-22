#pragma once
#include <cstdint>
#include <string>

namespace TotalControl {

// Decodes a raw Sony CrDeviceProperty_DriveMode (0x010e) value into a
// descriptive string (e.g. "bracket-1ev-5-cont"). Falls back to "0xHHHHHHHH"
// for values not covered by the table.
//
// Single source of truth for BOTH CameraController::GetStatus() (live camera
// status shown in the GUI/CLI) and CommandHandler's generic "get" property
// decoder — previously each kept its own hand-written copy of this table,
// and CameraController's copy only covered a handful of modes (falling back
// to raw hex for the other bracket variants), which is why the GUI showed
// "0x00040310" etc. instead of a name for cameras left in a non-default
// bracket mode. See Change log.
std::wstring DecodeDriveMode(uint32_t raw);

} // namespace TotalControl
