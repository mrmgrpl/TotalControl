#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace TotalControl {

// BufferCapacity: one-off operator calibration test ("Buffer Capacity
// Calibration" preset) -- holds the shutter for TLBlock::burstDurMs at
// TLBlock::burstDrive and measures how many shots fit before the camera's
// shooting rate slows down (buffer full, falling back to card-write-limited
// speed). Reuses existing fields rather than adding new ones; not part of
// normal photography sequences. Also the source of card write-speed
// calibration now (via the post-slowdown sustained fps) -- a separate
// CardCalib block type existed earlier (value 4, now retired/unused) but
// was removed: a short duration-held burst can never actually reach
// card-write-limited speed, only buffer-fill-limited speed, so it couldn't
// measure the thing it claimed to.
enum class BlockType : int {
    Single = 0, Burst = 1, Bracket = 2, Audio = 3, BufferCapacity = 5
};

// A block anchored to C2/C3 survives an observer-location change: when the
// location moves, C1..C4 all shift (and totality duration itself can grow or
// shrink), but a block's OFFSET from its anchor contact stays fixed, so the
// block moves with it instead of staying frozen at its old absolute time.
// None = legacy/unanchored (absolute atMs only, e.g. no totality at this
// location, or contacts not calculated yet).
enum class TLAnchor : int { None = 0, C2 = 1, C3 = 2 };

struct TLBlock {
    int64_t     id         = -1;       // DB row id (-1 = unsaved)
    BlockType   type       = BlockType::Single;
    int64_t     atMs       = -1;       // absolute UTC ms -- always the CURRENT
                                        // resolved time; source of truth when
                                        // anchor==None, else kept in sync with
                                        // anchor+anchorOffsetMs by
                                        // App::ResyncTimelineAnchors()
    TLAnchor    anchor         = TLAnchor::None;
    int64_t     anchorOffsetMs = 0;    // atMs - contact time, when anchored

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
