#pragma once

// Shared UI contract (docs/02 §2.x, spec §10.12–10.13, namespace vw::ui).
// Authoritative reference: spec §10.13. Deliberately free of Windows.h and
// engine types so ANY module can include it — the UI reads engine state
// through these pure data types and writes through Command.
//
// Delivery model (spec §10.12):
//   (a) UiSnapshot getUiSnapshot() — one-time pull on window open.
//   (b) INotificationSink — push deltas, invoked on the UI thread.
// Telemetry is pushed ~1–2 Hz while ≥1 subscriber (and only then); all other
// events are immediate. Paths/ids are std::wstring (native Win32); HMONITOR
// is an opaque uintptr_t; geometry is plain int32.

#include <cstdint>
#include <string>
#include <vector>

namespace vw::ui {

// ---- stable ids (strings; empty = none) ----
using MonitorId     = std::wstring;
using PlaylistId    = std::wstring;
using LibraryItemId = uint64_t;

// ---- enums ----
enum class VideoCodec : uint8_t    { Unknown, H264, HEVC, AV1, VP9 };
enum class ScalingMode : uint8_t   { Fill, Fit, Stretch, Center };
enum class PlaylistMode : uint8_t  { Single, Sequential, Loop, Shuffle };
enum class BatteryMode : uint8_t   { Continue, ReduceQuality, Pause };
enum class PlaybackState : uint8_t { NoWallpaper, Playing, Paused, Suspended };
enum class WallpaperSource : uint8_t { None, File, Playlist };
enum class MonitorEventKind : uint8_t   { Added, Removed, Changed };
enum class LibraryChangeKind : uint8_t  { Added, Removed, Updated, RescanStarted, RescanFinished };
enum class PlaylistChangeKind : uint8_t { Created, Renamed, Deleted, Duplicated, ItemsChanged, ModeChanged };

// Pause-reason bitmask (mirrors governor::Reason by NAME; the bit numbering
// follows spec §10.13 — convert in the app via uiPauseReasons()).
enum PauseReason : uint32_t {
    None            = 0,
    User            = 1u << 0,
    Game            = 1u << 1,
    Fullscreen      = 1u << 2,
    HighCPU         = 1u << 3,
    HighGPU         = 1u << 4,
    HighMemory      = 1u << 5,
    Battery         = 1u << 6,
    Locked          = 1u << 7,
    DisplayOff      = 1u << 8,
    MonitorHidden   = 1u << 9,
    SystemSuspended = 1u << 10,
};

// ---- media / monitor data (mirrors docs/01 §1.5.1, docs/02 §2.7) ----
struct VideoMetadata {
    uint32_t width = 0, height = 0;
    double frameRate = 0, durationSeconds = 0;
    VideoCodec codec = VideoCodec::Unknown;
    bool hdr = false;
    bool hasAudio = false;
};

struct MonitorInfo {                      // HMONITOR exposed as opaque handle
    MonitorId id;
    uintptr_t handle = 0;
    int32_t x = 0, y = 0, width = 0, height = 0;        // bounds
    int32_t workX = 0, workY = 0, workW = 0, workH = 0; // work area
    uint32_t refreshNum = 0, refreshDen = 0;            // refresh rate
    bool primary = false;
    bool active = false;
};

struct LibraryItem {
    LibraryItemId id = 0;
    std::wstring path;
    VideoMetadata metadata;
    uint64_t fileSize = 0;
    uint64_t lastWriteTicks = 0;          // FILETIME
    bool metadataLoaded = false;
};

// ---- notifications (read side) ----
struct TelemetrySnapshot {                // mirrors PerformanceStats — docs/01 §1.5.14
    double cpuUsage = 0, gpuUsage = 0;    // % (gpu: engine estimate, see R-03)
    uint64_t gpuMemoryUsed = 0, gpuMemoryBudget = 0, systemMemoryUsed = 0;
    double decodedFps = 0, presentedFps = 0;
    uint64_t droppedFrames = 0;
    double decodeLatencyMs = 0, renderTimeMs = 0;
    bool hardwareDecode = false;
    struct PerMonitor { MonitorId monitorId; double presentedFps = 0; uint64_t droppedFrames = 0; };
    std::vector<PerMonitor> perMonitor;   // only monitors with an active wallpaper
};

struct PlaybackStateNotification {        // per-monitor status (Home + tray tooltip)
    PlaybackState state = PlaybackState::NoWallpaper;
    uint32_t pauseReasons = 0;            // PauseReason bitmask
    MonitorId monitorId;
    std::wstring videoName, playlistName;
    std::wstring decoderMode, adapterName; // e.g. "NVIDIA NVDEC / hardware"
};

struct MonitorEvent { MonitorEventKind kind; MonitorInfo info; };

struct LibraryChangeNotification {
    LibraryChangeKind kind;
    std::vector<LibraryItemId> ids;       // affected items
};

struct PlaylistChangeNotification { PlaylistChangeKind kind; PlaylistId id; };

struct WallpaperAssignmentNotification {
    MonitorId monitorId;
    WallpaperSource source = WallpaperSource::None;
    std::wstring sourceId;                // file path or playlist id
    ScalingMode scaling = ScalingMode::Fill;
    bool clone = false;                   // true = Clone, false = Independent
};

struct PlaylistSummary {
    PlaylistId id;
    std::wstring name;
    size_t itemCount = 0;
    PlaylistMode mode = PlaylistMode::Sequential;
    bool loop = true, shuffle = false;
};

// M11 v1 deviation (documented): the engine has ONE playlist (shared-playlist
// mode is v2), so the Playlists panel manages its items directly. Item view is
// engine-free (paths + cached metadata, no decoded frames).
struct PlaylistItemView {
    std::wstring path;
    int64_t start100ns = 0, end100ns = 0;
    bool enabled = true;
    int64_t duration100ns = 0;
    uint32_t width = 0, height = 0;
    std::wstring codec;
};

// M11 v1 deviation (documented): the Performance/Settings panels need the
// CURRENT config values on open (spec's consumption table says "values come
// from the snapshot on open" but its UiSnapshot omitted them). Flat mirror of
// the engine config; panels write via CONFIG_SET commands.
struct ConfigSnapshot {
    bool pauseOnGame = true, pauseOnFullscreen = true;
    bool pauseOnHighCPU = true, pauseOnHighGPU = true, pauseOnHighRAM = false;
    int cpuPauseThreshold = 85, cpuResumeThreshold = 65;
    int gpuPauseThreshold = 90, gpuResumeThreshold = 70;
    int memoryPauseThreshold = 90, memoryResumeThreshold = 75;
    int pauseDelaySeconds = 3, resumeDelaySeconds = 5;
    // frameQueue default 1 — consumer-paced decode (see ConfigurationManager:
    // depth 1 makes stale-frame drops impossible; matches the config default).
    int longPauseReleaseSeconds = 2, frameQueue = 1;
    BatteryMode batteryMode = BatteryMode::Pause;
    PlaylistMode playbackMode = PlaylistMode::Loop;
    bool loop = true;
    ScalingMode scaling = ScalingMode::Fill;
    double playbackSpeed = 1.0; // playback speed multiplier (0.25–4.0)
    bool audio = true;
    int volume = 80; // 0–100
    bool clone = false; // wallpaper mode: true = Clone, false = Independent
    bool startWithWindows = false, minimizeToTray = true, startMinimized = false;
    bool fileAssociations = false;
    std::wstring logLevel = L"info";
};

// ---- commands (write side; posted to the controller queue, fire-and-forget) ----
// Field usage per CommandId follows the §10.10 table:
//   s1/s2   = monitorId, path, playlistId, name, or CONFIG_SET key/value
//   paths   = bulk file paths          itemIds = library ids
//   i1/i2   = playlist item index, move delta
//   d1/d2   = optional start/end time (PlaylistSetItemTimes) or CONFIG_SET numbers
//   b1      = toggles (enabled, loop, clone, minimizeToTray, …)
//   scaling = SetScaling               mode = PlaylistSetMode / SetGlobalMode(clone via b1)
enum class CommandId : uint8_t {
    PlayPauseToggle, Pause, Resume, Next, Previous,
    SetWallpaperFile, SetWallpaperPlaylist, SetGlobalMode, SetScaling, GrabFrameSnapshot,
    PlaylistCreate, PlaylistRename, PlaylistDelete, PlaylistDuplicate,
    PlaylistAddFiles, PlaylistAddLibraryItems, PlaylistRemoveItem, PlaylistMoveItem,
    PlaylistToggleItem, PlaylistSetItemTimes, PlaylistSetMode, PlaylistSetLoop,
    PlaylistExport, PlaylistImport,
    LibraryAddFiles, LibraryAddFolder, LibraryRemove, LibraryRefresh,
    ConfigSet, ConfigExport, ConfigImport, SetAudioVolume,
    ShowUi, ToggleUi, Focus, Exit,
};

struct Command {                          // tagged POD payload; cheap to queue
    CommandId id = CommandId::PlayPauseToggle;
    std::wstring s1, s2;
    std::vector<std::wstring> paths;
    std::vector<LibraryItemId> itemIds;
    int32_t i1 = 0, i2 = 0;
    double d1 = 0, d2 = 0;
    bool b1 = false;
    ScalingMode scaling = ScalingMode::Fill;
    PlaylistMode mode = PlaylistMode::Sequential;
};

// ---- one-time snapshot (pull on window open) ----
struct UiSnapshot {
    TelemetrySnapshot telemetry;
    std::vector<PlaybackStateNotification> playbackStates; // per active monitor
    std::vector<MonitorInfo> monitors;
    std::vector<LibraryItem> libraryItems;
    std::vector<PlaylistSummary> playlists;
    std::vector<PlaylistItemView> playlistItems; // M11 v1: single-playlist items
    std::vector<WallpaperAssignmentNotification> assignments;
    ConfigSnapshot config; // M11 v1: current config for the read-only panels
};

// ---- sink: push notifications; invoked on the UI thread ----
struct INotificationSink {
    virtual void onTelemetry(const TelemetrySnapshot&) = 0;             // 1–2 Hz
    virtual void onPlaybackState(const PlaybackStateNotification&) = 0; // per monitor
    virtual void onMonitorEvent(const MonitorEvent&) = 0;
    virtual void onLibraryChange(const LibraryChangeNotification&) = 0;
    virtual void onPlaylistChange(const PlaylistChangeNotification&) = 0;
    virtual void onWallpaperAssignment(const WallpaperAssignmentNotification&) = 0;
    virtual ~INotificationSink() = default;
};

} // namespace vw::ui
