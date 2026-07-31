/*
 *
 *    Copyright (c) 2025 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once
#include "camera-av-stream-manager.h"
#include "camera-avsettingsuserlevel-manager.h"
#include "camera-device-interface.h"
#include "chime-manager.h"
#include "push-av-stream-manager.h"
#include "webrtc-provider-manager.h"
#include "zone-manager.h"

#include "default-media-controller.h"
#include <protocols/interaction_model/StatusCode.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <gst/gst.h>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#define STREAM_GST_DEST_IP "127.0.0.1"
#define VIDEO_STREAM_GST_DEST_PORT 5000
#define AUDIO_STREAM_GST_DEST_PORT 5001

// Camera Constraints set to typical values.
// TODO: Look into ways to fetch from hardware, if required/possible.
static constexpr uint32_t kMaxContentBufferSizeBytes = 4096;
static constexpr uint32_t kMaxNetworkBandwidthbps    = 128000000; // 128 Mbps
static constexpr uint8_t kMaxConcurrentEncoders      = 1;
static constexpr uint8_t kSpeakerMinLevel            = 1;
static constexpr uint8_t kSpeakerMaxLevel            = 254;       // Spec constraint
static constexpr uint8_t kSpeakerMaxChannelCount     = 8;         // Same as Microphone
static constexpr uint32_t kMaxEncodedPixelRate       = 248832000; // 1080p at 120fps(1920 * 1080 * 120)
static constexpr uint8_t kMicrophoneMinLevel         = 1;
static constexpr uint8_t kMicrophoneMaxLevel         = 254;  // Spec constraint
static constexpr uint8_t kMicrophoneMaxChannelCount  = 8;    // Spec Constraint in AudioStreamAllocate
static constexpr uint16_t kMinResolutionWidth        = 640;  // Low VGA resolution
static constexpr uint16_t kMinResolutionHeight       = 480;  // Low VGA resolution
static constexpr uint16_t k720pResolutionWidth       = 1280; // 720p resolution
static constexpr uint16_t k720pResolutionHeight      = 720;  // 720p resolution
static constexpr uint16_t kMaxResolutionWidth        = 1920; // 1080p resolution
static constexpr uint16_t kMaxResolutionHeight       = 1080; // 1080p resolution
static constexpr uint16_t kSnapshotStreamFrameRate   = 30;
static constexpr uint16_t kMaxVideoFrameRate         = 120;
static constexpr uint16_t k60fpsVideoFrameRate       = 60;
static constexpr uint16_t k30fpsVideoFrameRate       = 30;
static constexpr uint16_t kMinVideoFrameRate         = 15;
static constexpr uint32_t kMinBitRateBps             = 10000;   // 10 kbps
static constexpr uint32_t kMaxBitRateBps             = 2000000; // 2 mbps
static constexpr uint32_t kKeyFrameIntervalMsec      = 4000;    // 4 sec; recommendation from Spec
static constexpr uint16_t kVideoSensorWidthPixels    = 1920;    // 1080p resolution
static constexpr uint16_t kVideoSensorHeightPixels   = 1080;    // 1080p resolution
static constexpr uint16_t kMinImageRotation          = 0;
static constexpr uint16_t kMaxImageRotation          = 359; // Spec constraint
static constexpr uint8_t kMaxZones                   = 10;  // Spec has min 1
static constexpr uint8_t kMaxUserDefinedZones        = 10;  // Spec has min 5
static constexpr uint8_t kSensitivityMax             = 10;  // Spec has 2 to 10

// StreamIDs typically start from 0 and monotonically increase. Setting
// Invalid value to a large and practically unused value.
static constexpr uint16_t kInvalidStreamID = 65500;
#define INVALID_SPKR_LEVEL (0)

namespace Camera {

constexpr const char * kDefaultVideoDevicePath = "/dev/video0";

// Per-camera ONVIF connection config.  Populated from cameras.json (multi-camera
// bridge) or from CLI options (single-camera backward-compat path in main.cpp).
enum class InboundAudioCodec { kNone, kPcmu, kPcma, kOpus, kUnsupported };

struct InboundAudioCapability
{
    bool verifiedByRtspSdp = false;
    InboundAudioCodec codec = InboundAudioCodec::kNone;
    uint32_t clockRateHz = 0;
    uint8_t channels = 0;
};

// Per-camera ONVIF connection config.  Populated from cameras.json (multi-camera
// bridge) or from CLI options (single-camera backward-compat path in main.cpp).
struct OnvifConfig
{
    std::string rtspUrl;     // RTSP stream URL, e.g. rtsp://192.168.1.100/live/ch00_0
    std::string ptzUrl;      // ONVIF PTZ service URL
    std::string snapshotUrl; // ONVIF GetSnapshotUri JPEG URL ("" if unsupported)
    std::string token;       // ONVIF profile token
    std::string user;
    std::string pass;
    std::string videoCodec = "H264"; // real RTSP stream codec ("H264" | "H265"), detected from
                                     // the SDP probe at onboard and persisted in cameras.json.
                                     // Selects the depayloader/parse elements and the WebRTC
                                     // packetizer; both codecs are pure passthrough (no transcode).
    bool useTestSrc = false; // when true (rtspUrl empty), use a GStreamer test pattern
                             // instead of RTSP/V4L2 — for hardware-free multi-camera testing
    bool needsBasicAuth = false; // this camera's RTSP Digest is broken (verified
                             // Hikvision firmware bug: rejects a correct password,
                             // but accepts the SAME credentials via Basic). Set from
                             // onvif_resolved_bridge_t::needs_basic_auth /
                             // rtsp_probe_result_t::used_basic_fallback during
                             // onboarding. rtspsrc has no Basic fallback of its own
                             // (unlike our onvif_rtsp_probe), so when true the video
                             // pipeline must force Basic auth itself (see the
                             // "before-send" hook in camera-device.cpp) or every
                             // connection attempt fails with "Unauthorized" even
                             // with the right password.
    InboundAudioCapability audioCapability;

    bool HasVerifiedAudio() const {
        return audioCapability.verifiedByRtspSdp &&
               (audioCapability.codec == InboundAudioCodec::kPcmu ||
                audioCapability.codec == InboundAudioCodec::kPcma);
    }
};

// Camera defined constants for Pan, Tilt, Zoom bounding values
constexpr int16_t kMinPanValue  = -90;
constexpr int16_t kMaxPanValue  = 90;
constexpr int16_t kMinTiltValue = -90;
constexpr int16_t kMaxTiltValue = 90;
constexpr uint8_t kMaxZoomValue = 75;

class CameraDevice : public CameraDeviceInterface, public CameraDeviceInterface::CameraHALInterface
{
public:
    chip::app::Clusters::ChimeDelegate & GetChimeDelegate() override;
    chip::app::Clusters::WebRTCTransportProvider::Delegate & GetWebRTCProviderDelegate() override;
    void
    SetWebRTCTransportProvider(chip::app::Clusters::WebRTCTransportProvider::WebRTCTransportProviderCluster * provider) override;
    chip::app::Clusters::CameraAvStreamManagement::CameraAVStreamManagementDelegate & GetCameraAVStreamMgmtDelegate() override;
    chip::app::Clusters::CameraAvStreamManagement::CameraAVStreamController & GetCameraAVStreamMgmtController() override;
    chip::app::Clusters::CameraAvSettingsUserLevelManagementDelegate & GetCameraAVSettingsUserLevelMgmtDelegate() override;
    chip::app::Clusters::PushAvStreamTransportDelegate & GetPushAVTransportDelegate() override;
    chip::app::Clusters::ZoneManagement::Delegate & GetZoneManagementDelegate() override;

    MediaController & GetMediaController() override;

    CameraDevice();
    ~CameraDevice();

    CameraDeviceInterface::CameraHALInterface & GetCameraHALInterface() override { return *this; }

    void Init();
    void Shutdown();

    // HAL interface impl
    CameraError InitializeCameraDevice() override;

    CameraError InitializeStreams() override;

    CameraError CaptureSnapshot(const chip::app::DataModel::Nullable<uint16_t> streamID, const VideoResolutionStruct & resolution,
                                ImageSnapshot & outImageSnapshot) override;

    CameraError StartVideoStream(const VideoStreamStruct & allocatedStream) override;

    // Start a video stream by ID (looks up the allocated stream params). Used by the
    // media controller to start the pipeline on-demand when the first viewer joins.
    CameraError StartVideoStreamByID(uint16_t streamID);

    // Stop video stream
    CameraError StopVideoStream(uint16_t streamID) override;

    // Start audio stream
    CameraError StartAudioStream(uint16_t streamID) override;

    // Stop audio stream
    CameraError StopAudioStream(uint16_t streamID) override;

    // Allocate snapshot stream
    CameraError AllocateSnapshotStream(
        const chip::app::Clusters::CameraAvStreamManagement::CameraAVStreamManagementDelegate::SnapshotStreamAllocateArgs & args,
        uint16_t & outStreamID) override;

    // Start snapshot stream
    CameraError StartSnapshotStream(uint16_t streamID) override;

    // Stop snapshot stream
    CameraError StopSnapshotStream(uint16_t streamID) override;

    uint8_t GetMaxConcurrentEncoders() override;

    uint32_t GetMaxEncodedPixelRate() override;

    VideoSensorParamsStruct & GetVideoSensorParams() override;

    bool GetCameraSupportsHDR() override;

    bool GetCameraSupportsNightVision() override;

    bool GetNightVisionUsesInfrared() override;

    bool GetCameraSupportsWatermark() override;

    bool GetCameraSupportsOSD() override;

    bool GetCameraSupportsSoftPrivacy() override;

    bool GetCameraSupportsImageControl() override;

    VideoResolutionStruct & GetMinViewport() override;

    std::vector<RateDistortionTradeOffStruct> & GetRateDistortionTradeOffPoints() override;

    uint32_t GetMaxContentBufferSize() override;

    AudioCapabilitiesStruct & GetMicrophoneCapabilities() override;

    AudioCapabilitiesStruct & GetSpeakerCapabilities() override;

    std::vector<SnapshotCapabilitiesStruct> & GetSnapshotCapabilities() override;

    uint32_t GetMaxNetworkBandwidth() override;

    uint16_t GetCurrentFrameRate() override;

    CameraError SetHDRMode(bool hdrMode) override;
    bool GetHDRMode() override { return mHDREnabled; }

    CameraError SetHardPrivacyMode(bool hardPrivacyMode) override;
    bool GetHardPrivacyMode() override { return mHardPrivacyModeOn; }

    CameraError SetNightVision(chip::app::Clusters::CameraAvStreamManagement::TriStateAutoEnum nightVision) override;
    chip::app::Clusters::CameraAvStreamManagement::TriStateAutoEnum GetNightVision() override { return mNightVision; }

    std::vector<StreamUsageEnum> & GetSupportedStreamUsages() override;

    std::vector<StreamUsageEnum> & GetStreamUsagePriorities() override { return mStreamUsagePriorities; }
    CameraError SetStreamUsagePriorities(std::vector<StreamUsageEnum> streamUsagePriorities) override;

    // Sets the Default Camera Viewport
    CameraError SetViewport(const chip::app::Clusters::Globals::Structs::ViewportStruct::Type & viewPort) override;
    const chip::app::Clusters::Globals::Structs::ViewportStruct::Type & GetViewport() override { return mViewport; }

    /**
     * Sets the Viewport for a specific stream. The implementation of this HAL API is responsible
     * for updating the stream identified with the provided viewport. The invoker of this
     * API shall have already ensured that the provided viewport conforms to the specification
     * requirements on size and aspect ratio.
     *
     * @param stream   the currently allocated video stream on which the viewport is being set
     * @param viewport the viewport to be set on the stream
     */
    CameraError SetViewport(VideoStream & stream,
                            const chip::app::Clusters::Globals::Structs::ViewportStruct::Type & viewport) override;

    // Get/Set SoftRecordingPrivacyMode.
    CameraError SetSoftRecordingPrivacyModeEnabled(bool softRecordingPrivacyMode) override;
    bool GetSoftRecordingPrivacyModeEnabled() override { return mSoftRecordingPrivacyModeEnabled; }

    // Get/Set SoftLivestreamPrivacyMode.
    CameraError SetSoftLivestreamPrivacyModeEnabled(bool softLivestreamPrivacyMode) override;
    bool GetSoftLivestreamPrivacyModeEnabled() override { return mSoftLivestreamPrivacyModeEnabled; }

    // Currently, defaulting to supporting hard privacy switch.
    bool HasHardPrivacySwitch() override { return true; }

    // Currently, defaulting to supporting speaker.
    bool HasSpeaker() override { return true; }

    // Mute/Unmute speaker.
    CameraError SetSpeakerMuted(bool muteSpeaker) override;
    bool GetSpeakerMuted() override { return mSpeakerMuted; }

    // Get/Set speaker volume level.
    CameraError SetSpeakerVolume(uint8_t speakerVol) override;
    uint8_t GetSpeakerVolume() override { return mSpeakerVol; }

    // Get the speaker max and min levels.
    uint8_t GetSpeakerMaxLevel() override { return mSpeakerMaxLevel; }
    uint8_t GetSpeakerMinLevel() override { return mSpeakerMinLevel; }

    // Does camera have a microphone. Kept true even though our ONVIF cameras have no audio
    // backchannel: SmartThings' camera plugin rejects clip recording outright ("Unsupported
    // functionality") for cameras without the audio feature. The Push AV recorder tolerates a
    // silent audio track (libav's opus encoder is enabled; the DASH muxer emits both init
    // segments at header time, so an empty audio representation does not stall uploads).
    bool HasMicrophone() override { return true; }

    // Mute/Unmute microphone.
    CameraError SetMicrophoneMuted(bool muteMicrophone) override;
    bool GetMicrophoneMuted() override { return mMicrophoneMuted; }

    // Set microphone volume level.
    CameraError SetMicrophoneVolume(uint8_t microphoneVol) override;
    uint8_t GetMicrophoneVolume() override { return mMicrophoneVol; }

    // Get the microphone max and min levels.
    uint8_t GetMicrophoneMaxLevel() override { return mMicrophoneMaxLevel; }
    uint8_t GetMicrophoneMinLevel() override { return mMicrophoneMinLevel; }

    // Get/Set image control attributes
    CameraError SetImageRotation(uint16_t imageRotation) override;
    uint16_t GetImageRotation() override { return mImageRotation; }

    CameraError SetImageFlipHorizontal(bool imageFlipHorizontal) override;
    bool GetImageFlipHorizontal() override { return mImageFlipHorizontal; }

    CameraError SetImageFlipVertical(bool imageFlipVertical) override;
    bool GetImageFlipVertical() override { return mImageFlipVertical; }

    // Does camera have local storage
    bool HasLocalStorage() override { return true; }

    // Set/Get LocalVideoRecordingEnabled
    CameraError SetLocalVideoRecordingEnabled(bool localVideoRecordingEnabled) override;
    bool GetLocalVideoRecordingEnabled() override { return mLocalVideoRecordingEnabled; }

    // Set/Get LocalSnapshotRecordingEnabled
    CameraError SetLocalSnapshotRecordingEnabled(bool localSnapshotRecordingEnabled) override;
    bool GetLocalSnapshotRecordingEnabled() override { return mLocalSnapshotRecordingEnabled; }

    // Does camera have a status light
    bool HasStatusLight() override { return true; }

    // Set/Get StatusLightEnabled
    CameraError SetStatusLightEnabled(bool statusLightEnabled) override;
    bool GetStatusLightEnabled() override { return mStatusLightEnabled; }

    int16_t GetPanMin() override;

    int16_t GetPanMax() override;

    int16_t GetTiltMin() override;

    int16_t GetTiltMax() override;

    uint8_t GetZoomMax() override;

    uint8_t GetMaxZones() override { return kMaxZones; }

    uint8_t GetMaxUserDefinedZones() override { return kMaxUserDefinedZones; }

    uint8_t GetSensitivityMax() override { return kSensitivityMax; }

    uint8_t GetDetectionSensitivity() override { return mDetectionSensitivity; }

    size_t GetPreRollBufferSize();

    int64_t GetMinKeyframeIntervalMs();

    CameraError SetDetectionSensitivity(uint8_t aSensitivity) override;

    CameraError CreateZoneTrigger(const chip::app::Clusters::ZoneManagement::ZoneTriggerControlStruct & zoneTrigger) override;

    CameraError UpdateZoneTrigger(const chip::app::Clusters::ZoneManagement::ZoneTriggerControlStruct & zoneTrigger) override;

    CameraError RemoveZoneTrigger(uint16_t zoneId) override;

    CameraError SetPan(int16_t aPan) override;
    CameraError SetTilt(int16_t aTilt) override;
    CameraError SetZoom(uint8_t aZoom) override;
    CameraError SetPhysicalPTZ(chip::Optional<int16_t> aPan, chip::Optional<int16_t> aTilt, chip::Optional<uint8_t> aZoom) override;

    std::vector<VideoStream> & GetAvailableVideoStreams() override { return mVideoStreams; }

    std::vector<AudioStream> & GetAvailableAudioStreams() override { return mAudioStreams; }

    std::vector<SnapshotStream> & GetAvailableSnapshotStreams() override { return mSnapshotStreams; }

    void SetVideoDevicePath(const std::string & path) { mVideoDevicePath = path; }

    // Per-instance ONVIF connection config (replaces the global LinuxDeviceOptions singleton
    // for multi-camera operation).  Call before Init().
    void SetOnvifConfig(const OnvifConfig & config) { mOnvifConfig = config; }
    const OnvifConfig & GetOnvifConfig() const { return mOnvifConfig; }

    // CameraDeviceInterface: real RTSP-source codec ("H264" | "H265"), used by the WebRTC layer
    // to select the matching depayloader/packetizer instead of assuming H264. Locked: the
    // self-healing correction below rewrites this from the watchdog thread while the WebRTC
    // thread reads it here (see mCodecMutex).
    std::string GetVideoCodec() override
    {
        std::lock_guard<std::mutex> lk(mCodecMutex);
        return mOnvifConfig.videoCodec;
    }

    // ---- Self-healing video-codec correction ---------------------------------------------
    // The persisted videoCodec comes from the RTSP SDP probe at onboarding, falling back to
    // the camera's ONVIF GetProfiles metadata when that probe fails (2 s timeout, no retry).
    // Some cameras report a codec in ONVIF that does not match what their RTSP stream
    // actually carries, and once a wrong value is persisted it is sticky: the pipeline is
    // built with the wrong depayloader, rtspsrc's video pad cannot be linked, and live view
    // stays dead until somebody re-onboards the camera by hand.
    //
    // rtspsrc's pad caps carry the stream's TRUE encoding, so the pad-added callback reports
    // it here and the watchdog — which already owns pipeline teardown/rebuild — applies the
    // correction and rebuilds with the right depayloader. Bounded (kMaxCodecCorrections) so a
    // camera that genuinely alternates codecs cannot drive a rebuild loop.
    //
    // SCOPE: this repairs the GStreamer pipeline, not the WebRTC session that triggered it.
    // The SDP answer is built (with the then-current codec) BEFORE the pipeline starts, so the
    // live-view attempt that detects the mismatch still shows nothing — its track was already
    // negotiated for the wrong codec. The NEXT attempt negotiates correctly, and after the
    // write-back below every attempt from the next boot onward is right the first time. The
    // point is that the camera stops being permanently broken, not that attempt #1 recovers.
    //
    // Called from the GStreamer streaming thread (pad-added); does no teardown itself.
    void NoteStreamEncoding(const char * encodingName);

    // Installed by the bridge main so a correction is written back to cameras.json and
    // survives a reboot. Invoked from the watchdog thread with NO CameraDevice lock held;
    // must not call back into this CameraDevice. Optional — without it the camera still
    // self-heals, just once per boot.
    void SetCodecCorrectionHandler(std::function<void(const std::string &)> handler)
    {
        std::lock_guard<std::mutex> lk(mCodecMutex);
        mCodecFixHandler = std::move(handler);
    }

    void HandleSimulatedZoneTriggeredEvent(const std::vector<uint16_t> & zoneIds);

    void HandleSimulatedZoneStoppedEvent(uint16_t zoneId);

    // Called from the appsink streaming thread for each video buffer. Feeds the RTSP no-data
    // watchdog AND computes the frame's wall-clock timestamp, both under mWatchdogMutex (F1):
    // the PTS-offset map is erased by the watchdog thread on pipeline restart, so streaming-
    // thread and watchdog-thread accesses must share the lock. Returns true if the frame is
    // to be forwarded (timestamp monotonic), with outTs/outFirstPts set.
    bool HandleVideoBufferTimestamp(uint16_t videoStreamID, uint64_t rawPts, int64_t & outTs, int64_t & outFirstPts);
    void SetAudioBranchLinked(bool linked);

    // Kick one background snapshot refresh (detached worker thread, TTL-gated, never blocks).
    // Called by CaptureSnapshot on every request (onvifOnly=false → full snapshot tier cascade)
    // and once from Init() at endpoint creation (onvifOnly=true → ONLY the lightweight ONVIF
    // curl, never the RTSP-decode tier) so the FIRST CaptureSnapshot after onboarding finds a
    // warm cache instead of failing while the initial ONVIF fetch is still in flight.
    void TriggerSnapshotRefresh(bool onvifOnly = false);

    // Audio playback pipeline methods
    CameraError StartAudioPlaybackStream();
    CameraError StopAudioPlaybackStream();

    // Timestamp handling for video and audio streams.
    // mVideoStreamPtsOffsetMs is read/written by the streaming-thread appsink callback AND
    // erased by the watchdog's TearDownVideoPipeline on restart, so all accesses to it are
    // guarded by mWatchdogMutex (the callback already takes that mutex each buffer). Without
    // the guard, a watchdog restart of stream 1 racing stream 3's callback corrupts the map.
    std::map<uint16_t, int64_t> mVideoStreamPtsOffsetMs;
    std::map<uint16_t, int64_t> mAudioStreamPtsOffsetMs;

    // On-demand pipeline: number of active consumers (live viewers + recorder) per
    // video stream ID. The GStreamer pipeline is built on 0->1 and torn down on ->0.
    std::map<uint16_t, int> mVideoStreamConsumers;

    // Count of live video pipelines currently running. Read from the Matter thread
    // (CaptureSnapshot) to avoid opening a competing snapshot RTSP session while a viewer is
    // streaming (cheap cameras allow very few concurrent RTSP sessions); atomic so that read
    // doesn't race the media-thread mutations of mVideoStreamConsumers.
    std::atomic<int> mActiveVideoStreams{ 0 };

    // Latest H.264 keyframe (Annex-B, SPS/PPS inline) seen on the live pipeline, plus when it was
    // captured. Snapshots decode this instead of opening a second RTSP session while streaming, so
    // the thumbnail never starves live view; when idle and this is stale, a fresh RTSP grab refreshes it.
    std::mutex mKeyframeMutex;
    std::vector<uint8_t> mLastLiveKeyframe;
    std::chrono::steady_clock::time_point mLastLiveKeyframeTime;

private:
    OnvifConfig mOnvifConfig; // per-instance ONVIF config; set via SetOnvifConfig() before Init()

    int videoDeviceFd            = -1;
    std::string mVideoDevicePath = kDefaultVideoDevicePath;
    std::vector<VideoStream> mVideoStreams;       // Vector to hold available video streams
    std::vector<AudioStream> mAudioStreams;       // Vector to hold available audio streams
    std::vector<SnapshotStream> mSnapshotStreams; // Vector to hold available snapshot streams

    void InitializeVideoStreams();
    void InitializeAudioStreams();
    void InitializeSnapshotStreams();

    bool AddSnapshotStream(const chip::app::Clusters::CameraAvStreamManagement::CameraAVStreamManager::SnapshotStreamAllocateArgs &
                               snapshotStreamAllocateArgs,
                           uint16_t & outStreamID);

    GstElement * CreateVideoPipeline(const std::string & device, int width, int height, int framerate, CameraError & error);
    GstElement * CreateAudioPipeline(const std::string & device, int channels, int sampleRate, int bitRate, CameraError & error);
    GstElement * CreateAudioPlaybackPipeline(CameraError & error);

    GstElement * CreateSnapshotPipeline(const std::string & device, int width, int height, int quality, int frameRate,
                                        const std::string & filename, CameraError & error);
    CameraError SetV4l2Control(uint32_t controlId, int value);

    // Per-camera cached snapshot JPEG path (tmpfs, keyed on the RTSP URL).
    std::string SnapshotCachePath() const;

    // Pipeline lifecycle helpers shared by StartVideoStream/StopVideoStream and the RTSP
    // watchdog's in-place restart. Neither touches the consumer ref-counts — callers own
    // those. Both require mPipelineLifecycleMutex to be held.
    CameraError BuildAndStartVideoPipeline(VideoStream & stream);
    bool TearDownVideoPipeline(VideoStream & stream); // false = GStreamer state change failed

    // ---- RTSP no-data watchdog -------------------------------------------------------------
    // Field failure this recovers from: after a PushAV clip some cameras silently stall their
    // RTSP stream (TCP stays ESTABLISHED, zero packets from port 554); the pipeline object
    // stays "running" and, because the PushAV transport remains a registered consumer between
    // clips, the ref-count never hits 0 so the stalled pipeline was never rebuilt. The
    // watchdog thread (pure media layer — it never touches the CHIP stack) checks every
    // running video pipeline: if the bus reports ERROR/EOS, or no buffer has arrived for
    // kWatchdogNoDataSec (INCLUDING a pipeline that connects but never delivers a single
    // frame — the common post-recording stall), it tears the pipeline down and rebuilds it
    // in place under the live consumers. Consecutive failed rebuilds back off (doubling,
    // capped) but retry forever — the camera may come back at any time and there is no other
    // recovery path.
    //
    // Dormancy is driven by the SAME consumer refcount that keeps the pipeline alive
    // (mVideoStreamConsumers), NOT by mActiveVideoStreams: the PushAV standing consumer must
    // keep the watchdog awake, and mActiveVideoStreams is used elsewhere (snapshot contention)
    // — the two must not be conflated or a PushAV-only stall goes unwatched.
    void StartWatchdog(); // idempotent; spawns the thread on first pipeline start and wakes it
    void StopWatchdog();  // idempotent; joins the thread (called from Shutdown + destructor)
    void WatchdogLoop();
    bool WatchdogCheckStreams(); // returns true while any video stream still has a consumer
    // Note a video buffer arrival for the watchdog. Assumes mWatchdogMutex is held (the
    // appsink callback holds it across the buffer's watchdog + PTS-offset bookkeeping).
    void NoteVideoDataLocked(uint16_t videoStreamID);

    static constexpr int kWatchdogPollSec           = 1;  // tick interval while pipelines run
    static constexpr int kWatchdogNoDataSec         = 10; // no buffers for this long => restart
    static constexpr int kWatchdogInitialBackoffSec = 5;
    static constexpr int kWatchdogMaxBackoffSec     = 60;

    // Serialises pipeline lifecycle (build/start/stop/rebuild). Start/Stop were historically
    // invoked from Matter-thread contexts and from the media controller (WebRTC callback
    // thread) without a CameraDevice-level lock; the watchdog thread adds a third mutator, so
    // every lifecycle path now takes this mutex. Never held while calling into the CHIP stack.
    std::mutex mPipelineLifecycleMutex;

    struct WatchdogStreamState
    {
        std::chrono::steady_clock::time_point lastData;    // last buffer arrival (or pipeline (re)start)
        std::chrono::steady_clock::time_point nextRestart; // backoff gate for the next rebuild
        std::chrono::steady_clock::time_point pipelineStart; // time when pipeline was (re)built
        int backoffSec = 0;                                // 0 = healthy (reset when data flows)
    };
    // Guards mWatchdogStreams AND mVideoStreamPtsOffsetMs (appsink streaming thread vs watchdog
    // thread). Lock order: mPipelineLifecycleMutex -> mWatchdogMutex (never the reverse).
    std::mutex mWatchdogMutex;
    std::map<uint16_t, WatchdogStreamState> mWatchdogStreams;

    // Guards mOnvifConfig.videoCodec (written by the watchdog thread, read by the WebRTC
    // thread via GetVideoCodec and by the pipeline builder) and the correction bookkeeping
    // below. Deliberately its OWN mutex, not mWatchdogMutex: nothing else is ever locked
    // while this is held, so it cannot participate in a lock cycle. Order when nested:
    // mPipelineLifecycleMutex -> mCodecMutex.
    std::mutex mCodecMutex;
    std::string mPendingCodecFix;                 // "H264"/"H265" queued by NoteStreamEncoding
    int mCodecFixCount                            = 0;
    static constexpr int kMaxCodecCorrections     = 3;
    std::function<void(const std::string &)> mCodecFixHandler; // persist hook (may be null)

    // Consume a queued correction: applies it to mOnvifConfig.videoCodec and returns the new
    // codec, or "" when there is nothing to do. Must be called with mPipelineLifecycleMutex
    // held (the caller rebuilds the pipeline off the back of it).
    std::string TakePendingCodecFix();

    std::thread mWatchdogThread;
    std::mutex mWatchdogCvMutex;
    std::condition_variable mWatchdogCv;
    bool mWatchdogStop = false; // guarded by mWatchdogCvMutex
    bool mWatchdogWake = false; // guarded by mWatchdogCvMutex; defeats the lost-wakeup race

    bool MatchClosestSnapshotParams(const VideoResolutionStruct & requested, VideoResolutionStruct & outResolution,
                                    chip::app::Clusters::CameraAvStreamManagement::ImageCodecEnum & outCodec);

    // Various cluster server delegates
    chip::app::Clusters::ChimeManager mChimeManager;
    chip::app::Clusters::WebRTCTransportProvider::WebRTCProviderManager mWebRTCProviderManager;
    chip::app::Clusters::CameraAvStreamManagement::CameraAVStreamManager mCameraAVStreamManager;
    chip::app::Clusters::PushAvStreamTransport::PushAvStreamTransportManager mPushAVTransportManager;
    chip::app::Clusters::CameraAvSettingsUserLevelManagement::CameraAVSettingsUserLevelManager mCameraAVSettingsUserLevelManager;
    chip::app::Clusters::ZoneManagement::ZoneManager mZoneManager;

    DefaultMediaController mMediaController;

    int16_t mPan  = chip::app::Clusters::CameraAvSettingsUserLevelManagement::kDefaultPan;
    int16_t mTilt = chip::app::Clusters::CameraAvSettingsUserLevelManagement::kDefaultTilt;
    uint8_t mZoom = chip::app::Clusters::CameraAvSettingsUserLevelManagement::kDefaultZoom;
    // Use a standard 1080p aspect ratio
    chip::app::Clusters::Globals::Structs::ViewportStruct::Type mViewport = { 0, 0, 1920, 1080 };
    uint16_t mCurrentVideoFrameRate                                       = k30fpsVideoFrameRate;
    bool mHDREnabled                                                      = false;
    bool mSpeakerMuted                                                    = false;
    bool mMicrophoneMuted                                                 = false;
    bool mHardPrivacyModeOn                                               = false;
    chip::app::Clusters::CameraAvStreamManagement::TriStateAutoEnum mNightVision =
        chip::app::Clusters::CameraAvStreamManagement::TriStateAutoEnum::kOff;
    bool mSoftRecordingPrivacyModeEnabled  = false;
    bool mSoftLivestreamPrivacyModeEnabled = false;
    uint8_t mSpeakerVol                    = kSpeakerMinLevel;
    uint8_t mSpeakerMinLevel               = kSpeakerMinLevel;
    uint8_t mSpeakerMaxLevel               = kSpeakerMaxLevel;
    uint8_t mMicrophoneVol                 = kMicrophoneMinLevel;
    uint8_t mMicrophoneMinLevel            = kMicrophoneMinLevel;
    uint8_t mMicrophoneMaxLevel            = kMicrophoneMaxLevel;
    bool mLocalVideoRecordingEnabled       = false;
    bool mLocalSnapshotRecordingEnabled    = false;
    bool mStatusLightEnabled               = false;
    uint16_t mImageRotation                = kMinImageRotation;
    bool mImageFlipHorizontal              = false;
    bool mImageFlipVertical                = false;
    uint8_t mDetectionSensitivity          = (1 + kSensitivityMax) / 2; // Average over the range

    std::vector<StreamUsageEnum> mStreamUsagePriorities = { StreamUsageEnum::kLiveView, StreamUsageEnum::kRecording };

    // Audio playback pipeline specific members
    GstElement * mAudioPlaybackPipeline = nullptr;

    // Per-stream audio delivery state (part of the shared RTSP pipeline)
    bool mAudioBranchLinked = false;   // true when rtspsrc exposed a supported audio pad
    bool mAudioDeliveryEnabled = false; // true when StartAudioStream has been called
    bool mAudioPadTimeoutLogged = false; // true when pad timeout has been logged for current run
    uint64_t mAudioEpoch = 0;          // shared timestamp epoch
    uint64_t mAudioPtsOffset = 0;      // audio PTS running offset on restart
};

} // namespace Camera
