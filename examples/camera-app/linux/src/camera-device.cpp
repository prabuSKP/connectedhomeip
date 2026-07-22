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

#include "camera-device.h"
#include "onvif/ptz_bridge.h" // ONVIF bridge: drive real-camera PTZ from the Matter HAL
#include <AppMain.h>
#include <Options.h>
#include <chrono>
#include <cstring> // memcpy/memset for the padded JPEG decode buffer
#include <fcntl.h> // For file descriptor operations
#include <filesystem>
#include <fstream>
#include <functional> // std::hash for the per-camera snapshot path
#include <mutex>      // background snapshot refresh guard
#include <set>
#include <thread>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/rtsp/rtsp.h> // GstRTSPMessage header edit for the Hikvision Basic-auth override
#include <iostream>
#include <lib/support/logging/CHIPLogging.h>
#include <limits.h>          // For PATH_MAX
#include <linux/videodev2.h> // For V4L2 definitions
#include <sys/ioctl.h>

// On-demand snapshot/thumbnail: ONVIF snapshot URI (curl) first, else decode one RTSP
// keyframe (libav h264 decoder) and MJPEG-encode it.
#include <curl/curl.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

// File used to store snapshot from stream and return for CaptureSnapshot
// command.
#define SNAPSHOT_FILE_PATH "./capture_snapshot.jpg"
// Timeout (seconds) to wait for the video pipeline to reach PLAYING. Kept at 0 so
// StartVideoStream returns immediately: a live rtspsrc reaches PLAYING
// asynchronously anyway, and the start is now triggered from the WebRTC callback
// thread (media controller, on first viewer) which must not block.
#define VIDEO_PIPELINE_PLAY_TIMEOUT 0
// Framesize for audio pipeline
#define AUDIO_FRAMESIZE 20

using namespace chip::app::Clusters;
using namespace chip::app::Clusters::Chime;
using namespace chip::app::Clusters::CameraAvStreamManagement;
using namespace chip::app::Clusters::CameraAvSettingsUserLevelManagement;
using namespace chip::app::Clusters::WebRTCTransportProvider;
using namespace chip::app::Clusters::ZoneManagement;

using namespace Camera;

namespace {

// Context structure to pass both CameraDevice and videoStreamID to the callback
struct VideoAppSinkContext
{
    CameraDevice * device;
    uint16_t videoStreamID;
};

// Context structure to pass both CameraDevice and audioStreamID to the callback
struct AudioAppSinkContext
{
    CameraDevice * device;
    uint16_t audioStreamID;
};

// Using Gstreamer video test source's ball animation pattern for the live streaming visual verification.
// Refer https://gstreamer.freedesktop.org/documentation/videotestsrc/index.html?gi-language=c#GstVideoTestSrcPattern

// Callback function for GStreamer app sink
GstFlowReturn OnNewVideoSampleFromAppSink(GstAppSink * appsink, gpointer user_data)
{
    VideoAppSinkContext * context = static_cast<VideoAppSinkContext *>(user_data);
    CameraDevice * self           = context->device;
    uint16_t videoStreamID        = context->videoStreamID;

    GstSample * sample = gst_app_sink_pull_sample(appsink);
    if (sample == nullptr)
    {
        return GST_FLOW_ERROR;
    }

    GstBuffer * buffer = gst_sample_get_buffer(sample);
    if (buffer == nullptr)
    {
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ))
    {
        GstClockTime rawPts = GST_BUFFER_PTS(buffer);
        if (rawPts == GST_CLOCK_TIME_NONE)
        {
            rawPts = GST_BUFFER_DTS(buffer);
            if (rawPts == GST_CLOCK_TIME_NONE)
            {
                rawPts = 0;
            }
        }

        // Watchdog bookkeeping + PTS-offset lookup share mWatchdogMutex (F1): the PTS map is
        // erased by the watchdog thread's TearDownVideoPipeline on restart, so it must not be
        // touched here without the lock. HandleVideoBufferTimestamp does both under that lock;
        // DistributeVideo is done AFTER it returns (it is heavy and must not stall the
        // watchdog thread).
        int64_t ts         = 0;
        int64_t firstPts   = 0;
        bool forwardFrame  = self->HandleVideoBufferTimestamp(videoStreamID, rawPts, ts, firstPts);

        if (forwardFrame)
        {
            // Forward raw H.264 encoded frames to media controller with timestamp
            // The PreRollBuffer will distribute to ALL transports registered for this videoStreamID
            // Each transport will handle its own SFrame encryption (if configured) during RTP packetization
            self->GetMediaController().DistributeVideo(reinterpret_cast<const uint8_t *>(map.data), map.size, videoStreamID, ts);
        }
        else
        {
            ChipLogError(Camera,
                         "Dropping video frame with PTS %" G_GUINT64_FORMAT " <= first PTS %" G_GINT64_FORMAT " for stream %u",
                         rawPts, firstPts, videoStreamID);
        }

        // Cache the latest keyframe (h264parse config-interval=-1 keeps SPS/PPS inline, so it is
        // decodable standalone) so CaptureSnapshot can produce a thumbnail by decoding it instead of
        // opening a SECOND RTSP session — this cheap camera allows very few concurrent sessions, and a
        // competing snapshot RTSP was starving live view.
        if (!GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT))
        {
            std::lock_guard<std::mutex> lk(self->mKeyframeMutex);
            self->mLastLiveKeyframe.assign(map.data, map.data + map.size);
            self->mLastLiveKeyframeTime = std::chrono::steady_clock::now();
        }

        gst_buffer_unmap(buffer, &map);
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

// Cleanup function for the context
void DestroyVideoAppSinkContext(gpointer user_data)
{
    VideoAppSinkContext * context = static_cast<VideoAppSinkContext *>(user_data);
    delete context;
}
static GstFlowReturn OnNewAudioSampleFromAppSink(GstAppSink * appsink, gpointer user_data)
{
    auto * context     = static_cast<AudioAppSinkContext *>(user_data);
    auto * self        = context->device;
    auto audioStreamID = context->audioStreamID;

    GstSample * sample = gst_app_sink_pull_sample(appsink);
    if (!sample)
        return GST_FLOW_ERROR;

    GstBuffer * buffer = gst_sample_get_buffer(sample);
    if (!buffer)
    {
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    // opusenc sends codec headers at start; ignore them
    if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_HEADER))
    {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ))
    {
        GstClockTime rawPts = GST_BUFFER_PTS(buffer);
        if (rawPts == GST_CLOCK_TIME_NONE)
        {
            rawPts = GST_BUFFER_DTS(buffer);
            if (rawPts == GST_CLOCK_TIME_NONE)
            {
                rawPts = 0;
            }
        }
        auto firstPtsIt = self->mAudioStreamPtsOffsetMs.find(audioStreamID);
        if (firstPtsIt == self->mAudioStreamPtsOffsetMs.end())
        {
            auto now                                     = std::chrono::steady_clock::now().time_since_epoch();
            int64_t nowMs                                = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
            int64_t rawMs                                = static_cast<int64_t>(rawPts / 1000000);
            self->mAudioStreamPtsOffsetMs[audioStreamID] = nowMs - rawMs;
        }
        int64_t ts = self->mAudioStreamPtsOffsetMs[audioStreamID] + (rawPts / 1000000);
        if (ts >= self->mAudioStreamPtsOffsetMs[audioStreamID])
        {
            // Forward raw Opus encoded frames to media controller with timestamp
            // The PreRollBuffer will distribute to ALL transports registered for this audioStreamID
            // Each transport will handle its own SFrame encryption (if configured) during RTP packetization
            self->GetMediaController().DistributeAudio(reinterpret_cast<const uint8_t *>(map.data), map.size, audioStreamID, ts);
        }
        else
        {
            ChipLogError(Camera,
                         "Dropping audio frame with PTS %" G_GUINT64_FORMAT " <= first PTS %" G_GUINT64_FORMAT " for stream %u",
                         rawPts, self->mAudioStreamPtsOffsetMs[audioStreamID], audioStreamID);
        }
        gst_buffer_unmap(buffer, &map);
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

static void DestroyAudioAppSinkContext(gpointer user_data)
{
    delete static_cast<AudioAppSinkContext *>(user_data);
}

} // namespace

namespace GstreamerPipepline {

enum class CameraType
{
    kCsi,
    kUsb,
    kFailure,
};

CameraType detectCameraType(const std::string & fullDevicePath)
{
    if (!std::filesystem::exists(fullDevicePath))
    {
        return CameraType::kFailure;
    }

    std::string videoDeviceName = std::filesystem::path(fullDevicePath).filename(); // ex: video0
    std::string sysPath         = "/sys/class/video4linux/" + videoDeviceName + "/device/driver/module";

    char resolvedPath[PATH_MAX];
    ssize_t len = readlink(sysPath.c_str(), resolvedPath, sizeof(resolvedPath) - 1);

    // Define driver name constants to avoid magic strings
    constexpr const char * kCsiDriver1 = "bm2835";
    constexpr const char * kCsiDriver2 = "unicam";
    constexpr const char * kUsbDriver  = "uvc";

    VerifyOrReturnError(len != -1, CameraType::kFailure);

    const std::string driverPath(resolvedPath, static_cast<size_t>(len));

    if (driverPath.find(kCsiDriver1) != std::string::npos || driverPath.find(kCsiDriver2) != std::string::npos)
    {
        return CameraType::kCsi;
    }
    if (driverPath.find(kUsbDriver) != std::string::npos)
    {
        return CameraType::kUsb;
    }

    return CameraType::kFailure;
}

// Function to unreference GStreamer elements and the pipeline
template <typename... Args>
void unrefGstElements(GstElement * pipeline, Args... elements)
{
    if (pipeline)
    {
        gst_object_unref(pipeline);
    }

    // Unreference each element in the variadic template argument pack
    ((elements ? gst_object_unref(elements) : void()), ...);
}

bool isGstElementsNull(const std::vector<std::pair<GstElement *, const char *>> & elements)
{
    bool isNull = false;

    // Check if any of the elements in the vector is nullptr
    for (const auto & element : elements)
    {
        if (!element.first)
        {
            ChipLogError(Camera, "Element '%s' could not be created.", element.second);
            isNull = true;
        }
    }

    return isNull;
}

namespace Snapshot {
struct SnapshotPipelineConfig
{
    std::string device;
    int width;
    int height;
    int quality;
    int framerate;
    std::string filename;
};

GstElement * CreateSnapshotPipelineV4l2(const SnapshotPipelineConfig & config, CameraError & error)
{
    // Create the GStreamer elements for the snapshot pipeline
    GstElement * pipeline = gst_pipeline_new("snapshot-pipeline");
    // TODO: Have the video source passed in.
    GstElement * source         = gst_element_factory_make("v4l2src", "source");
    GstElement * jpeg_caps      = gst_element_factory_make("capsfilter", "jpeg_caps");
    GstElement * videorate      = gst_element_factory_make("videorate", "videorate");
    GstElement * videorate_caps = gst_element_factory_make("capsfilter", "timelapse_framerate");
    GstElement * queue          = gst_element_factory_make("queue", "queue");
    GstElement * filesink       = gst_element_factory_make("multifilesink", "sink");

    // Check for any nullptr among the created elements
    const std::vector<std::pair<GstElement *, const char *>> elements = {
        { pipeline, "pipeline" },             //
        { source, "source" },                 //
        { jpeg_caps, "jpeg_caps" },           //
        { videorate, "videorate" },           //
        { videorate_caps, "videorate_caps" }, //
        { queue, "queue" },                   //
        { filesink, "filesink" }              //
    };
    bool isElementFactoryMakeFailed = GstreamerPipepline::isGstElementsNull(elements);

    // If any element creation failed, log the error and unreference the elements
    if (isElementFactoryMakeFailed)
    {
        // Unreference the elements that were created
        GstreamerPipepline::unrefGstElements(pipeline, source, jpeg_caps, videorate, videorate_caps, queue, filesink);

        error = CameraError::ERROR_INIT_FAILED;
        return nullptr;
    }

    // Set the source device and caps
    g_object_set(source, "device", config.device.c_str(), "do-timestamp", TRUE, nullptr);

    GstCaps * caps = gst_caps_new_simple(                    //
        "image/jpeg",                                        //
        "width", G_TYPE_INT, config.width,                   //
        "height", G_TYPE_INT, config.height,                 //
        "framerate", GST_TYPE_FRACTION, config.framerate, 1, //
        "quality", G_TYPE_INT, config.quality,               //
        nullptr                                              //
    );
    g_object_set(jpeg_caps, "caps", caps, nullptr);
    gst_caps_unref(caps);

    // Set the output file location
    g_object_set(filesink, "location", config.filename.c_str(), nullptr);

    // Add elements to the pipeline
    gst_bin_add_many(GST_BIN(pipeline), source, jpeg_caps, videorate, videorate_caps, queue, filesink, nullptr);

    // Link the elements
    if (gst_element_link_many(source, jpeg_caps, videorate, videorate_caps, queue, filesink, nullptr) != TRUE)
    {
        ChipLogError(Camera, "Elements could not be linked.");

        // The pipeline will unref all added elements automatically when you unref the pipeline.
        gst_object_unref(pipeline);
        error = CameraError::ERROR_INIT_FAILED;
        return nullptr;
    }

    return pipeline;
}

GstElement * CreateSnapshotPipelineLibcamerasrc(const SnapshotPipelineConfig & config, CameraError & error)
{
    // Create the GStreamer elements for the snapshot pipeline
    GstElement * pipeline   = gst_pipeline_new("snapshot-pipeline");
    GstElement * source     = gst_element_factory_make("libcamerasrc", "source");
    GstElement * capsfilter = gst_element_factory_make("capsfilter", "capsfilter");
    GstElement * jpegenc    = gst_element_factory_make("jpegenc", "jpegenc");
    GstElement * queue      = gst_element_factory_make("queue", "queue");
    GstElement * filesink   = gst_element_factory_make("multifilesink", "sink");

    // Check for any nullptr among the created elements
    const std::vector<std::pair<GstElement *, const char *>> elements = {
        { pipeline, "pipeline" },     //
        { source, "source" },         //
        { capsfilter, "capsfilter" }, //
        { jpegenc, "jpegenc" },       //
        { queue, "queue" },           //
        { filesink, "filesink" }      //
    };
    const bool isElementFactoryMakeFailed = GstreamerPipepline::isGstElementsNull(elements);

    // If any element creation failed, log the error and unreference the elements
    if (isElementFactoryMakeFailed)
    {
        // Unreference the elements that were created
        GstreamerPipepline::unrefGstElements(pipeline, source, capsfilter, jpegenc, filesink);
        error = CameraError::ERROR_INIT_FAILED;
        return nullptr;
    }

    // Set resolution and framerate caps
    GstCaps * caps = gst_caps_new_simple(                    //
        "video/x-raw",                                       //
        "width", G_TYPE_INT, config.width,                   //
        "height", G_TYPE_INT, config.height,                 //
        "framerate", GST_TYPE_FRACTION, config.framerate, 1, //
        nullptr                                              //
    );
    g_object_set(capsfilter, "caps", caps, nullptr);
    gst_caps_unref(caps);

    // Set JPEG quality
    g_object_set(jpegenc, "quality", config.quality, nullptr);

    // Set multifilesink to write only one file
    g_object_set(filesink, "location", config.filename.c_str(), nullptr);

    // Add and link elements
    gst_bin_add_many(GST_BIN(pipeline), source, capsfilter, jpegenc, queue, filesink, nullptr);
    if (!gst_element_link_many(source, capsfilter, jpegenc, queue, filesink, nullptr))
    {
        ChipLogError(Camera, "Elements could not be linked.");
        gst_object_unref(pipeline);
        error = CameraError::ERROR_INIT_FAILED;
        return nullptr;
    }

    return pipeline;
}
} // namespace Snapshot

} // namespace GstreamerPipepline

CameraDevice::CameraDevice()
{
    // Set the CameraHALInterface in CameraAVStreamManager and CameraAVsettingsUserLevelManager.
    mCameraAVStreamManager.SetCameraDeviceHAL(this);
    mCameraAVSettingsUserLevelManager.SetCameraDeviceHAL(this);

    // Provider manager uses the Media controller to register WebRTC Transport with media controller for AV source data
    mWebRTCProviderManager.SetMediaController(&mMediaController);

    mPushAVTransportManager.SetMediaController(&mMediaController);

    // Set the CameraDevice interface in WebRTCManager
    mWebRTCProviderManager.SetCameraDevice(this);

    // Set the CameraDevice interface in ZoneManager
    mZoneManager.SetCameraDevice(this);
    mPushAVTransportManager.SetCameraDevice(this);
    mMediaController.SetCameraDevice(this);
}

CameraDevice::~CameraDevice()
{
    // No-op if Shutdown() already ran: make sure the watchdog thread is joined before any
    // member it touches (pipelines, media controller) is destroyed.
    StopWatchdog();

    // Tear down the PushAV transports while every member is still alive. Members destruct in
    // reverse declaration order, so mMediaController (declared after the managers) is destroyed
    // FIRST — if the manager's destructor were left to do this unregister loop itself, the
    // DefaultMediaController vtable would already have rolled back to the abstract base and
    // UnregisterTransport() would be a pure-virtual call -> abort (crashed camera removes on
    // hardware once bridged-camera PushAV recording started populating the transport map).
    mPushAVTransportManager.Shutdown();

    if (videoDeviceFd != -1)
    {
        close(videoDeviceFd);
    }
}

void CameraDevice::Init()
{
    InitializeCameraDevice();
    InitializeStreams();
    mWebRTCProviderManager.Init();
    mPushAVTransportManager.Init();

    // Warm the snapshot cache at endpoint creation. Both onboarding paths funnel through here
    // (boot-time cameras.json load and runtime IPC AddCamera both call BridgedCamera::
    // InitClusters -> Init(), the single-camera CLI path calls Init() from main.cpp), so the
    // FIRST CaptureSnapshot no longer fails with "not ready yet" while the initial ONVIF fetch
    // is still in flight. onvifOnly=true keeps this to just the lightweight ONVIF curl — no
    // RTSP session or H.264 decode at boot (F4). Detached/non-blocking, so it never delays
    // endpoint bring-up, and harmless when the camera is unreachable (curl timeout is bounded).
    // Only worth doing when the camera actually has an ONVIF snapshot URI; RTSP-only cameras
    // warm lazily on the first CaptureSnapshot.
    if (!mOnvifConfig.snapshotUrl.empty())
    {
        TriggerSnapshotRefresh(/*onvifOnly=*/true);
    }
}

void CameraDevice::Shutdown()
{
    // Stop the RTSP watchdog first so it cannot rebuild a pipeline under the teardown below.
    StopWatchdog();

    // Close WebRTC connections while the SystemLayer is still alive, so that WebRTC callbacks can safely use ScheduleLambda.
    mWebRTCProviderManager.CloseConnection();

    // Stop Video and Audio Streams so their threads don't access CameraDevice members (like mAudioStreamPtsOffsetMs) after
    // destruction.
    for (auto & stream : mVideoStreams)
    {
        StopVideoStream(stream.videoStreamParams.videoStreamID);
    }
    for (auto & stream : mAudioStreams)
    {
        StopAudioStream(stream.audioStreamParams.audioStreamID);
    }
}

CameraError CameraDevice::InitializeCameraDevice()
{
    static bool gstreamerInitialized = false;

    if (!gstreamerInitialized)
    {
        gst_init(nullptr, nullptr);
        gstreamerInitialized = true;
    }

    ChipLogDetail(Camera, "InitializeCameraDevice: %s", mVideoDevicePath.c_str());

    videoDeviceFd = open(mVideoDevicePath.c_str(), O_RDWR);
    if (videoDeviceFd == -1)
    {
        ChipLogError(Camera, "Error opening video device: %s at %s", strerror(errno), mVideoDevicePath.c_str());
        return CameraError::ERROR_INIT_FAILED;
    }

    return CameraError::SUCCESS;
}

CameraError CameraDevice::InitializeStreams()
{
    InitializeVideoStreams();
    InitializeAudioStreams();
    InitializeSnapshotStreams();

    return CameraError::SUCCESS;
}

// Function to create the GStreamer pipeline
GstElement * CameraDevice::CreateSnapshotPipeline(const std::string & device, int width, int height, int quality, int framerate,
                                                  const std::string & filename, CameraError & error)
{
    const auto cameraType = GstreamerPipepline::detectCameraType(device);

    const GstreamerPipepline::Snapshot::SnapshotPipelineConfig config = {
        .device    = device,
        .width     = width,
        .height    = height,
        .quality   = quality,
        .framerate = framerate,
        .filename  = filename,
    };

    switch (cameraType)
    {
    case GstreamerPipepline::CameraType::kCsi: {
        ChipLogDetail(Camera, "Detected CSI camera: %s", device.c_str());
        return GstreamerPipepline::Snapshot::CreateSnapshotPipelineLibcamerasrc(config, error);
    }
    break;
    case GstreamerPipepline::CameraType::kUsb: {
        ChipLogDetail(Camera, "Detected USB camera: %s", device.c_str());
        return GstreamerPipepline::Snapshot::CreateSnapshotPipelineV4l2(config, error);
    }
    break;
    case GstreamerPipepline::CameraType::kFailure: {
        ChipLogError(Camera, "Unsupported camera type or device not found: %s", device.c_str());
        error = CameraError::ERROR_INIT_FAILED;
        return nullptr;
    }
    }

    return nullptr; // Here to avoid compiler warnings, should never reach this point.
}

struct OnvifPadLinkContext {
    GstElement * videoDepay = nullptr;
    GstElement * audioDepay = nullptr;
    CameraDevice * device   = nullptr;
};

// [hikvision] Some Hikvision cameras (verified: DS-2CD122P-I3) have a broken RTSP
// Digest implementation — they reject the CORRECT password over Digest but accept
// the SAME credentials over Basic. Our onvif_rtsp_probe already detects this and
// falls back to Basic (setting OnvifConfig::needsBasicAuth at onboarding), but
// GStreamer's rtspsrc has no such fallback: it tries Digest, gets 401, and gives up
// with "Unauthorized" even though the password is right. For a flagged camera we hook
// rtspsrc's "before-send" signal and force a Basic Authorization header on every
// outgoing RTSP request, so the broken Digest exchange is bypassed entirely. The
// signal hands us the request message with G_SIGNAL_TYPE_STATIC_SCOPE (modifiable in
// place); returning TRUE sends it (FALSE would drop it).
struct RtspForceBasicContext {
    std::string authHeader; // full field value: "Basic <base64(user:pass)>"
};

static gboolean OnvifForceBasicAuth(GstElement * /*src*/, GstRTSPMessage * msg, gpointer user_data)
{
    auto * ctx = static_cast<RtspForceBasicContext *>(user_data);
    if (ctx && msg && gst_rtsp_message_get_type(msg) == GST_RTSP_MESSAGE_REQUEST)
    {
        // Drop any Authorization header rtspsrc may have set (e.g. a Digest response
        // to a 401) and replace it with our precomputed Basic header. indx -1 removes
        // every existing instance; a no-op if none are present.
        gst_rtsp_message_remove_header(msg, GST_RTSP_HDR_AUTHORIZATION, -1);
        gst_rtsp_message_add_header(msg, GST_RTSP_HDR_AUTHORIZATION, ctx->authHeader.c_str());
    }
    return TRUE; // always send the request
}

// ONVIF bridge: rtspsrc exposes its stream pads dynamically once the RTSP session is
// negotiated, so rtspsrc → depay is linked at "pad-added" time.
static void OnvifLinkRtspPadAV(GstElement * src, GstPad * newPad, gpointer user_data)
{
    OnvifPadLinkContext * ctx = static_cast<OnvifPadLinkContext *>(user_data);
    GstCaps * caps            = gst_pad_get_current_caps(newPad);
    const GstStructure * st   = caps ? gst_caps_get_structure(caps, 0) : nullptr;
    const gchar * media       = st ? gst_structure_get_string(st, "media") : nullptr;
    const gchar * encoding    = st ? gst_structure_get_string(st, "encoding-name") : nullptr;

    if (media && g_strcmp0(media, "video") == 0)
    {
        // Link the pad to whatever depayloader the pipeline was built with (rtph264depay or
        // rtph265depay, chosen from the persisted codec). gst_pad_link's caps check succeeds
        // when the stream encoding matches the depayloader and fails otherwise — either way we
        // log the ACTUAL encoding, so an unexpected codec is never silently dropped (the old
        // code hard-matched "H264" and skipped H.265 with no log at all).
        if (ctx->videoDepay)
        {
            GstPad * sinkPad = gst_element_get_static_pad(ctx->videoDepay, "sink");
            if (sinkPad && !gst_pad_is_linked(sinkPad))
            {
                if (gst_pad_link(newPad, sinkPad) == GST_PAD_LINK_OK)
                {
                    ChipLogProgress(Camera, "ONVIF: linked rtspsrc video pad (encoding=%s) → depayloader",
                                    encoding ? encoding : "unknown");
                }
                else
                {
                    ChipLogError(Camera,
                                 "ONVIF: could not link video pad — stream encoding=%s does not match the "
                                 "configured depayloader. The camera's real codec differs from what was detected; "
                                 "re-onboard the camera to re-probe, or set its encoding to H.264/H.265.",
                                 encoding ? encoding : "unknown");
                }
            }
            if (sinkPad)
                gst_object_unref(sinkPad);
        }
    }
    else if (media && g_strcmp0(media, "audio") == 0)
    {
        if (ctx->audioDepay)
        {
            GstPad * sinkPad = gst_element_get_static_pad(ctx->audioDepay, "sink");
            if (sinkPad && !gst_pad_is_linked(sinkPad))
            {
                if (gst_pad_link(newPad, sinkPad) == GST_PAD_LINK_OK)
                {
                    ctx->device->SetAudioBranchLinked(true);
                    ChipLogProgress(Camera, "CAM_AUDIO branch-linked media=audio codec=%s", encoding ? encoding : "unknown");
                }
                else
                {
                    ChipLogError(Camera, "ONVIF: failed to link audio pad");
                }
            }
            if (sinkPad)
                gst_object_unref(sinkPad);
        }
    }

    if (caps)
    {
        gst_caps_unref(caps);
    }
}

// Helper function to create a GStreamer pipeline that captures raw video frames from
// the camera, converts them to I420 format, encodes to H.264, and sends the encoded
// stream to the media controller via app sink.
GstElement * CameraDevice::CreateVideoPipeline(const std::string & device, int width, int height, int framerate,
                                               CameraError & error)
{
    // ONVIF bridge passthrough path: the camera already emits H.264 over RTSP, so forward
    // its access units straight to the appsink (rtspsrc → rtph264depay → h264parse → appsink)
    // with no decode/re-encode. width/height/framerate are dictated by the camera here.
    if (!mOnvifConfig.rtspUrl.empty())
    {
        const std::string onvifUrl = mOnvifConfig.rtspUrl;
        // Codec is detected from the real RTSP SDP at onboard and persisted (mOnvifConfig.videoCodec).
        // H.265 is the SAME zero-transcode passthrough as H.264 — only the RTP depayloader and the
        // parser differ (rtph265depay/h265parse vs rtph264depay/h264parse); the compressed access
        // units flow straight to the appsink untouched.
        const bool   isH265        = (mOnvifConfig.videoCodec == "H265");
        GstElement * pipeline      = gst_pipeline_new("video-pipeline");
        GstElement * source        = gst_element_factory_make("rtspsrc", "source");
        GstElement * depay         = gst_element_factory_make(isH265 ? "rtph265depay" : "rtph264depay", "depay");
        GstElement * parse         = gst_element_factory_make(isH265 ? "h265parse" : "h264parse", "parse");
        GstElement * h264caps      = gst_element_factory_make("capsfilter", "h264caps");
        GstElement * appsink       = gst_element_factory_make("appsink", "appsink");

        const std::vector<std::pair<GstElement *, const char *>> onvifElements = {
            { pipeline, "pipeline" }, { source, "source" }, { depay, "depay" },
            { parse, "parse" },       { h264caps, "h264caps" }, { appsink, "appsink" },
        };
        if (GstreamerPipepline::isGstElementsNull(onvifElements))
        {
            // Name the codec: for H.265 the usual cause is a hub GStreamer lacking rtph265depay
            // (gst-plugins-good) or h265parse (gst-plugins-bad). Without those the H.265 pipeline
            // cannot be built and live view is impossible until the plugins are deployed.
            ChipLogError(Camera, "ONVIF: not all %s video elements could be created (depay=%p parse=%p) — %s",
                         isH265 ? "H.265" : "H.264", (void *) depay, (void *) parse,
                         isH265 ? "hub may be missing rtph265depay/h265parse plugins" : "check GStreamer install");
            GstreamerPipepline::unrefGstElements(pipeline, source, depay, parse, h264caps, appsink);
            error = CameraError::ERROR_INIT_FAILED;
            return nullptr;
        }

        // rtspsrc: cap the jitterbuffer latency; let it negotiate transport (UDP→TCP fallback).
        g_object_set(source, "location", onvifUrl.c_str(), "latency", 200, nullptr);
        // Credentials go to rtspsrc as element properties (user-id/user-pw)
        if (!mOnvifConfig.user.empty())
            g_object_set(source, "user-id", mOnvifConfig.user.c_str(), "user-pw", mOnvifConfig.pass.c_str(), nullptr);

        // [hikvision] For a camera whose Digest is broken (needsBasicAuth, set at
        // onboarding from the RTSP probe's Basic fallback), force Basic auth on every
        // request via the before-send hook — rtspsrc's own Digest attempt would 401.
        if (mOnvifConfig.needsBasicAuth && !mOnvifConfig.user.empty())
        {
            std::string creds = mOnvifConfig.user + ":" + mOnvifConfig.pass;
            gchar * b64       = g_base64_encode(reinterpret_cast<const guchar *>(creds.data()), creds.size());
            auto * authCtx    = new RtspForceBasicContext();
            authCtx->authHeader = std::string("Basic ") + (b64 ? b64 : "");
            g_free(b64);
            g_signal_connect_data(source, "before-send", G_CALLBACK(OnvifForceBasicAuth), authCtx,
                                  [](gpointer data, GClosure *) { delete static_cast<RtspForceBasicContext *>(data); },
                                  static_cast<GConnectFlags>(0));
            ChipLogProgress(Camera, "ONVIF: forcing Basic RTSP auth (Digest was rejected but Basic accepted at probe time)");
        }
        // h264parse/h265parse: emit byte-stream access units and repeat the parameter sets
        // (SPS/PPS, plus VPS for H.265) inline via config-interval=-1 — needed for keyframe
        // caching and for a decoder joining mid-stream.
        g_object_set(parse, "config-interval", -1, nullptr);
        GstCaps * outCaps = gst_caps_new_simple(isH265 ? "video/x-h265" : "video/x-h264",
                                                "stream-format", G_TYPE_STRING, "byte-stream",
                                                "alignment", G_TYPE_STRING, "au", nullptr);
        g_object_set(h264caps, "caps", outCaps, nullptr);
        gst_caps_unref(outCaps);
        g_object_set(appsink, "emit-signals", TRUE, nullptr);

        // Preflight check for audio plugins
        bool includeAudio = false;
        GstElement * audio_queue    = nullptr;
        GstElement * audio_depay    = nullptr;
        GstElement * audio_dec      = nullptr;
        GstElement * audio_convert  = nullptr;
        GstElement * audio_resample = nullptr;
        GstElement * audio_enc      = nullptr;
        GstElement * audio_sink     = nullptr;

        if (mOnvifConfig.HasVerifiedAudio())
        {
            const char * depay_plugin = (mOnvifConfig.audioCapability.codec == InboundAudioCodec::kPcmu) ? "rtppcmudepay" : "rtppcmadepay";
            const char * dec_plugin   = (mOnvifConfig.audioCapability.codec == InboundAudioCodec::kPcmu) ? "mulawdec" : "alawdec";
            const std::vector<const char *> requiredAudioPlugins = {
                "queue", depay_plugin, dec_plugin, "audioconvert", "audioresample", "opusenc", "appsink"
            };
            bool hasAllPlugins = true;
            for (const auto & plugin : requiredAudioPlugins) {
                if (!gst_element_factory_find(plugin)) {
                    ChipLogError(Camera, "CAM_AUDIO unavailable reason=missing_plugin missing=%s", plugin);
                    hasAllPlugins = false;
                }
            }

            if (hasAllPlugins)
            {
                audio_queue    = gst_element_factory_make("queue", "audio_queue");
                audio_depay    = gst_element_factory_make(depay_plugin, "audio_depay");
                audio_dec      = gst_element_factory_make(dec_plugin, "audio_dec");
                audio_convert  = gst_element_factory_make("audioconvert", "audio_convert");
                audio_resample = gst_element_factory_make("audioresample", "audio_resample");
                audio_enc      = gst_element_factory_make("opusenc", "audio_enc");
                audio_sink     = gst_element_factory_make("appsink", "audio_sink");

                if (audio_queue && audio_depay && audio_dec && audio_convert && audio_resample && audio_enc && audio_sink)
                {
                    includeAudio = true;
                }
                else
                {
                    ChipLogError(Camera, "ONVIF: failed to create all audio elements despite factory presence.");
                    GstreamerPipepline::unrefGstElements(audio_queue, audio_depay, audio_dec, audio_convert, audio_resample, audio_enc, audio_sink);
                    audio_queue = audio_depay = audio_dec = audio_convert = audio_resample = audio_enc = audio_sink = nullptr;
                }
            }
        }

        gst_bin_add_many(GST_BIN(pipeline), source, depay, parse, h264caps, appsink, nullptr);
        if (includeAudio)
        {
            g_object_set(audio_enc, "bitrate", 20000, "complexity", 3, nullptr);
            g_object_set(audio_sink, "emit-signals", TRUE, nullptr);

            AudioAppSinkContext * aCtx = new AudioAppSinkContext{ this, 1 }; // Audio streamID is 1
            GstAppSinkCallbacks aCallbacks = { nullptr, nullptr, OnNewAudioSampleFromAppSink };
            gst_app_sink_set_callbacks(GST_APP_SINK(audio_sink), &aCallbacks, aCtx, DestroyAudioAppSinkContext);

            gst_bin_add_many(GST_BIN(pipeline), audio_queue, audio_depay, audio_dec, audio_convert, audio_resample, audio_enc, audio_sink, nullptr);
        }

        // Static chain: rtph264depay → h264parse → capsfilter → appsink.
        if (!gst_element_link_many(depay, parse, h264caps, appsink, nullptr))
        {
            ChipLogError(Camera, "ONVIF: link depay → parse → appsink failed");
            gst_object_unref(pipeline);
            error = CameraError::ERROR_INIT_FAILED;
            return nullptr;
        }

        if (includeAudio)
        {
            if (!gst_element_link_many(audio_queue, audio_depay, audio_dec, audio_convert, audio_resample, audio_enc, audio_sink, nullptr))
            {
                ChipLogError(Camera, "ONVIF: link audio elements failed");
                gst_object_unref(pipeline);
                error = CameraError::ERROR_INIT_FAILED;
                return nullptr;
            }
        }

        // Dynamic link setup
        OnvifPadLinkContext * linkCtx = new OnvifPadLinkContext();
        linkCtx->videoDepay = depay;
        linkCtx->audioDepay = includeAudio ? audio_queue : nullptr;
        linkCtx->device     = this;

        g_signal_connect_data(source, "pad-added", G_CALLBACK(OnvifLinkRtspPadAV), linkCtx,
                              [](gpointer data, GClosure *) { delete static_cast<OnvifPadLinkContext *>(data); },
                              static_cast<GConnectFlags>(0));

        ChipLogProgress(Camera, "Video pipeline: ONVIF/RTSP source %s (%s passthrough%s)", onvifUrl.c_str(),
                        isH265 ? "H.265" : "H.264", includeAudio ? " + Audio PCMU/PCMA->Opus" : "");
        return pipeline;
    }

    GstElement * pipeline     = gst_pipeline_new("video-pipeline");
    GstElement * capsfilter1  = gst_element_factory_make("capsfilter", "filter1");
    GstElement * capsfilter2  = gst_element_factory_make("capsfilter", "filter2");
    GstElement * videoconvert = gst_element_factory_make("videoconvert", "videoconvert");
    GstElement * x264enc      = gst_element_factory_make("x264enc", "encoder");
    GstElement * appsink      = gst_element_factory_make("appsink", "appsink");
    GstElement * source       = nullptr;

    if (mOnvifConfig.useTestSrc || LinuxDeviceOptions::GetInstance().cameraTestVideosrc)
    {
        const int kBallAnimationPattern = 18;
        source                          = gst_element_factory_make("videotestsrc", "source");
        g_object_set(source, "pattern", kBallAnimationPattern, nullptr);
        ChipLogProgress(Camera, "Video piepline: using test video source");
    }
    else
    {
        source = gst_element_factory_make("v4l2src", "source");
        g_object_set(source, "device", device.c_str(), nullptr);
        ChipLogProgress(Camera, "Video pipeline: using V4L2 source");
    }

    // Check for any nullptr among the created elements
    const std::vector<std::pair<GstElement *, const char *>> elements = {
        { pipeline, "pipeline" },         //
        { source, "source" },             //
        { capsfilter1, "filter1" },       //
        { videoconvert, "videoconvert" }, //
        { capsfilter2, "filter2" },       //
        { x264enc, "encoder" },           //
        { appsink, "appsink" }            //
    };
    const bool isElementFactoryMakeFailed = GstreamerPipepline::isGstElementsNull(elements);

    // If any element creation failed, log the error and unreference the elements
    if (isElementFactoryMakeFailed)
    {
        ChipLogError(Camera, "Not all elements could be created.");
        // Unreference the elements that were created
        GstreamerPipepline::unrefGstElements(pipeline, source, capsfilter1, videoconvert, capsfilter2, x264enc, appsink);
        error = CameraError::ERROR_INIT_FAILED;
        return nullptr;
    }

    // Camera caps request: RAW @ WxH @ fps
    GstCaps * caps1 = gst_caps_new_simple("video/x-raw", "width", G_TYPE_INT, width, "height", G_TYPE_INT, height, "framerate",
                                          GST_TYPE_FRACTION, framerate, 1, nullptr);
    g_object_set(capsfilter1, "caps", caps1, nullptr);
    gst_caps_unref(caps1);

    // Camera caps request: I420
    GstCaps * caps2 = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "I420", nullptr);
    g_object_set(capsfilter2, "caps", caps2, nullptr);
    gst_caps_unref(caps2);

    // Configure encoder for low‑latency and force IDR at start
    g_object_set(x264enc, "tune", 0, "speed-preset", 1, "key-int-max", framerate * 1, "insert-vui", TRUE, nullptr);

    // Configure appsink for receiving H.264 buffers data
    g_object_set(appsink, "emit-signals", TRUE, nullptr);

    // Build pipeline: v4l2src → capsfilter1 → videoconvert → capsfilter2 -> x264enc → appsink
    gst_bin_add_many(GST_BIN(pipeline), source, capsfilter1, videoconvert, capsfilter2, x264enc, appsink, nullptr);

    // Link the elements
    if (!gst_element_link_many(source, capsfilter1, videoconvert, capsfilter2, x264enc, appsink, nullptr))
    {
        ChipLogError(Camera, "CreateVideoPipeline: link failed");

        // The bin (pipeline) will unref all added elements automatically when you unref the bin.
        gst_object_unref(pipeline);
        error = CameraError::ERROR_INIT_FAILED;
        return nullptr;
    }

    return pipeline;
}

// Helper function to create a GStreamer pipeline
GstElement * CameraDevice::CreateAudioPipeline(const std::string & device, int channels, int sampleRate, int bitRate,
                                               CameraError & error)
{
    if (!mOnvifConfig.rtspUrl.empty())
    {
        error = CameraError::ERROR_INIT_FAILED;
        return nullptr;
    }

    // Pipeline: source → capsfilter → audioconvert → audioresample → opusenc → appsink
    GstElement * pipeline = gst_pipeline_new("audio-pipeline");
    GstElement * source   = nullptr;

    if (LinuxDeviceOptions::GetInstance().cameraTestAudiosrc)
    {
        source = gst_element_factory_make("audiotestsrc", "source");
        g_object_set(source, "wave", 0, "is-live", TRUE, nullptr); // beep 0
        ChipLogProgress(Camera, "Audio piepline: using test audio source");
    }
    else
    {
        source = gst_element_factory_make("pulsesrc", "source");
        // g_object_set(source, "device", device.c_str(), nullptr);
    }

    GstElement * acaps   = gst_element_factory_make("capsfilter", "acaps");
    GstElement * aconv   = gst_element_factory_make("audioconvert", "aconv");
    GstElement * ares    = gst_element_factory_make("audioresample", "ares");
    GstElement * opusenc = gst_element_factory_make("opusenc", "opus");
    GstElement * appsink = gst_element_factory_make("appsink", "appsink");

    // Check creations (same helpers you already use for video)
    const std::vector<std::pair<GstElement *, const char *>> elements = {
        { pipeline, "pipeline" }, { source, "source" },   { acaps, "acaps" },     { aconv, "aconv" },
        { ares, "ares" },         { opusenc, "opusenc" }, { appsink, "appsink" },
    };

    const bool isElementFactoryMakeFailed = GstreamerPipepline::isGstElementsNull(elements);

    // If any element creation failed, log the error and unreference the elements
    if (isElementFactoryMakeFailed)
    {
        ChipLogError(Camera, "Not all elements could be created.");

        // Unreference the elements that were created
        GstreamerPipepline::unrefGstElements(pipeline, source, acaps, aconv, ares, opusenc, appsink);

        error = CameraError::ERROR_INIT_FAILED;
        return nullptr;
    }

    // Create GstCaps for the audio source
    GstCaps * caps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "S16LE", "rate", G_TYPE_INT, sampleRate,
                                         "channels", G_TYPE_INT, channels, nullptr);
    g_object_set(acaps, "caps", caps, nullptr);
    gst_caps_unref(caps);

    // Match controller expectations: Opus @ 64 kbps (kAudioBitrate)
    g_object_set(opusenc, "bitrate", bitRate, "frame-size", AUDIO_FRAMESIZE, "inband-fec", FALSE, "dtx", FALSE, nullptr);

    // Emit samples to your appsink callback (DistributeAudio → packetizer)
    g_object_set(appsink, "emit-signals", TRUE, nullptr);
    g_object_set(source, "do-timestamp", TRUE, nullptr);

    // Build and link
    gst_bin_add_many(GST_BIN(pipeline), source, acaps, aconv, ares, opusenc, appsink, nullptr);
    if (!gst_element_link_many(source, acaps, aconv, ares, opusenc, appsink, nullptr))
    {
        ChipLogError(Camera, "CreateAudioPipeline: link failed");
        error = CameraError::ERROR_INIT_FAILED;
        return nullptr;
    }

    return pipeline;
}

// Helper function to create a GStreamer pipeline for audio playback
GstElement * CameraDevice::CreateAudioPlaybackPipeline(CameraError & error)
{
    GstElement * pipeline        = gst_pipeline_new("audio-playback-pipeline");
    GstElement * udpsrc          = gst_element_factory_make("udpsrc", "audio_udpsrc");
    GstElement * rtpjitterbuffer = gst_element_factory_make("rtpjitterbuffer", "rtp_jitter_buffer");
    GstElement * rtpopusdepay    = gst_element_factory_make("rtpopusdepay", "rtp_opus_depay");
    GstElement * opusdec         = gst_element_factory_make("opusdec", "opus_dec");
    GstElement * audioconvert    = gst_element_factory_make("audioconvert", "audio_convert");
    GstElement * audioresample   = gst_element_factory_make("audioresample", "audio_resample");
    GstElement * capsfilter      = gst_element_factory_make("capsfilter", "audio_caps");
    GstElement * queue           = gst_element_factory_make("queue", "audio_queue");
    GstElement * autoaudiosink   = gst_element_factory_make("autoaudiosink", "audio_sink");

    const std::vector<std::pair<GstElement *, const char *>> elements = {
        { pipeline, "pipeline" },           { udpsrc, "udpsrc" },         { rtpjitterbuffer, "rtpjitterbuffer" },
        { rtpopusdepay, "rtpopusdepay" },   { opusdec, "opusdec" },       { audioconvert, "audioconvert" },
        { audioresample, "audioresample" }, { capsfilter, "capsfilter" }, { queue, "queue" },
        { autoaudiosink, "autoaudiosink" }
    };

    if (GstreamerPipepline::isGstElementsNull(elements))
    {
        ChipLogError(Camera, "Not all audio playback pipeline elements could be created.");
        GstreamerPipepline::unrefGstElements(pipeline, udpsrc, rtpjitterbuffer, rtpopusdepay, opusdec, audioconvert, audioresample,
                                             capsfilter, queue, autoaudiosink);
        error = CameraError::ERROR_INIT_FAILED;
        return nullptr;
    }

    g_object_set(udpsrc, "port", 6001, nullptr);
    GstCaps * udpsrc_caps =
        gst_caps_new_simple("application/x-rtp", "media", G_TYPE_STRING, "audio", "encoding-name", G_TYPE_STRING, "OPUS", "payload",
                            G_TYPE_INT, 111, "clock-rate", G_TYPE_INT, 48000, "channel", G_TYPE_INT, 1, nullptr);
    g_object_set(udpsrc, "caps", udpsrc_caps, nullptr);
    gst_caps_unref(udpsrc_caps);

    // Configure rtpjitterbuffer
    // latency is set high (2.5s) because too much underflow, clock-skew issues are observed with the piepline is receiving audio
    // stream from libdatachannel.
    g_object_set(rtpjitterbuffer, "latency", 2500, nullptr);

    // Configure capsfilter for autoaudiosink
    GstCaps * sink_caps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "S16LE", "layout", G_TYPE_STRING,
                                              "interleaved", "channels", G_TYPE_INT, 1, "rate", G_TYPE_INT, 48000, nullptr);
    g_object_set(capsfilter, "caps", sink_caps, nullptr);
    gst_caps_unref(sink_caps);
    g_object_set(autoaudiosink, "sync", FALSE, nullptr);

    gst_bin_add_many(GST_BIN(pipeline), udpsrc, rtpjitterbuffer, rtpopusdepay, opusdec, audioconvert, audioresample, capsfilter,
                     queue, autoaudiosink, nullptr);

    if (!gst_element_link_many(udpsrc, rtpjitterbuffer, rtpopusdepay, opusdec, audioconvert, audioresample, capsfilter, queue,
                               autoaudiosink, nullptr))
    {
        ChipLogError(Camera, "Audio playback pipeline elements could not be linked.");
        gst_object_unref(pipeline);
        error = CameraError::ERROR_INIT_FAILED;
        return nullptr;
    }

    return pipeline;
}

// Helper function to set V4L2 control
CameraError CameraDevice::SetV4l2Control(uint32_t controlId, int value)
{
    if (videoDeviceFd == -1)
    {
        return CameraError::ERROR_INIT_FAILED;
    }

    v4l2_control control;
    control.id    = controlId;
    control.value = value;

    if (ioctl(videoDeviceFd, VIDIOC_S_CTRL, &control) == -1)
    {
        ChipLogError(Camera, "Error setting V4L2 control: %s", strerror(errno));

        return CameraError::ERROR_CONFIG_FAILED;
    }

    return CameraError::SUCCESS;
}

// Find the closest allocated snapshot stream with resolution >= requested, or closest possible
bool CameraDevice::MatchClosestSnapshotParams(const VideoResolutionStruct & requested, VideoResolutionStruct & matchedResolution,
                                              ImageCodecEnum & matchedCodec)
{
    int64_t requestedPixels = static_cast<int64_t>(requested.width) * requested.height;
    int64_t bestDiff        = std::numeric_limits<int64_t>::max();
    int64_t bestGEQDiff     = std::numeric_limits<int64_t>::max();

    const SnapshotStream * bestStream    = nullptr;
    const SnapshotStream * bestGEQStream = nullptr;

    for (const auto & stream : mSnapshotStreams)
    {
        int64_t streamPixels = static_cast<int64_t>(stream.snapshotStreamParams.minResolution.width) *
            stream.snapshotStreamParams.minResolution.height;
        int64_t diff    = streamPixels - requestedPixels;
        int64_t absDiff = std::abs(diff);

        // Candidate 1: First stream with resolution >= requested
        if (diff >= 0 && diff < bestGEQDiff)
        {
            bestGEQDiff   = diff;
            bestGEQStream = &stream;
        }

        // Candidate 2: Closest stream (absolute difference)
        if (absDiff < bestDiff)
        {
            bestDiff   = absDiff;
            bestStream = &stream;
        }
    }

    const SnapshotStream * chosen = bestGEQStream ? bestGEQStream : bestStream;
    if (chosen)
    {
        matchedResolution = chosen->snapshotStreamParams.minResolution;
        matchedCodec      = chosen->snapshotStreamParams.imageCodec;
        return true;
    }
    return false;
}

namespace {

// libcurl sink: append the HTTP body into a std::string.
size_t SnapCurlWrite(char * ptr, size_t sz, size_t nm, void * ud)
{
    static_cast<std::string *>(ud)->append(ptr, sz * nm);
    return sz * nm;
}

// Try the camera's ONVIF GetSnapshotUri JPEG endpoint. Fills `out` with JPEG bytes and
// returns true only on HTTP 200 with a valid JPEG SOI marker (many cheap cameras advertise
// the URI but return an error page / HTTP 500).
bool FetchOnvifSnapshot(const std::string & url, const std::string & user, const std::string & pass, std::string & out)
{
    if (url.empty())
        return false;
    CURL * curl = curl_easy_init();
    if (!curl)
        return false;
    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, SnapCurlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    if (!user.empty())
    {
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_ANY)); // Basic or Digest
        curl_easy_setopt(curl, CURLOPT_USERNAME, user.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, pass.c_str());
    }
    CURLcode rc = curl_easy_perform(curl);
    long code   = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    if (rc == CURLE_OK && code == 200 && body.size() > 3 && static_cast<unsigned char>(body[0]) == 0xFF &&
        static_cast<unsigned char>(body[1]) == 0xD8)
    {
        out.swap(body);
        return true;
    }
    return false;
}

// Hard cap for ANY JPEG served by CaptureSnapshot, shared by all snapshot tiers (including
// the camera-produced tier-1 ONVIF JPEG — field failures showed busy scenes pushing it past
// the cluster limit: "Snapshot image file size(75823) exceeded limit 63802"). Stays under
// the CaptureSnapshot image limit (63802).
constexpr int kMaxSnapshotJpegBytes = 60000;

// MJPEG-encode one decoded frame to `path` via libav. The decoded H.264 frame is YUV420P;
// the MJPEG encoder wants YUVJ420P (identical layout, JPEG/full range) so we just relabel it.
// The CaptureSnapshot response has a hard payload limit (~62 KB — the cluster rejects larger
// files with cluster status FAILURE (0x01)), so step up the compression until the JPEG fits.
bool EncodeFrameToJpeg(AVFrame * frame, const std::string & path)
{
    const AVCodec * enc = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!enc)
        return false;

    bool ok          = false;
    int savedFmt     = frame->format;
    int64_t savedPts = frame->pts;
    frame->format    = AV_PIX_FMT_YUVJ420P;
    frame->pts       = 0;

    // MJPEG qscale: 1 = best, 31 = smallest. Escalate compression until the JPEG fits the
    // CaptureSnapshot cap; the last step MUST be the true minimum q=31 (a plain 8,15,22,29
    // ramp stops at 29 and never tries the smallest, so a very busy full-res frame could stay
    // over the cap even though q=31 would fit).
    static const int kQSteps[] = { 8, 15, 22, 29, 31 };
    for (int qi = 0; qi < static_cast<int>(sizeof(kQSteps) / sizeof(kQSteps[0])) && !ok; ++qi)
    {
        const int q           = kQSteps[qi];
        AVCodecContext * ectx = avcodec_alloc_context3(enc);
        if (!ectx)
            break;
        ectx->width          = frame->width;
        ectx->height         = frame->height;
        ectx->pix_fmt        = AV_PIX_FMT_YUVJ420P;
        ectx->color_range    = AVCOL_RANGE_JPEG;
        ectx->time_base      = AVRational{ 1, 25 };
        ectx->flags |= AV_CODEC_FLAG_QSCALE;
        ectx->global_quality = FF_QP2LAMBDA * q;

        AVPacket * pkt = av_packet_alloc();
        if (pkt && avcodec_open2(ectx, enc, nullptr) == 0)
        {
            frame->quality = FF_QP2LAMBDA * q;
            if (avcodec_send_frame(ectx, frame) == 0 && avcodec_receive_packet(ectx, pkt) == 0)
            {
                if (pkt->size <= kMaxSnapshotJpegBytes)
                {
                    std::ofstream f(path, std::ios::binary | std::ios::trunc);
                    if (f.is_open())
                    {
                        f.write(reinterpret_cast<const char *>(pkt->data), pkt->size);
                        ok = f.good();
                    }
                }
                else
                {
                    ChipLogProgress(Camera, "Snapshot JPEG %d bytes > %d at q=%d; recompressing", pkt->size,
                                    kMaxSnapshotJpegBytes, q);
                }
            }
        }
        av_packet_free(&pkt);
        avcodec_free_context(&ectx);
    }

    frame->format = savedFmt;
    frame->pts    = savedPts;
    return ok;
}

// Fallback snapshot: pull one H.264 keyframe from RTSP with a short-lived GStreamer pipeline
// (independent of the live-view path), decode it with libav, and MJPEG-encode to `path`.
// The hub ships no jpegenc/decoder GStreamer plugins, so decode+encode is done via libav.
bool SnapshotViaRtsp(const std::string & rtspUrl, const std::string & user, const std::string & pass,
                     const std::string & path, int timeoutSec)
{
    if (rtspUrl.empty())
        return false;

    std::string desc = "rtspsrc name=src location=\"" + rtspUrl +
        "\" protocols=tcp latency=100 ! rtph264depay ! h264parse config-interval=-1 ! "
        "video/x-h264,stream-format=byte-stream,alignment=au ! "
        "appsink name=sink emit-signals=false sync=false max-buffers=60 drop=true";
    GError * gerr         = nullptr;
    GstElement * pipeline = gst_parse_launch(desc.c_str(), &gerr);
    if (!pipeline)
    {
        if (gerr)
            g_error_free(gerr);
        return false;
    }
    if (gerr)
        g_error_free(gerr);
    GstElement * sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    if (!sink)
    {
        gst_object_unref(pipeline);
        return false;
    }

    // Credentials as rtspsrc properties (not in the launch-string URL, which is built from
    // untrusted config and would leak the password if logged). See the live pipeline above.
    if (!user.empty())
    {
        GstElement * src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
        if (src)
        {
            g_object_set(src, "user-id", user.c_str(), "user-pw", pass.c_str(), nullptr);
            gst_object_unref(src);
        }
    }

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    const AVCodec * dec   = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext * dctx = dec ? avcodec_alloc_context3(dec) : nullptr;
    AVFrame * frame       = av_frame_alloc();
    AVPacket * pkt        = av_packet_alloc();
    bool ok               = false;

    if (dctx && frame && pkt && avcodec_open2(dctx, dec, nullptr) == 0)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
        while (!ok && std::chrono::steady_clock::now() < deadline)
        {
            GstSample * sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 500 * GST_MSECOND);
            if (!sample)
                continue;
            GstBuffer * buf = gst_sample_get_buffer(sample);
            GstMapInfo map;
            if (buf && gst_buffer_map(buf, &map, GST_MAP_READ))
            {
                pkt->data = map.data;
                pkt->size = static_cast<int>(map.size);
                if (avcodec_send_packet(dctx, pkt) == 0)
                {
                    while (avcodec_receive_frame(dctx, frame) == 0)
                    {
                        if (EncodeFrameToJpeg(frame, path))
                        {
                            ok = true;
                            break;
                        }
                    }
                }
                gst_buffer_unmap(buf, &map);
            }
            gst_sample_unref(sample);
        }
    }

    if (pkt)
    {
        pkt->data = nullptr; // borrowed from the GstBuffer map; don't let av_packet_free touch it
        pkt->size = 0;
        av_packet_free(&pkt);
    }
    av_frame_free(&frame);
    if (dctx)
        avcodec_free_context(&dctx);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(sink);
    gst_object_unref(pipeline);
    return ok;
}

// De-duplicate concurrent background snapshot refreshes by output path. Process-global (not a
// CameraDevice member) so a detached refresh thread never touches a possibly-destroyed device.
std::mutex gSnapBusyMutex;
std::set<std::string> gSnapBusyPaths;

bool TrySnapshotBegin(const std::string & path)
{
    std::lock_guard<std::mutex> lk(gSnapBusyMutex);
    return gSnapBusyPaths.insert(path).second; // true = we own this refresh
}
void SnapshotEnd(const std::string & path)
{
    std::lock_guard<std::mutex> lk(gSnapBusyMutex);
    gSnapBusyPaths.erase(path);
}

// Process-global cap on concurrent boot-time (onvifOnly) prefetch workers. Discovery/reboot of a
// multi-camera fleet would otherwise fan out N detached prefetch threads at once (F4); cap them so
// the boot storm is bounded. Cameras that miss the cap warm lazily on the first CaptureSnapshot.
std::atomic<int> gPrefetchInFlight{ 0 };
constexpr int kMaxConcurrentPrefetch = 2;

// Hybrid on-demand snapshot: ONVIF snapshot URI first (camera-produced JPEG, no local
// decode), else decode one RTSP keyframe. Writes a JPEG to `path`.
// Decode a single H.264 access unit (Annex-B with SPS/PPS inline) to a JPEG file via libav.
bool DecodeAuToJpeg(const uint8_t * au, size_t auSize, const std::string & path)
{
    if (au == nullptr || auSize == 0)
        return false;
    const AVCodec * dec   = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext * dctx = dec ? avcodec_alloc_context3(dec) : nullptr;
    AVFrame * frame       = av_frame_alloc();
    AVPacket * pkt        = av_packet_alloc();
    bool ok               = false;
    if (dctx && frame && pkt && avcodec_open2(dctx, dec, nullptr) == 0)
    {
        pkt->data = const_cast<uint8_t *>(au);
        pkt->size = static_cast<int>(auSize);
        if (avcodec_send_packet(dctx, pkt) == 0)
        {
            avcodec_send_packet(dctx, nullptr); // flush so the single frame is emitted
            while (avcodec_receive_frame(dctx, frame) == 0)
            {
                if (EncodeFrameToJpeg(frame, path))
                {
                    ok = true;
                    break;
                }
            }
        }
    }
    if (pkt)
    {
        pkt->data = nullptr; // borrowed
        pkt->size = 0;
        av_packet_free(&pkt);
    }
    av_frame_free(&frame);
    if (dctx)
        avcodec_free_context(&dctx);
    return ok;
}

// Tier-1 oversize handling: decode a camera-produced JPEG with libav's MJPEG decoder and
// re-encode it through the shared qscale loop so it fits kMaxSnapshotJpegBytes. This keeps
// the camera's full-res image and needs no RTSP session, so it works even while live view is
// active (no contention on session-limited cameras). Returns false on any decode problem —
// the caller then falls through to tiers 2-4 exactly as if tier 1 had failed.
bool ReencodeJpegToFit(const std::string & jpeg, const std::string & path)
{
    const AVCodec * dec   = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    AVCodecContext * dctx = dec ? avcodec_alloc_context3(dec) : nullptr;
    AVFrame * frame       = av_frame_alloc();
    AVPacket * pkt        = av_packet_alloc();
    bool ok               = false;

    // libav decoders read past the end of the packet in SIMD paths, so the input MUST carry
    // AV_INPUT_BUFFER_PADDING_SIZE zeroed trailing bytes. Feeding jpeg.data() directly (no
    // padding) made the MJPEG decoder fail on a perfectly valid baseline 4:2:0 JPEG — the
    // observed silent tier-1 failure (Blocker 2). Copy into a padded av_malloc'd buffer.
    uint8_t * padded = static_cast<uint8_t *>(av_malloc(jpeg.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (dctx && frame && pkt && padded && avcodec_open2(dctx, dec, nullptr) == 0)
    {
        memcpy(padded, jpeg.data(), jpeg.size());
        memset(padded + jpeg.size(), 0, AV_INPUT_BUFFER_PADDING_SIZE);
        pkt->data = padded;
        pkt->size = static_cast<int>(jpeg.size());

        bool decoded = false;
        if (avcodec_send_packet(dctx, pkt) == 0)
        {
            avcodec_send_packet(dctx, nullptr); // flush so the single image is emitted
            decoded = (avcodec_receive_frame(dctx, frame) == 0);
        }

        if (!decoded)
        {
            ChipLogProgress(Camera, "Snapshot: ONVIF JPEG re-encode DECODE failed (%zu bytes); falling through to next tier",
                            jpeg.size());
        }
        // EncodeFrameToJpeg relabels the frame as YUVJ420P, which is only valid for 4:2:0
        // input; treat any other subsampling (some cameras emit 4:2:2 JPEGs) as a failure so
        // the caller falls through to tiers 2-4.
        else if (frame->format != AV_PIX_FMT_YUVJ420P && frame->format != AV_PIX_FMT_YUV420P)
        {
            ChipLogProgress(Camera, "Snapshot: ONVIF JPEG pix_fmt %d is not 4:2:0; skipping re-encode", frame->format);
        }
        else
        {
            ok = EncodeFrameToJpeg(frame, path);
            if (!ok)
            {
                ChipLogProgress(Camera, "Snapshot: ONVIF JPEG decoded but ENCODE-to-fit failed; falling through to next tier");
            }
        }
    }
    if (pkt)
    {
        pkt->data = nullptr; // points at `padded`, freed separately below
        pkt->size = 0;
        av_packet_free(&pkt);
    }
    if (padded)
        av_free(padded);
    av_frame_free(&frame);
    if (dctx)
        avcodec_free_context(&dctx);
    return ok;
}

// Hybrid on-demand snapshot: ONVIF snapshot URI (camera-produced JPEG) first; else decode the most
// recent LIVE keyframe (no extra RTSP session); else — only when nothing is streaming — a short
// dedicated RTSP grab. The cached-keyframe path is what stops the thumbnail from starving this
// session-limited camera's live view once it has streamed at least once.
bool GenerateSnapshotJpeg(const OnvifConfig & cfg, const std::vector<uint8_t> & cachedKeyframe, bool liveActive,
                          bool cacheFresh, const std::string & path, bool onvifOnly = false)
{
    std::string jpeg;
    if (FetchOnvifSnapshot(cfg.snapshotUrl, cfg.user, cfg.pass, jpeg))
    {
        if (jpeg.size() <= static_cast<size_t>(kMaxSnapshotJpegBytes))
        {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            if (f.is_open())
            {
                f.write(jpeg.data(), static_cast<std::streamsize>(jpeg.size()));
                if (f.good())
                {
                    ChipLogProgress(Camera, "Snapshot: ONVIF snapshot URI -> %s (%zu bytes)", path.c_str(), jpeg.size());
                    return true;
                }
            }
        }
        // The camera's own JPEG is over the CaptureSnapshot cap (scene-dependent: busy scenes
        // inflate the mainstream JPEG past the cluster limit and the command fails). Never
        // return it as-is — re-encode it locally to fit; on any decode failure fall through
        // to tiers 2-4 exactly as if tier 1 had failed.
        else if (ReencodeJpegToFit(jpeg, path))
        {
            ChipLogProgress(Camera, "Snapshot: ONVIF snapshot URI re-encoded to fit (%zu bytes > %d cap) -> %s", jpeg.size(),
                            kMaxSnapshotJpegBytes, path.c_str());
            return true;
        }
        else
        {
            ChipLogProgress(Camera, "Snapshot: ONVIF JPEG %zu bytes > %d cap and re-encode failed; trying next tier",
                            jpeg.size(), kMaxSnapshotJpegBytes);
        }
    }
    // Boot-time cache warm (onvifOnly): only the lightweight ONVIF curl above is allowed. Do NOT
    // fall to the cached-keyframe / dedicated-RTSP tiers here — with a cold cache at boot those
    // would spin up N concurrent full RTSP sessions + H.264 decodes across a fleet on the
    // ~169 MB-free hub (F4). RTSP-only cameras (no ONVIF snapshot URI) simply warm lazily on the
    // first CaptureSnapshot instead.
    if (onvifOnly)
    {
        return false;
    }
    // While streaming (or if a viewer just did), the cached live keyframe is current — decode it and
    // never open a competing RTSP session.
    if ((liveActive || cacheFresh) && !cachedKeyframe.empty() && DecodeAuToJpeg(cachedKeyframe.data(), cachedKeyframe.size(), path))
    {
        ChipLogProgress(Camera, "Snapshot: decoded cached live keyframe -> %s", path.c_str());
        return true;
    }
    // Idle + stale (nobody streaming, cache old/empty): grab a FRESH frame with a short dedicated RTSP
    // session. Safe from contention because nothing else is using the camera right now.
    if (!liveActive && SnapshotViaRtsp(cfg.rtspUrl, cfg.user, cfg.pass, path, /*timeoutSec=*/3))
    {
        ChipLogProgress(Camera, "Snapshot: refreshed via dedicated RTSP keyframe -> %s", path.c_str());
        return true;
    }
    // Last resort: a stale cached frame beats no thumbnail (e.g. a viewer is mid-negotiation).
    if (!cachedKeyframe.empty() && DecodeAuToJpeg(cachedKeyframe.data(), cachedKeyframe.size(), path))
    {
        ChipLogProgress(Camera, "Snapshot: decoded stale cached keyframe (fallback) -> %s", path.c_str());
        return true;
    }
    ChipLogProgress(Camera, "Snapshot: not produced this pass (liveActive=%d, hadCachedKeyframe=%d) — will retry",
                    liveActive ? 1 : 0, cachedKeyframe.empty() ? 0 : 1);
    return false;
}

} // namespace

std::string CameraDevice::SnapshotCachePath() const
{
    return "/tmp/onvif-bridge/snapshot_" + std::to_string(std::hash<std::string>{}(mOnvifConfig.rtspUrl)) + ".jpg";
}

// Kick one TTL-gated background snapshot refresh on a detached thread. Never blocks the
// caller: GenerateSnapshotJpeg picks a source that never fights live view (it prefers the
// cached live keyframe and only opens a dedicated RTSP session when nothing is streaming),
// and every copy the worker uses is captured by value, so it is safe even if this
// CameraDevice is destroyed while the worker runs. Called from CaptureSnapshot on every
// request (onvifOnly=false → full tier cascade) and once from Init() to warm the cache at
// endpoint creation (onvifOnly=true → ONLY the lightweight ONVIF curl, bounded by a global
// concurrency cap so a fleet reboot doesn't storm the hub with RTSP sessions).
void CameraDevice::TriggerSnapshotRefresh(bool onvifOnly)
{
    std::error_code snapEc;
    std::filesystem::create_directories("/tmp/onvif-bridge", snapEc);
    const std::string snapPath = SnapshotCachePath();

    // Refresh at most once per TTL.
    constexpr auto kSnapshotTtlSec = 10;
    bool liveActive                = mActiveVideoStreams.load() > 0;
    bool stale                     = true;
    {
        std::error_code e;
        auto mtime = std::filesystem::last_write_time(snapPath, e);
        if (!e)
            stale = (std::filesystem::file_time_type::clock::now() - mtime) > std::chrono::seconds(kSnapshotTtlSec);
    }
    if (!(stale && TrySnapshotBegin(snapPath)))
    {
        return;
    }

    // Boot prefetch: honor the global concurrency cap. If we're at the cap, release the busy
    // marker and skip — this camera warms lazily on the first CaptureSnapshot.
    if (onvifOnly)
    {
        if (gPrefetchInFlight.fetch_add(1) >= kMaxConcurrentPrefetch)
        {
            gPrefetchInFlight.fetch_sub(1);
            SnapshotEnd(snapPath);
            return;
        }
    }

    OnvifConfig cfgCopy = mOnvifConfig;
    std::vector<uint8_t> keyframeCopy;
    bool cacheFresh = false;
    {
        std::lock_guard<std::mutex> lk(mKeyframeMutex);
        keyframeCopy = mLastLiveKeyframe;
        if (!mLastLiveKeyframe.empty())
            cacheFresh = (std::chrono::steady_clock::now() - mLastLiveKeyframeTime) < std::chrono::seconds(60);
    }
    std::string tmpPath = snapPath + ".tmp";
    std::thread([cfgCopy, keyframeCopy, liveActive, cacheFresh, snapPath, tmpPath, onvifOnly]() {
        std::error_code e;
        if (GenerateSnapshotJpeg(cfgCopy, keyframeCopy, liveActive, cacheFresh, tmpPath, onvifOnly))
            std::filesystem::rename(tmpPath, snapPath, e); // atomic swap-in
        else
            std::filesystem::remove(tmpPath, e);
        SnapshotEnd(snapPath);
        if (onvifOnly)
            gPrefetchInFlight.fetch_sub(1);
    }).detach();
}

CameraError CameraDevice::CaptureSnapshot(const chip::app::DataModel::Nullable<uint16_t> streamID,
                                          const VideoResolutionStruct & resolution, ImageSnapshot & outImageSnapshot)
{
    VideoResolutionStruct matchedRes;
    ImageCodecEnum matchedCodec;

    if (streamID.IsNull())
    {
        if (!MatchClosestSnapshotParams(resolution, matchedRes, matchedCodec))
        {
            ChipLogError(Camera, "No matching snapshot stream found for requested resolution %ux%u", resolution.width,
                         resolution.height);
            return CameraError::ERROR_CAPTURE_SNAPSHOT_FAILED;
        }
    }
    else
    {
        uint16_t streamId = streamID.Value();
        auto it           = std::find_if(mSnapshotStreams.begin(), mSnapshotStreams.end(), [streamId](const SnapshotStream & s) {
            return s.snapshotStreamParams.snapshotStreamID == streamId;
        });
        if (it == mSnapshotStreams.end())
        {
            ChipLogError(Camera, "Snapshot stream not found for stream ID %u", streamId);
            return CameraError::ERROR_CAPTURE_SNAPSHOT_FAILED;
        }
        matchedRes   = it->snapshotStreamParams.minResolution;
        matchedCodec = it->snapshotStreamParams.imageCodec;
    }

    // Serve a cached JPEG and refresh it in the BACKGROUND. CaptureSnapshot runs on the Matter
    // event loop, so it must not block on RTSP/curl I/O (that would stall the node and drop it
    // offline). The refresh thread touches only value-captured copies + process-global state, so
    // it is safe even if this CameraDevice is destroyed while it runs. Cache lives in tmpfs
    // (no flash wear, auto-cleaned on reboot); the atomic rename means readers never see a
    // partial file.
    const std::string snapPath = SnapshotCachePath();
    TriggerSnapshotRefresh();

    // Return the most recent cached JPEG. On the very first request (none cached yet) this
    // fails; the controller retries and gets the image once the background refresh finishes.
    std::ifstream file(snapPath, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        ChipLogProgress(Camera, "Snapshot not ready yet (refreshing in background): %s", snapPath.c_str());
        return CameraError::ERROR_CAPTURE_SNAPSHOT_FAILED;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    // Ensure space for image snapshot data in outImageSnapshot
    outImageSnapshot.data.resize(static_cast<size_t>(size));

    if (!file.read(reinterpret_cast<char *>(outImageSnapshot.data.data()), size))
    {
        ChipLogError(Camera, "Error reading image file: ");
        file.close();
        return CameraError::ERROR_CAPTURE_SNAPSHOT_FAILED;
    }

    file.close();

    outImageSnapshot.imageRes   = matchedRes;
    outImageSnapshot.imageCodec = matchedCodec;

    return CameraError::SUCCESS;
}

CameraError CameraDevice::StartVideoStream(const VideoStreamStruct & allocatedStream)
{
    uint16_t streamID = allocatedStream.videoStreamID;
    auto it           = std::find_if(mVideoStreams.begin(), mVideoStreams.end(),
                                     [streamID](const VideoStream & s) { return s.videoStreamParams.videoStreamID == streamID; });

    if (it == mVideoStreams.end())
    {
        return CameraError::ERROR_VIDEO_STREAM_START_FAILED;
    }

    // Serialise against StopVideoStream and the RTSP watchdog's in-place restarts (this is
    // called from Matter-thread contexts AND from the media controller on WebRTC threads).
    std::lock_guard<std::mutex> lifecycleLock(mPipelineLifecycleMutex);

    // On-demand reference counting: multiple consumers (live WebRTC viewers and/or
    // the push recorder) share a single pipeline. If one is already running, just
    // record the extra consumer and reuse it; StopVideoStream tears the pipeline
    // down only when the last consumer leaves. This avoids pulling the camera's
    // RTSP stream 24/7 when nobody is watching. Keyed on the consumer count, NOT on
    // videoContext: after a failed watchdog rebuild the pipeline pointer is null while
    // consumers remain registered, and the watchdog keeps retrying on its backoff.
    auto rcIt = mVideoStreamConsumers.find(streamID);
    if (rcIt != mVideoStreamConsumers.end() && rcIt->second > 0)
    {
        rcIt->second++;
        ChipLogProgress(Camera, "Video stream %u already running; consumers=%d", streamID, rcIt->second);
        return CameraError::SUCCESS;
    }

    CameraError error = BuildAndStartVideoPipeline(*it);
    if (error != CameraError::SUCCESS)
    {
        return error;
    }

    mVideoStreamConsumers[streamID] = 1; // first consumer
    mActiveVideoStreams.fetch_add(1);    // a live pipeline now holds an RTSP session
    StartWatchdog();                     // spawn/wake the RTSP no-data watchdog

    return CameraError::SUCCESS;
}

// Build + start the GStreamer pipeline for one allocated video stream. Consumer ref-counts
// are NOT touched here — StartVideoStream and the watchdog restart own those. Caller must
// hold mPipelineLifecycleMutex.
CameraError CameraDevice::BuildAndStartVideoPipeline(VideoStream & stream)
{
    const VideoStreamStruct & params = stream.videoStreamParams;
    const uint16_t streamID          = params.videoStreamID;

    // Create Gstreamer video pipeline using the final allocated stream parameters
    CameraError error          = CameraError::SUCCESS;
    GstElement * videoPipeline = CreateVideoPipeline(mVideoDevicePath, params.minResolution.width, params.minResolution.height,
                                                     params.minFrameRate, error);
    if (videoPipeline == nullptr)
    {
        ChipLogError(Camera, "Failed to create video pipeline.");
        stream.videoContext = nullptr;
        return error;
    }

    // Get the appsink and set up callback
    GstElement * appsink = gst_bin_get_by_name(GST_BIN(videoPipeline), "appsink");
    if (appsink)
    {
        VideoAppSinkContext * context = new VideoAppSinkContext{ this, streamID };
        GstAppSinkCallbacks callbacks = { nullptr, nullptr, OnNewVideoSampleFromAppSink };
        gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &callbacks, context, DestroyVideoAppSinkContext);
        gst_object_unref(appsink);
    }

    ChipLogProgress(Camera, "Starting video stream (id=%u): %u×%u @ %ufps", streamID, params.minResolution.width,
                    params.minResolution.height, params.minFrameRate);

    // Start the pipeline
    ChipLogProgress(Camera, "Requesting PLAYING …");
    GstStateChangeReturn result = gst_element_set_state(videoPipeline, GST_STATE_PLAYING);
    if (result == GST_STATE_CHANGE_FAILURE)
    {
        // Get error message from GStreamer bus
        GstBus * bus = gst_element_get_bus(videoPipeline);
        if (bus)
        {
            GstMessage * msg = gst_bus_pop_filtered(bus, (GstMessageType) (GST_MESSAGE_ERROR | GST_MESSAGE_WARNING));
            if (msg)
            {
                GError * err       = nullptr;
                gchar * debug_info = nullptr;

                if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
                {
                    gst_message_parse_error(msg, &err, &debug_info);
                    ChipLogError(Camera, "GStreamer Error: %s", err ? err->message : "unknown");
                    if (debug_info)
                    {
                        ChipLogError(Camera, "Debug info: %s", debug_info);
                    }
                }
                else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_WARNING)
                {
                    gst_message_parse_warning(msg, &err, &debug_info);
                    ChipLogError(Camera, "GStreamer Warning: %s", err ? err->message : "unknown");
                    if (debug_info)
                    {
                        ChipLogError(Camera, "Debug info: %s", debug_info);
                    }
                }

                g_error_free(err);
                g_free(debug_info);
                gst_message_unref(msg);
            }
            gst_object_unref(bus);
        }

        ChipLogError(Camera, "Failed to start video pipeline.");
        gst_object_unref(videoPipeline);
        stream.videoContext = nullptr;
        return CameraError::ERROR_VIDEO_STREAM_START_FAILED;
    }

    // Wait for the pipeline to reach the PLAYING state. A live ONVIF/RTSP source
    // negotiates asynchronously (rtspsrc does DESCRIBE/SETUP/PLAY, which can take
    // several seconds), so get_state frequently returns GST_STATE_CHANGE_ASYNC
    // here. That is NOT a failure: keep the pipeline running and let it reach
    // PLAYING in the background -- the appsink new-sample callback starts
    // delivering H.264 frames as soon as rtspsrc connects. Tearing the pipeline
    // down on a timeout (the previous behavior) permanently killed video whenever
    // the camera was slow to answer, so the WebRTC viewer got no media and the
    // session timed out. Only a hard FAILURE is fatal.
    GstState state;
    GstStateChangeReturn waitResult =
        gst_element_get_state(videoPipeline, &state, nullptr, VIDEO_PIPELINE_PLAY_TIMEOUT * GST_SECOND);
    if (waitResult == GST_STATE_CHANGE_FAILURE)
    {
        ChipLogError(Camera, "Video pipeline failed to start.");
        gst_element_set_state(videoPipeline, GST_STATE_NULL);
        gst_object_unref(videoPipeline);
        stream.videoContext = nullptr;
        return CameraError::ERROR_VIDEO_STREAM_START_FAILED;
    }

    // Store in stream context (keep it even if the live source is still negotiating).
    stream.videoContext = videoPipeline;

    // Seed the watchdog: measure the no-data window from the PLAYING request so a slow
    // rtspsrc negotiation isn't misread as a stall.
    {
        std::lock_guard<std::mutex> lk(mWatchdogMutex);
        auto now = std::chrono::steady_clock::now();
        mWatchdogStreams[streamID].lastData = now;
        mWatchdogStreams[streamID].pipelineStart = now;
    }

    if (state == GST_STATE_PLAYING)
    {
        ChipLogProgress(Camera, "Video is PLAYING …");
    }
    else
    {
        ChipLogProgress(Camera,
                        "Video pipeline starting asynchronously (rtspsrc negotiating); frames will flow once connected.");
    }

    return CameraError::SUCCESS;
}

// Stop and free one stream's pipeline WITHOUT touching consumer ref-counts (the watchdog
// rebuilds under live consumers). Caller must hold mPipelineLifecycleMutex. Returns false
// when the GStreamer state change failed (the pipeline is unref'd regardless).
bool CameraDevice::TearDownVideoPipeline(VideoStream & stream)
{
    GstElement * videoPipeline = reinterpret_cast<GstElement *>(stream.videoContext);
    if (videoPipeline == nullptr)
    {
        return true;
    }

    // set_state(NULL) blocks until the streaming thread has drained out of the appsink
    // callback, so no callback runs concurrently past this point. mWatchdogMutex is NOT held
    // across set_state(NULL) (that would deadlock: the callback takes it), only around the
    // map erase below.
    GstStateChangeReturn result = gst_element_set_state(videoPipeline, GST_STATE_NULL);
    gst_object_unref(videoPipeline);
    stream.videoContext = nullptr;

    // A rebuilt pipeline restarts PTS from zero; recompute the wall-clock offset on the first
    // buffer of the new pipeline. Guarded by mWatchdogMutex (F1) — the same mutex the appsink
    // callback holds for its mVideoStreamPtsOffsetMs accesses.
    {
        std::lock_guard<std::mutex> lk(mWatchdogMutex);
        mVideoStreamPtsOffsetMs.erase(stream.videoStreamParams.videoStreamID);
        mAudioBranchLinked = false;
        mAudioEpoch = 0;
        mAudioPadTimeoutLogged = false;
    }

    return result != GST_STATE_CHANGE_FAILURE;
}

// Stop video stream
CameraError CameraDevice::StartVideoStreamByID(uint16_t streamID)
{
    auto it = std::find_if(mVideoStreams.begin(), mVideoStreams.end(),
                           [streamID](const VideoStream & s) { return s.videoStreamParams.videoStreamID == streamID; });
    if (it == mVideoStreams.end())
    {
        ChipLogError(Camera, "StartVideoStreamByID: video stream %u not found/allocated", streamID);
        return CameraError::ERROR_VIDEO_STREAM_START_FAILED;
    }
    return StartVideoStream(it->videoStreamParams);
}

CameraError CameraDevice::StopVideoStream(uint16_t streamID)
{
    auto it = std::find_if(mVideoStreams.begin(), mVideoStreams.end(),
                           [streamID](const VideoStream & s) { return s.videoStreamParams.videoStreamID == streamID; });

    if (it == mVideoStreams.end())
    {
        return CameraError::ERROR_VIDEO_STREAM_STOP_FAILED;
    }

    // Serialise against StartVideoStream and the RTSP watchdog's in-place restarts.
    std::lock_guard<std::mutex> lifecycleLock(mPipelineLifecycleMutex);

    // Reference counted: keep the pipeline alive while other consumers remain;
    // only tear it down when the last consumer leaves.
    auto rcIt = mVideoStreamConsumers.find(streamID);
    if (rcIt != mVideoStreamConsumers.end() && rcIt->second > 1)
    {
        rcIt->second--;
        ChipLogProgress(Camera, "Video stream %u still has %d consumer(s); keeping pipeline", streamID, rcIt->second);
        return CameraError::SUCCESS;
    }
    mVideoStreamConsumers.erase(streamID);
    if (mActiveVideoStreams.load() > 0)
        mActiveVideoStreams.fetch_sub(1); // last consumer left; the live RTSP session is freed

    // Tear the pipeline down FIRST: set_state(NULL) blocks until the streaming thread has
    // drained out of the appsink callback, so no further NoteVideoDataLocked can re-insert a
    // watchdog entry after we erase it below (F2).
    const bool teardownOk = TearDownVideoPipeline(*it);

    // Now safe to forget this stream: no consumers, no more callbacks.
    {
        std::lock_guard<std::mutex> lk(mWatchdogMutex);
        mWatchdogStreams.erase(streamID);
    }

    return teardownOk ? CameraError::SUCCESS : CameraError::ERROR_VIDEO_STREAM_STOP_FAILED;
}

// ---------------------------------------------------------------------------------------------
// RTSP no-data watchdog. See the block comment in camera-device.h for the field failure this
// recovers from (camera stalls its RTSP session after a PushAV clip; the pipeline stays
// "running" and the standing PushAV consumer keeps the ref-count from ever reaching 0).
// Pure media layer: none of this touches the CHIP stack, so no StackLock is involved.
// ---------------------------------------------------------------------------------------------

// Called from the appsink streaming thread for every video buffer, with mWatchdogMutex held.
void CameraDevice::NoteVideoDataLocked(uint16_t videoStreamID)
{
    auto & st   = mWatchdogStreams[videoStreamID];
    st.lastData = std::chrono::steady_clock::now();
    if (st.backoffSec != 0)
    {
        ChipLogProgress(Camera, "RTSP watchdog: video data flowing again on stream %u; backoff reset", videoStreamID);
        st.backoffSec  = 0;
        st.nextRestart = std::chrono::steady_clock::time_point{};
    }
}

// Called from the appsink streaming thread for each video buffer. Under mWatchdogMutex (F1):
// feeds the RTSP no-data watchdog (proving the camera is delivering data) and computes the
// wall-clock timestamp from the raw PTS using the per-stream offset map (which the watchdog
// thread erases on restart, hence the shared lock). Returns true if the frame should be
// forwarded (timestamp monotonic w.r.t. the stream's first PTS), with outTs/outFirstPts set.
bool CameraDevice::HandleVideoBufferTimestamp(uint16_t videoStreamID, uint64_t rawPts, int64_t & outTs, int64_t & outFirstPts)
{
    std::lock_guard<std::mutex> wlk(mWatchdogMutex);

    NoteVideoDataLocked(videoStreamID);

    auto firstPtsIt = mVideoStreamPtsOffsetMs.find(videoStreamID);
    if (firstPtsIt == mVideoStreamPtsOffsetMs.end())
    {
        auto now                               = std::chrono::steady_clock::now().time_since_epoch();
        int64_t nowMs                          = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        int64_t rawMs                          = static_cast<int64_t>(rawPts / 1000000);
        mVideoStreamPtsOffsetMs[videoStreamID] = nowMs - rawMs;
    }
    outFirstPts = mVideoStreamPtsOffsetMs[videoStreamID];
    outTs       = outFirstPts + static_cast<int64_t>(rawPts / 1000000);
    return outTs >= outFirstPts;
}

void CameraDevice::SetAudioBranchLinked(bool linked)
{
    std::lock_guard<std::mutex> lk(mWatchdogMutex);
    mAudioBranchLinked = linked;
}

void CameraDevice::StartWatchdog()
{
    {
        std::lock_guard<std::mutex> lk(mWatchdogCvMutex);
        if (mWatchdogStop)
        {
            return; // shutting down; don't spawn a new thread
        }
        // Set the wake flag BEFORE notifying so a watchdog about to go dormant (it just
        // observed "no consumers") cannot miss this start (lost-wakeup guard).
        mWatchdogWake = true;
        if (!mWatchdogThread.joinable())
        {
            mWatchdogThread = std::thread([this]() { WatchdogLoop(); });
        }
    }
    mWatchdogCv.notify_all(); // wake a dormant watchdog: a pipeline just started
}

void CameraDevice::StopWatchdog()
{
    {
        std::lock_guard<std::mutex> lk(mWatchdogCvMutex);
        mWatchdogStop = true;
    }
    mWatchdogCv.notify_all();
    if (mWatchdogThread.joinable())
    {
        mWatchdogThread.join();
    }
}

void CameraDevice::WatchdogLoop()
{
    for (;;)
    {
        // Run the stream check OUTSIDE the cv mutex — it takes mPipelineLifecycleMutex, and
        // StartWatchdog takes mWatchdogCvMutex while holding the lifecycle lock, so holding
        // both here in the other order would risk a deadlock. hasConsumers is derived from the
        // SAME consumer refcount that keeps the pipeline alive (mVideoStreamConsumers), so a
        // PushAV standing consumer keeps the watchdog awake through a viewerless stall.
        const bool hasConsumers = WatchdogCheckStreams();

        std::unique_lock<std::mutex> lk(mWatchdogCvMutex);
        if (mWatchdogStop)
        {
            return;
        }
        if (hasConsumers)
        {
            // Pipelines are live: 1 s tick (or wake early on stop).
            mWatchdogCv.wait_for(lk, std::chrono::seconds(kWatchdogPollSec), [this] { return mWatchdogStop; });
        }
        else
        {
            // No consumer on any stream: go fully dormant (idle cameras must cost ~0). Woken by
            // StartWatchdog when a pipeline starts; the mWatchdogWake predicate closes the
            // lost-wakeup window between WatchdogCheckStreams observing "no consumers" and this
            // wait.
            mWatchdogCv.wait(lk, [this] { return mWatchdogStop || mWatchdogWake; });
        }
        mWatchdogWake = false;
        if (mWatchdogStop)
        {
            return;
        }
    }
}

bool CameraDevice::WatchdogCheckStreams()
{
    std::lock_guard<std::mutex> lifecycleLock(mPipelineLifecycleMutex);
    const auto now    = std::chrono::steady_clock::now();
    bool anyConsumers = false;

    for (auto & stream : mVideoStreams)
    {
        const uint16_t streamID = stream.videoStreamParams.videoStreamID;

        // Only pipelines that are supposed to be streaming (>= 1 registered consumer).
        auto rcIt = mVideoStreamConsumers.find(streamID);
        if (rcIt == mVideoStreamConsumers.end() || rcIt->second <= 0)
        {
            continue;
        }
        anyConsumers = true;

        // Read (and defensively seed) this stream's watchdog state. A stream that has a
        // consumer but no entry yet — e.g. a pipeline that connected but has not delivered a
        // single frame, the common post-recording stall — gets seeded here so its no-data
        // window starts now and becomes detectable, instead of being skipped forever.
        std::chrono::steady_clock::time_point lastData;
        std::chrono::steady_clock::time_point nextRestart;
        int backoffSec = 0;
        {
            std::lock_guard<std::mutex> lk(mWatchdogMutex);
            auto wit = mWatchdogStreams.find(streamID);
            if (wit == mWatchdogStreams.end())
            {
                WatchdogStreamState seed;
                seed.lastData          = now;
                seed.pipelineStart     = now;
                mWatchdogStreams[streamID] = seed;
                continue; // just seeded; evaluate on the next tick
            }
            lastData    = wit->second.lastData;
            nextRestart = wit->second.nextRestart;
            backoffSec  = wit->second.backoffSec;
        }

        // Bounded retry: while inside the backoff window, do nothing AND leave any bus
        // ERROR/EOS queued (F5) so it is acted on the moment the backoff expires rather than
        // being consumed and lost.
        if (now < nextRestart)
        {
            continue;
        }

        GstElement * pipeline = reinterpret_cast<GstElement *>(stream.videoContext);

        // Hook bus ERROR/EOS into the restart path (historically only a start-failure check
        // existed; a mid-stream error or EOS left the pipeline dead forever).
        bool busFailure = false;
        if (pipeline != nullptr)
        {
            GstBus * bus = gst_element_get_bus(pipeline);
            if (bus != nullptr)
            {
                GstMessage * msg;
                while ((msg = gst_bus_pop_filtered(bus, (GstMessageType) (GST_MESSAGE_ERROR | GST_MESSAGE_EOS))) != nullptr)
                {
                    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
                    {
                        GError * err       = nullptr;
                        gchar * debug_info = nullptr;
                        gst_message_parse_error(msg, &err, &debug_info);
                        ChipLogError(Camera, "RTSP watchdog: pipeline ERROR on video stream %u: %s", streamID,
                                     err ? err->message : "unknown");
                        g_clear_error(&err);
                        g_free(debug_info);
                    }
                    else
                    {
                        ChipLogError(Camera, "RTSP watchdog: pipeline EOS on video stream %u", streamID);
                    }
                    busFailure = true;
                    gst_message_unref(msg);
                }
                gst_object_unref(bus);
            }
        }

        const bool noData = (now - lastData) >= std::chrono::seconds(kWatchdogNoDataSec);
        // pipeline == nullptr here means an earlier rebuild failed hard while consumers are
        // still registered — keep retrying on the backoff schedule.
        const bool pipelineMissing = (pipeline == nullptr);

        // Pad timeout: video is flowing (or pipeline started) but audio branch was never linked after kWatchdogNoDataSec
        {
            std::lock_guard<std::mutex> lk(mWatchdogMutex);
            if (pipeline != nullptr && mOnvifConfig.HasVerifiedAudio() && !mAudioBranchLinked && !mAudioPadTimeoutLogged)
            {
                auto start_time = mWatchdogStreams[streamID].pipelineStart;
                if ((now - start_time) >= std::chrono::seconds(kWatchdogNoDataSec))
                {
                    ChipLogProgress(Camera, "CAM_AUDIO unavailable reason=pad_timeout");
                    mAudioPadTimeoutLogged = true;
                }
            }
        }

        if (!(busFailure || noData || pipelineMissing))
        {
            continue;
        }

        // Grow the backoff (doubling, capped); it is reset by NoteVideoDataLocked the moment
        // data flows again. Retries continue forever at the capped interval — the camera may
        // come back at any time and there is no other recovery path.
        int newBackoff = (backoffSec == 0) ? kWatchdogInitialBackoffSec : backoffSec * 2;
        if (newBackoff > kWatchdogMaxBackoffSec)
        {
            newBackoff = kWatchdogMaxBackoffSec;
        }

        if (busFailure)
        {
            ChipLogError(Camera, "RTSP watchdog: bus ERROR/EOS on video stream %u — restarting pipeline (retry backoff %ds)",
                         streamID, newBackoff);
        }
        else if (pipelineMissing)
        {
            ChipLogError(Camera, "RTSP watchdog: no pipeline for video stream %u (previous rebuild failed) — rebuilding (retry backoff %ds)",
                         streamID, newBackoff);
        }
        else
        {
            ChipLogError(Camera, "RTSP watchdog: no video data for %ds on stream %u — restarting pipeline (retry backoff %ds)",
                         kWatchdogNoDataSec, streamID, newBackoff);
        }

        // Tear down + rebuild IN PLACE: consumers stay registered (the ref-count path cannot
        // recover this case — the PushAV transport is a standing consumer between clips, so
        // the count never reaches 0), and mActiveVideoStreams keeps counting the stream so a
        // concurrent snapshot won't open a competing RTSP session against the same camera.
        TearDownVideoPipeline(stream);

        {
            std::lock_guard<std::mutex> lk(mWatchdogMutex);
            auto & st      = mWatchdogStreams[streamID];
            st.backoffSec  = newBackoff;
            st.nextRestart = now + std::chrono::seconds(newBackoff);
            st.lastData    = now; // measure the fresh pipeline's no-data window from now
        }

        if (BuildAndStartVideoPipeline(stream) != CameraError::SUCCESS)
        {
            ChipLogError(Camera, "RTSP watchdog: pipeline rebuild failed for video stream %u; will retry in %ds", streamID,
                         newBackoff);
        }
    }

    return anyConsumers;
}

// Start audio stream
CameraError CameraDevice::StartAudioStream(uint16_t streamID)
{
    auto it = std::find_if(mAudioStreams.begin(), mAudioStreams.end(),
                           [streamID](const AudioStream & s) { return s.audioStreamParams.audioStreamID == streamID; });

    if (it == mAudioStreams.end())
    {
        ChipLogError(Camera, "Audio streamID : %u not found", streamID);
        return CameraError::ERROR_AUDIO_STREAM_START_FAILED;
    }

    if (!mOnvifConfig.rtspUrl.empty())
    {
        {
            std::lock_guard<std::mutex> lk(mWatchdogMutex);
            mAudioDeliveryEnabled = true;
        }
        if (LinuxDeviceOptions::GetInstance().cameraAudioPlayback)
        {
            CameraError playbackError = StartAudioPlaybackStream();
            if (playbackError != CameraError::SUCCESS)
            {
                ChipLogError(Camera, "Failed to start audio playback pipeline for stream ID: %u. Error: %d", streamID,
                             static_cast<int>(playbackError));
            }
        }
        ChipLogProgress(Camera, "CAM_AUDIO started streamID=%u", streamID);
        return CameraError::SUCCESS;
    }

    int channels   = it->audioStreamParams.channelCount;
    int sampleRate = static_cast<int>(it->audioStreamParams.sampleRate);
    int bitRate    = static_cast<int>(it->audioStreamParams.bitRate);

    // Create Gstreamer audio pipeline
    CameraError error          = CameraError::SUCCESS;
    GstElement * audioPipeline = CreateAudioPipeline("/dev/audio0", channels, sampleRate, bitRate, error);
    if (audioPipeline == nullptr)
    {
        ChipLogError(Camera, "Failed to create audio pipeline.");
        it->audioContext = nullptr;
        return CameraError::ERROR_AUDIO_STREAM_START_FAILED;
    }

    // Get the appsink and set up callback
    GstElement * appsink = gst_bin_get_by_name(GST_BIN(audioPipeline), "appsink");
    if (appsink)
    {
        AudioAppSinkContext * context = new AudioAppSinkContext{ this, streamID };
        GstAppSinkCallbacks callbacks = { nullptr, nullptr, OnNewAudioSampleFromAppSink };
        gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &callbacks, context, DestroyAudioAppSinkContext);
        gst_object_unref(appsink);
    }

    // Start the pipeline
    GstStateChangeReturn result = gst_element_set_state(audioPipeline, GST_STATE_PLAYING);
    if (result == GST_STATE_CHANGE_FAILURE)
    {
        ChipLogError(Camera, "Failed to start audio pipeline.");
        gst_object_unref(audioPipeline);
        it->audioContext = nullptr;
        return CameraError::ERROR_AUDIO_STREAM_START_FAILED;
    }
    else
    {
        ChipLogProgress(Camera, "Audio Pipeline reached playing state");
    }

    // Wait for the pipeline to reach the PLAYING state
    GstState state;
    gst_element_get_state(audioPipeline, &state, nullptr, GST_CLOCK_TIME_NONE);
    if (state != GST_STATE_PLAYING)
    {
        ChipLogError(Camera, "Audio pipeline did not reach PLAYING state.");
        gst_element_set_state(audioPipeline, GST_STATE_NULL);
        gst_object_unref(audioPipeline);
        it->audioContext = nullptr;
        return CameraError::ERROR_AUDIO_STREAM_START_FAILED;
    }

    // Store in stream context
    it->audioContext = audioPipeline;

    // Start the audio playback pipeline
    if (LinuxDeviceOptions::GetInstance().cameraAudioPlayback)
    {
        CameraError playbackError = StartAudioPlaybackStream();
        if (playbackError != CameraError::SUCCESS)
        {
            ChipLogError(Camera, "Failed to start audio playback pipeline for stream ID: %u. Error: %d", streamID,
                         static_cast<int>(playbackError));
        }
    }

    return CameraError::SUCCESS;
}

// Stop audio stream
CameraError CameraDevice::StopAudioStream(uint16_t streamID)
{
    auto it = std::find_if(mAudioStreams.begin(), mAudioStreams.end(),
                           [streamID](const AudioStream & s) { return s.audioStreamParams.audioStreamID == streamID; });

    if (it == mAudioStreams.end())
    {
        return CameraError::ERROR_AUDIO_STREAM_STOP_FAILED;
    }

    if (!mOnvifConfig.rtspUrl.empty())
    {
        {
            std::lock_guard<std::mutex> lk(mWatchdogMutex);
            mAudioDeliveryEnabled = false;
        }
        if (LinuxDeviceOptions::GetInstance().cameraAudioPlayback)
        {
            CameraError playbackError = StopAudioPlaybackStream();
            if (playbackError != CameraError::SUCCESS)
            {
                ChipLogError(Camera, "Failed to stop audio playback pipeline for stream ID: %u. Error: %d", streamID,
                             static_cast<int>(playbackError));
            }
        }
        ChipLogProgress(Camera, "CAM_AUDIO stopped streamID=%u", streamID);
        return CameraError::SUCCESS;
    }

    GstElement * audioPipeline = reinterpret_cast<GstElement *>(it->audioContext);
    if (audioPipeline != nullptr)
    {
        GstStateChangeReturn result = gst_element_set_state(audioPipeline, GST_STATE_NULL);
        if (result == GST_STATE_CHANGE_FAILURE)
        {
            return CameraError::ERROR_SNAPSHOT_STREAM_STOP_FAILED;
        }
        gst_object_unref(audioPipeline);
        it->audioContext = nullptr;
    }

    // Stop the audio playback pipeline
    if (LinuxDeviceOptions::GetInstance().cameraAudioPlayback)
    {
        CameraError playbackError = StopAudioPlaybackStream();
        if (playbackError != CameraError::SUCCESS)
        {
            ChipLogError(Camera, "Failed to stop audio playback pipeline for stream ID: %u. Error: %d", streamID,
                         static_cast<int>(playbackError));
        }
    }

    mAudioStreamPtsOffsetMs.erase(streamID);

    return CameraError::SUCCESS;
}

CameraError CameraDevice::StartAudioPlaybackStream()
{

    if (mAudioPlaybackPipeline != nullptr)
    {
        ChipLogError(Camera, "Audio playback pipeline already exists. Stop it before starting a new one.");
        return CameraError::ERROR_AUDIO_STREAM_START_FAILED;
    }

    CameraError error      = CameraError::SUCCESS;
    mAudioPlaybackPipeline = CreateAudioPlaybackPipeline(error);
    if (mAudioPlaybackPipeline == nullptr)
    {
        ChipLogError(Camera, "Failed to create audio playback pipeline.");
        return error;
    }

    GstStateChangeReturn result = gst_element_set_state(mAudioPlaybackPipeline, GST_STATE_PLAYING);
    if (result == GST_STATE_CHANGE_FAILURE)
    {
        ChipLogError(Camera, "Failed to start audio playback pipeline.");
        gst_object_unref(mAudioPlaybackPipeline);
        mAudioPlaybackPipeline = nullptr;
        return CameraError::ERROR_AUDIO_STREAM_START_FAILED;
    }

    ChipLogProgress(Camera, "Audio playback pipeline started");
    return CameraError::SUCCESS;
}

CameraError CameraDevice::StopAudioPlaybackStream()
{
    if (mAudioPlaybackPipeline == nullptr)
    {
        ChipLogDetail(Camera, "Audio playback pipeline is not running or already stopped.");
        return CameraError::SUCCESS;
    }

    ChipLogProgress(Camera, "Stopping audio playback pipeline");
    GstStateChangeReturn result = gst_element_set_state(mAudioPlaybackPipeline, GST_STATE_NULL);
    gst_object_unref(mAudioPlaybackPipeline);
    mAudioPlaybackPipeline = nullptr;

    if (result == GST_STATE_CHANGE_FAILURE)
    {
        ChipLogError(Camera, "Failed to stop audio playback pipeline.");
        return CameraError::ERROR_AUDIO_STREAM_STOP_FAILED;
    }

    ChipLogProgress(Camera, "Audio playback pipeline stopped.");
    return CameraError::SUCCESS;
}

// Allocate snapshot stream
CameraError CameraDevice::AllocateSnapshotStream(const CameraAVStreamManagementDelegate::SnapshotStreamAllocateArgs & args,
                                                 uint16_t & outStreamID)
{

    if (AddSnapshotStream(args, outStreamID))
    {
        auto it = std::find_if(mSnapshotStreams.begin(), mSnapshotStreams.end(), [outStreamID](const SnapshotStream & s) {
            return s.snapshotStreamParams.snapshotStreamID == outStreamID;
        });
        if (it == mSnapshotStreams.end())
        {
            ChipLogError(Camera, "Snapshot stream with ID %u not found", outStreamID);
            return CameraError::ERROR_RESOURCE_EXHAUSTED;
        }
        it->isAllocated = true;
        ChipLogProgress(Camera, "Allocated snapshot stream with ID: %u", outStreamID);
        return CameraError::SUCCESS;
    }
    return CameraError::ERROR_RESOURCE_EXHAUSTED;
}

// Start snapshot stream
CameraError CameraDevice::StartSnapshotStream(uint16_t streamID)
{
    auto it = std::find_if(mSnapshotStreams.begin(), mSnapshotStreams.end(),
                           [streamID](const SnapshotStream & s) { return s.snapshotStreamParams.snapshotStreamID == streamID; });
    if (it == mSnapshotStreams.end())
    {
        ChipLogError(Camera, "Snapshot streamID : %u not found", streamID);
        return CameraError::ERROR_SNAPSHOT_STREAM_START_FAILED;
    }

    // Create the GStreamer pipeline
    CameraError error             = CameraError::SUCCESS;
    GstElement * snapshotPipeline = CreateSnapshotPipeline(
        mVideoDevicePath, it->snapshotStreamParams.minResolution.width, it->snapshotStreamParams.minResolution.height,
        it->snapshotStreamParams.quality, it->snapshotStreamParams.frameRate, "capture_snapshot.jpg", error);
    if (snapshotPipeline == nullptr)
    {
        ChipLogError(Camera, "Failed to create snapshot pipeline.");
        it->snapshotContext = nullptr;
        return error;
    }

    // Start the pipeline
    GstStateChangeReturn result = gst_element_set_state(snapshotPipeline, GST_STATE_PLAYING);
    if (result == GST_STATE_CHANGE_FAILURE)
    {
        ChipLogError(Camera, "Failed to start snapshot pipeline.");
        gst_object_unref(snapshotPipeline);
        it->snapshotContext = nullptr;
        return CameraError::ERROR_SNAPSHOT_STREAM_START_FAILED;
    }

    // Wait for the pipeline to reach the PLAYING state
    GstState state;
    gst_element_get_state(snapshotPipeline, &state, nullptr, GST_CLOCK_TIME_NONE);
    if (state != GST_STATE_PLAYING)
    {
        ChipLogError(Camera, "Snapshot pipeline did not reach PLAYING state.");
        gst_element_set_state(snapshotPipeline, GST_STATE_NULL);
        gst_object_unref(snapshotPipeline);
        it->snapshotContext = nullptr;
        return CameraError::ERROR_SNAPSHOT_STREAM_START_FAILED;
    }

    // Store in stream context
    it->snapshotContext = snapshotPipeline;

    return CameraError::SUCCESS;
}

// Stop snapshot stream
CameraError CameraDevice::StopSnapshotStream(uint16_t streamID)
{
    auto it = std::find_if(mSnapshotStreams.begin(), mSnapshotStreams.end(),
                           [streamID](const SnapshotStream & s) { return s.snapshotStreamParams.snapshotStreamID == streamID; });
    if (it == mSnapshotStreams.end())
    {
        return CameraError::ERROR_SNAPSHOT_STREAM_STOP_FAILED;
    }

    GstElement * snapshotPipeline = reinterpret_cast<GstElement *>(it->snapshotContext);
    if (snapshotPipeline != nullptr)
    {
        // Stop the pipeline
        GstStateChangeReturn result = gst_element_set_state(snapshotPipeline, GST_STATE_NULL);
        if (result == GST_STATE_CHANGE_FAILURE)
        {
            return CameraError::ERROR_SNAPSHOT_STREAM_STOP_FAILED;
        }

        // Unreference the pipeline
        gst_object_unref(snapshotPipeline);
        it->snapshotContext = nullptr;
    }

    // Remove the snapshot file
    std::string fileName = SNAPSHOT_FILE_PATH;
    if (unlink(fileName.c_str()) == -1)
    {
        ChipLogError(Camera, "Failed to remove snapshot file after stopping stream (err = %s).", strerror(errno));
        return CameraError::ERROR_SNAPSHOT_STREAM_STOP_FAILED;
    }

    return CameraError::SUCCESS;
}

uint8_t CameraDevice::GetMaxConcurrentEncoders()
{
    return kMaxConcurrentEncoders;
}

uint32_t CameraDevice::GetMaxEncodedPixelRate()
{
    return kMaxEncodedPixelRate;
}

VideoSensorParamsStruct & CameraDevice::GetVideoSensorParams()
{
    static VideoSensorParamsStruct videoSensorParams = { kVideoSensorWidthPixels, kVideoSensorHeightPixels, kMaxVideoFrameRate,
                                                         chip::Optional<uint16_t>(30) }; // Typical numbers for Pi camera.
    return videoSensorParams;
}

bool CameraDevice::GetCameraSupportsHDR()
{
    return true;
}

bool CameraDevice::GetCameraSupportsNightVision()
{
    return true;
}

bool CameraDevice::GetNightVisionUsesInfrared()
{
    return false;
}

bool CameraDevice::GetCameraSupportsWatermark()
{
    return true;
}

bool CameraDevice::GetCameraSupportsOSD()
{
    return true;
}

bool CameraDevice::GetCameraSupportsSoftPrivacy()
{
    return true;
}

bool CameraDevice::GetCameraSupportsImageControl()
{
    return true;
}

VideoResolutionStruct & CameraDevice::GetMinViewport()
{
    static VideoResolutionStruct minViewport = { kMinResolutionWidth, kMinResolutionHeight };
    return minViewport;
}

std::vector<RateDistortionTradeOffStruct> & CameraDevice::GetRateDistortionTradeOffPoints()
{
    static std::vector<RateDistortionTradeOffStruct> rateDistTradeOffs = {
        { VideoCodecEnum::kH264, { kMinResolutionWidth, kMinResolutionHeight }, 10000 /* bitrate */ }
    };
    return rateDistTradeOffs;
}

uint32_t CameraDevice::GetMaxContentBufferSize()
{
    return kMaxContentBufferSizeBytes;
}

AudioCapabilitiesStruct & CameraDevice::GetMicrophoneCapabilities()
{
    static std::array<AudioCodecEnum, 2> audioCodecs = { AudioCodecEnum::kOpus, AudioCodecEnum::kAacLc };
    static std::array<uint32_t, 2> sampleRates       = { 48000, 32000 }; // Sample rates in Hz
    static std::array<uint8_t, 2> bitDepths          = { 24, 32 };
    static AudioCapabilitiesStruct audioCapabilities = { kMicrophoneMaxChannelCount, chip::Span<AudioCodecEnum>(audioCodecs),
                                                         chip::Span<uint32_t>(sampleRates), chip::Span<uint8_t>(bitDepths) };
    return audioCapabilities;
}

AudioCapabilitiesStruct & CameraDevice::GetSpeakerCapabilities()
{
    static std::array<AudioCodecEnum, 2> audioCodecs   = { AudioCodecEnum::kOpus, AudioCodecEnum::kAacLc };
    static std::array<uint32_t, 2> sampleRates         = { 48000, 32000 }; // Sample rates in Hz
    static std::array<uint8_t, 2> bitDepths            = { 24, 32 };
    static AudioCapabilitiesStruct speakerCapabilities = { kSpeakerMaxChannelCount, chip::Span<AudioCodecEnum>(audioCodecs),
                                                           chip::Span<uint32_t>(sampleRates), chip::Span<uint8_t>(bitDepths) };
    return speakerCapabilities;
}

std::vector<SnapshotCapabilitiesStruct> & CameraDevice::GetSnapshotCapabilities()
{
    static std::vector<SnapshotCapabilitiesStruct> snapshotCapabilities = {
        { { kMinResolutionWidth, kMinResolutionHeight },
          kSnapshotStreamFrameRate,
          ImageCodecEnum::kJpeg,
          false,
          chip::MakeOptional(static_cast<bool>(false)) },
        { { k720pResolutionWidth, k720pResolutionHeight },
          kSnapshotStreamFrameRate,
          ImageCodecEnum::kJpeg,
          true,
          chip::MakeOptional(static_cast<bool>(true)) },
    };
    return snapshotCapabilities;
}

CameraError CameraDevice::SetNightVision(TriStateAutoEnum nightVision)
{
    mNightVision = nightVision;

    return CameraError::SUCCESS;
}

uint32_t CameraDevice::GetMaxNetworkBandwidth()
{
    return kMaxNetworkBandwidthbps;
}

uint16_t CameraDevice::GetCurrentFrameRate()
{
    return mCurrentVideoFrameRate;
}

CameraError CameraDevice::SetHDRMode(bool hdrMode)
{
    mHDREnabled = hdrMode;

    return CameraError::SUCCESS;
}

CameraError CameraDevice::SetHardPrivacyMode(bool hardPrivacyMode)
{
    ChipLogProgress(Camera, "SetHardPrivacyMode: Setting hard privacy mode to %s", hardPrivacyMode ? "true" : "false");
    mHardPrivacyModeOn = hardPrivacyMode;

    return CameraError::SUCCESS;
}

CameraError CameraDevice::SetStreamUsagePriorities(std::vector<StreamUsageEnum> streamUsagePriorities)
{
    mStreamUsagePriorities = streamUsagePriorities;

    return CameraError::SUCCESS;
}

std::vector<StreamUsageEnum> & CameraDevice::GetSupportedStreamUsages()
{
    static std::vector<StreamUsageEnum> supportedStreamUsage = { StreamUsageEnum::kLiveView, StreamUsageEnum::kRecording };
    return supportedStreamUsage;
}

CameraError CameraDevice::SetViewport(const chip::app::Clusters::Globals::Structs::ViewportStruct::Type & viewPort)
{
    mViewport = viewPort;

    return CameraError::SUCCESS;
}

CameraError CameraDevice::SetViewport(VideoStream & stream,
                                      const chip::app::Clusters::Globals::Structs::ViewportStruct::Type & viewport)
{
    ChipLogDetail(Camera, "Setting per stream viewport for stream %d.", stream.videoStreamParams.videoStreamID);
    ChipLogDetail(Camera, "New viewport. x1=%d, x2=%d, y1=%d, y2=%d.", viewport.x1, viewport.x2, viewport.y1, viewport.y2);
    stream.viewport = viewport;
    return CameraError::SUCCESS;
}

CameraError CameraDevice::SetSoftRecordingPrivacyModeEnabled(bool softRecordingPrivacyMode)
{
    mSoftRecordingPrivacyModeEnabled = softRecordingPrivacyMode;

    // Notify the PushAVManager about change
    mPushAVTransportManager.RecordingStreamPrivacyModeChanged(softRecordingPrivacyMode);

    return CameraError::SUCCESS;
}

CameraError CameraDevice::SetSoftLivestreamPrivacyModeEnabled(bool softLivestreamPrivacyMode)
{
    mSoftLivestreamPrivacyModeEnabled = softLivestreamPrivacyMode;

    // Notify WebRTCProviderManager about change
    mWebRTCProviderManager.LiveStreamPrivacyModeChanged(softLivestreamPrivacyMode);

    return CameraError::SUCCESS;
}

// Mute/Unmute speaker.
CameraError CameraDevice::SetSpeakerMuted(bool muteSpeaker)
{
    mSpeakerMuted = muteSpeaker;

    return CameraError::SUCCESS;
}

// Set speaker volume level.
CameraError CameraDevice::SetSpeakerVolume(uint8_t speakerVol)
{
    mSpeakerVol = speakerVol;

    return CameraError::SUCCESS;
}

// Mute/Unmute microphone.
CameraError CameraDevice::SetMicrophoneMuted(bool muteMicrophone)
{
    mMicrophoneMuted = muteMicrophone;

    return CameraError::SUCCESS;
}

// Set microphone volume level.
CameraError CameraDevice::SetMicrophoneVolume(uint8_t microphoneVol)
{
    mMicrophoneVol = microphoneVol;

    return CameraError::SUCCESS;
}

// Set image rotation attributes
CameraError CameraDevice::SetImageRotation(uint16_t imageRotation)
{
    mImageRotation = imageRotation;

    return CameraError::SUCCESS;
}

CameraError CameraDevice::SetImageFlipHorizontal(bool imageFlipHorizontal)
{
    mImageFlipHorizontal = imageFlipHorizontal;

    return CameraError::SUCCESS;
}

CameraError CameraDevice::SetImageFlipVertical(bool imageFlipVertical)
{
    mImageFlipVertical = imageFlipVertical;

    return CameraError::SUCCESS;
}

CameraError CameraDevice::SetLocalVideoRecordingEnabled(bool localVideoRecordingEnabled)
{
    mLocalVideoRecordingEnabled = localVideoRecordingEnabled;

    return CameraError::SUCCESS;
}

CameraError CameraDevice::SetLocalSnapshotRecordingEnabled(bool localSnapshotRecordingEnabled)
{
    mLocalSnapshotRecordingEnabled = localSnapshotRecordingEnabled;

    return CameraError::SUCCESS;
}

CameraError CameraDevice::SetStatusLightEnabled(bool statusLightEnabled)
{
    mStatusLightEnabled = statusLightEnabled;

    return CameraError::SUCCESS;
}

int16_t CameraDevice::GetPanMin()
{
    return kMinPanValue;
}

int16_t CameraDevice::GetPanMax()
{
    return kMaxPanValue;
}

int16_t CameraDevice::GetTiltMin()
{
    return kMinTiltValue;
}

int16_t CameraDevice::GetTiltMax()
{
    return kMaxTiltValue;
}

uint8_t CameraDevice::GetZoomMax()
{
    return kMaxZoomValue;
}

// Set the Pan level
CameraError CameraDevice::SetPan(int16_t aPan)
{
    mPan = aPan;
    return CameraError::SUCCESS;
}

// Set the Tilt level
CameraError CameraDevice::SetTilt(int16_t aTilt)
{
    mTilt = aTilt;
    return CameraError::SUCCESS;
}

// Set the Zoom level
CameraError CameraDevice::SetZoom(uint8_t aZoom)
{
    mZoom = aZoom;
    return CameraError::SUCCESS;
}

// Set the PTZ values as received
CameraError CameraDevice::SetPhysicalPTZ(chip::Optional<int16_t> aPan, chip::Optional<int16_t> aTilt, chip::Optional<uint8_t> aZoom)
{
    if (aPan.HasValue())
    {
        SetPan(aPan.Value());
    }

    if (aTilt.HasValue())
    {
        SetTilt(aTilt.Value());
    }

    if (aZoom.HasValue())
    {
        SetZoom(aZoom.Value());
    }

    // ONVIF bridge: forward the resulting absolute MPTZ position to the real camera via
    // ONVIF AbsoluteMove. Matter ranges (pan/tilt ±90, zoom 0..75) map to ONVIF generic
    // space (pan/tilt [-1,1], zoom [0,1]).
    if (!mOnvifConfig.ptzUrl.empty())
    {
        auto clampd  = [](double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); };
        double panN  = clampd(static_cast<double>(mPan) / kMaxPanValue, -1.0, 1.0);
        double tiltN = clampd(static_cast<double>(mTilt) / kMaxTiltValue, -1.0, 1.0);
        double zoomN = clampd(static_cast<double>(mZoom) / kMaxZoomValue, 0.0, 1.0);
        int rc = onvif_ptz_bridge_absmove(mOnvifConfig.ptzUrl.c_str(), mOnvifConfig.token.c_str(),
                                          mOnvifConfig.user.c_str(), mOnvifConfig.pass.c_str(), panN, tiltN, zoomN);
        if (rc != 0)
        {
            ChipLogError(Camera, "ONVIF PTZ AbsoluteMove failed (rc=%d)", rc);
        }
        else
        {
            ChipLogProgress(Camera, "ONVIF PTZ AbsoluteMove pan=%.3f tilt=%.3f zoom=%.3f", panN, tiltN, zoomN);
        }
    }

    return CameraError::SUCCESS;
}

CameraError CameraDevice::SetDetectionSensitivity(uint8_t aSensitivity)
{
    mDetectionSensitivity = aSensitivity;
    return CameraError::SUCCESS;
}

CameraError CameraDevice::CreateZoneTrigger(const ZoneTriggerControlStruct & zoneTrigger)
{

    return CameraError::SUCCESS;
}

CameraError CameraDevice::UpdateZoneTrigger(const ZoneTriggerControlStruct & zoneTrigger)
{

    return CameraError::SUCCESS;
}

CameraError CameraDevice::RemoveZoneTrigger(const uint16_t zoneId)
{

    return CameraError::SUCCESS;
}

void CameraDevice::HandleSimulatedZoneTriggeredEvent(const std::vector<uint16_t> & zoneIds)
{
    // Zone events are per-zone - each zone needs its own event notification
    for (const auto & zoneId : zoneIds)
    {
        mZoneManager.OnZoneTriggeredEvent(zoneId, ZoneEventTriggeredReasonEnum::kMotion);
    }
    // Transport trigger is per-motion-event - all zones are passed together
    // This deliberate asymmetry reflects that zone events track individual zone activity
    // while transport triggers coordinate recording across all zones in a single motion event
    mPushAVTransportManager.HandleZoneTrigger(zoneIds);
}

void CameraDevice::HandleSimulatedZoneStoppedEvent(uint16_t zoneId)
{
    mZoneManager.OnZoneStoppedEvent(zoneId, ZoneEventStoppedReasonEnum::kActionStopped);
    // Note: PushAVTransportManager doesn't need zone stopped event currently
}

void CameraDevice::InitializeVideoStreams()
{
    // Declare the codec the controller can actually request for THIS camera. VideoStreamAllocate
    // is gated on an exact codec match against these declared streams (see
    // CameraAVStreamManager::VideoStreamAllocate -> VideoStream::IsCompatible); if we always
    // declared kH264 here, an H.265 camera's allocate would still "succeed" against this fake
    // menu (SmartThings' offer includes both codecs' rtpmap regardless), but the cluster's
    // AllocatedVideoStreams state would misreport the real codec. Mirrors the same fix already
    // applied for audio (see InitializeAudioStreams: publish what's actually available).
    const VideoCodecEnum codec = (mOnvifConfig.videoCodec == "H265") ? VideoCodecEnum::kHevc : VideoCodecEnum::kH264;

    // Create a video stream with a max resolution of 720p and max frame rate of
    // 60 fps
    VideoStream videoStream1 = { { 1 /* Id */,
                                   StreamUsageEnum::kLiveView /* StreamUsage */,
                                   codec,
                                   kMinVideoFrameRate /* MinFrameRate */,
                                   k60fpsVideoFrameRate /* MaxFrameRate */,
                                   { kMinResolutionWidth, kMinResolutionHeight } /* MinResolution */,
                                   { k720pResolutionWidth, k720pResolutionHeight } /* MaxResolution */,
                                   kMinBitRateBps /* MinBitRate */,
                                   kMaxBitRateBps /* MaxBitRate */,
                                   kKeyFrameIntervalMsec /* KeyFrameInterval */,
                                   chip::MakeOptional(static_cast<bool>(false)) /* WMark */,
                                   chip::MakeOptional(static_cast<bool>(false)) /* OSD */,
                                   0 /* RefCount */ },
                                 false,
                                 { mViewport.x1, mViewport.y1, mViewport.x2, mViewport.y2 },
                                 nullptr };
    mVideoStreams.push_back(videoStream1);

    // Create a video stream with a min framerate of 60 fps and min resolution
    // of 720p
    VideoStream videoStream2 = { { 2 /* Id */,
                                   StreamUsageEnum::kLiveView /* StreamUsage */,
                                   codec,
                                   k60fpsVideoFrameRate /* MinFrameRate */,
                                   kMaxVideoFrameRate /* MaxFrameRate */,
                                   { k720pResolutionWidth, k720pResolutionHeight } /* MinResolution */,
                                   { kMaxResolutionWidth, kMaxResolutionHeight } /* MaxResolution */,
                                   kMinBitRateBps /* MinBitRate */,
                                   kMaxBitRateBps /* MaxBitRate */,
                                   kKeyFrameIntervalMsec /* KeyFrameInterval */,
                                   chip::MakeOptional(static_cast<bool>(false)) /* WMark */,
                                   chip::MakeOptional(static_cast<bool>(false)) /* OSD */,
                                   0 /* RefCount */ },
                                 false,
                                 { mViewport.x1, mViewport.y1, mViewport.x2, mViewport.y2 },
                                 nullptr };

    mVideoStreams.push_back(videoStream2);

    // Create a video stream for the full range(fps, resolution, bitrate) supported by the camera.
    VideoStream videoStream3 = { { 3 /* Id */,
                                   StreamUsageEnum::kLiveView /* StreamUsage */,
                                   codec,
                                   kMinVideoFrameRate /* MinFrameRate */,
                                   kMaxVideoFrameRate /* MaxFrameRate */,
                                   { kMinResolutionWidth, kMinResolutionHeight } /* MinResolution */,
                                   { kMaxResolutionWidth, kMaxResolutionHeight } /* MaxResolution */,
                                   kMinBitRateBps /* MinBitRate */,
                                   kMaxBitRateBps /* MaxBitRate */,
                                   kKeyFrameIntervalMsec /* KeyFrameInterval */,
                                   chip::MakeOptional(static_cast<bool>(false)) /* WMark */,
                                   chip::MakeOptional(static_cast<bool>(false)) /* OSD */,
                                   0 /* RefCount */ },
                                 false,
                                 { mViewport.x1, mViewport.y1, mViewport.x2, mViewport.y2 },
                                 nullptr };

    mVideoStreams.push_back(videoStream3);

    // The controller's VideoStreamAllocate is gated on an exact codec match against these
    // declared streams. If live view fails with DynamicConstraintError (0xcf) on video, this
    // is the line to check: the declared codec here MUST equal the camera's real stream codec.
    ChipLogProgress(Camera, "CAM_CODEC: declared %zu video stream(s) as codec=%s (from onvifConfig.videoCodec='%s')",
                    mVideoStreams.size(), (codec == VideoCodecEnum::kHevc) ? "HEVC/H.265" : "H.264",
                    mOnvifConfig.videoCodec.c_str());
}

void CameraDevice::InitializeAudioStreams()
{
    mAudioStreams.clear();
    if (mOnvifConfig.HasVerifiedAudio())
    {
        AudioStream stream = { { 1 /* Id */, StreamUsageEnum::kLiveView, AudioCodecEnum::kOpus, 1 /* ChannelCount: Mono */,
                                 48000 /* SampleRate */, 20000 /* BitRate */, 24 /* BitDepth */, 0 /* RefCount */ },
                               false,
                               nullptr };
        mAudioStreams.push_back(stream);
        ChipLogProgress(Camera, "CAM_AUDIO stream initialized: stream_id=1 codec=Opus rate=48000 channels=1");
    }
    else
    {
        // [live-view] This camera has no real inbound audio, but HasMicrophone() is advertised
        // as true (kept true so SmartThings allows clip recording — see camera-device.h). With the
        // microphone feature advertised, SmartThings' FIRST live-view step is AudioStreamAllocate
        // for a LiveView audio stream; if there is NO allocatable audio stream that call fails with
        // DYNAMIC_CONSTRAINT_ERROR and SmartThings never proceeds to video — i.e. no live view at
        // all. So publish ONE default, allocatable Opus stream whose params satisfy the controller's
        // request (AudioStream::IsCompatible checks audioCodec==, and channelCount/sampleRate/bitDepth
        // >= the request; bitRate is not checked). No real audio flows on it — the RTSP pipeline has
        // no audio branch and the HAL audio start no-ops gracefully (logs, does not fail the
        // allocate) — the controller tolerates a silent audio track, and live VIDEO now allocates and
        // streams normally. This keeps clip recording working (audio feature present) AND unblocks
        // live view.
        AudioStream stream = { { 1 /* Id */, StreamUsageEnum::kLiveView, AudioCodecEnum::kOpus, 1 /* ChannelCount: Mono */,
                                 48000 /* SampleRate */, 20000 /* BitRate */, 24 /* BitDepth */, 0 /* RefCount */ },
                               false,
                               nullptr };
        mAudioStreams.push_back(stream);
        ChipLogProgress(Camera, "CAM_AUDIO: no real inbound audio — publishing a silent default Opus stream so live-view "
                                "AudioStreamAllocate succeeds (video-only; clip recording keeps the audio feature)");
    }
}

void CameraDevice::InitializeSnapshotStreams()
{
    // Create single snapshot stream with typical supported parameters
    uint16_t streamId = kInvalidStreamID;
    AddSnapshotStream({ ImageCodecEnum::kJpeg,
                        kSnapshotStreamFrameRate /* FrameRate */,
                        { kMinResolutionWidth, kMinResolutionHeight } /* MinResolution*/,
                        { kMaxResolutionWidth, kMaxResolutionHeight } /* MaxResolution */,
                        90 /* Quality */ },
                      streamId);
}

bool CameraDevice::AddSnapshotStream(
    const CameraAVStreamManagementDelegate::SnapshotStreamAllocateArgs & snapshotStreamAllocateArgs, uint16_t & outStreamID)
{
    constexpr uint16_t kMaxSnapshotStreams = std::numeric_limits<uint16_t>::max();

    if (mSnapshotStreams.size() >= kMaxSnapshotStreams)
    {
        ChipLogError(Camera, "Maximum number of snapshot streams reached. Cannot a allocate new one");
        return false;
    }

    uint16_t streamId = 0;
    // Fetch a new stream ID if the passed ID is kInvalidStreamID, otherwise use
    // the ID that was passed in. A valid streamID would be passed in when the
    // stream list is being constructed from the persisted list of allocated
    // streams that was loaded at Init()
    if (outStreamID == kInvalidStreamID)
    {
        for (const auto & s : mSnapshotStreams)
        {
            // Find the highest existing stream ID.
            if (s.snapshotStreamParams.snapshotStreamID > streamId)
            {
                streamId = s.snapshotStreamParams.snapshotStreamID;
            }
        }

        // Find a unique stream id, starting from the last used one above, incrementing and wrapping at 65535.
        for (uint16_t attempts = 0; attempts < kMaxSnapshotStreams; ++attempts)
        {
            auto found = std::find_if(mSnapshotStreams.begin(), mSnapshotStreams.end(), [streamId](const SnapshotStream & s) {
                return s.snapshotStreamParams.snapshotStreamID == streamId;
            });
            if (found == mSnapshotStreams.end())
            {
                break;
            }
            if (attempts == kMaxSnapshotStreams - 1)
            {
                ChipLogError(Camera, "No available slot for stream allocation");
                return false;
            }
            streamId = static_cast<uint16_t>((streamId + 1) % kMaxSnapshotStreams); // Wraps to 0 after max-1
        }

        outStreamID = streamId;
    }
    else
    {
        // Have a sanity check that the passed streamID does not already exist
        // in the list
        auto found = std::find_if(mSnapshotStreams.begin(), mSnapshotStreams.end(), [outStreamID](const SnapshotStream & s) {
            return s.snapshotStreamParams.snapshotStreamID == outStreamID;
        });

        if (found == mSnapshotStreams.end())
        {
            streamId = outStreamID;
        }
        else
        {
            ChipLogError(Camera, "StreamID %d already exists in the available snapshot stream list", outStreamID);
            return false;
        }
    }

    SnapshotStream snapshotStream = { { streamId, snapshotStreamAllocateArgs.imageCodec, snapshotStreamAllocateArgs.maxFrameRate,
                                        snapshotStreamAllocateArgs.minResolution, snapshotStreamAllocateArgs.maxResolution,
                                        snapshotStreamAllocateArgs.quality, 0 /* RefCount */ },
                                      false,
                                      nullptr };

    mSnapshotStreams.push_back(snapshotStream);
    return true;
}

ChimeDelegate & CameraDevice::GetChimeDelegate()
{
    return mChimeManager;
}

WebRTCTransportProvider::Delegate & CameraDevice::GetWebRTCProviderDelegate()
{
    return mWebRTCProviderManager;
}

void CameraDevice::SetWebRTCTransportProvider(WebRTCTransportProvider::WebRTCTransportProviderCluster * provider)
{
    mWebRTCProviderManager.SetWebRTCTransportProvider(provider);
}

PushAvStreamTransportDelegate & CameraDevice::GetPushAVTransportDelegate()
{
    return mPushAVTransportManager;
}

CameraAVStreamManagementDelegate & CameraDevice::GetCameraAVStreamMgmtDelegate()
{
    return mCameraAVStreamManager;
}

CameraAVStreamController & CameraDevice::GetCameraAVStreamMgmtController()
{
    return mCameraAVStreamManager;
}

CameraAvSettingsUserLevelManagementDelegate & CameraDevice::GetCameraAVSettingsUserLevelMgmtDelegate()
{
    return mCameraAVSettingsUserLevelManager;
}

ZoneManagement::Delegate & CameraDevice::GetZoneManagementDelegate()
{
    return mZoneManager;
}

MediaController & CameraDevice::GetMediaController()
{
    return mMediaController;
}

size_t CameraDevice::GetPreRollBufferSize()
{
    return kMaxContentBufferSizeBytes;
}

int64_t CameraDevice::GetMinKeyframeIntervalMs()
{
    return kKeyFrameIntervalMsec;
}
