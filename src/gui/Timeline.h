#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace TotalControl {

enum class BlockType : int { Single = 0, Burst = 1, Bracket = 2, Audio = 3 };

struct TLBlock {
    int64_t     id         = -1;       // DB row id (-1 = unsaved)
    BlockType   type       = BlockType::Single;
    int64_t     atMs       = -1;       // absolute UTC ms

    // Camera block params
    std::string ss         = "1/100";
    int         iso        = 100;
    std::string fstop      = "8.0";
    int         count      = 5;        // bracket: shot count (3/5/9)
    std::string ev         = "1.0ev";  // bracket: EV step (0.3ev…3.0ev)
    std::string burstDrive = "cont-hi-plus";
    int32_t     burstDurMs = 3000;

    // Audio block params
    std::string audioFile;
    int32_t     audioDurMs = 10000;

    // Common
    std::string label;
    bool        snapToPrev = false;
    bool        snapToSec  = false;    // rounds atMs to the nearest whole second
};

struct TLTrack {
    int64_t              id       = -1;
    std::string          type;          // "camera" | "audio"
    std::string          cameraId;
    std::string          label;
    int                  focalMm  = 0;  // lens focal length in mm (0 = unset)
    std::vector<TLBlock> blocks;

    bool IsCamera() const { return type == "camera"; }
    bool IsAudio()  const { return type == "audio";  }
};

} // namespace TotalControl
