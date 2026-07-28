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

#include "webrtc-abstract.h"
#include <Options.h>
#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <lib/support/logging/CHIPLogging.h>
#include <rtc/rtc.hpp>
#include <sys/stat.h>

namespace {

// Constants
constexpr int kVideoH264PayloadType    = 96;
constexpr int kVideoH265PayloadType    = 35; // observed SmartThings offer default for H265/90000
constexpr int kVideoBitRate            = 3000;
constexpr int kSSRC                    = 42;
constexpr int kMaxFragmentSize         = 1188; // 1200 (max packet size) - 12 (RTP header size)
constexpr int kAudioBitRate            = 64000;
constexpr int kOpusPayloadType         = 111;
constexpr int kAudioSSRC               = 43;
constexpr int kAudioPlaybackDestPort   = 6001;
const char * kAudioPlaybackDestAddress = "127.0.0.1"; // Always localhost

rtc::Description::Type SDPTypeToRtcType(SDPType type)
{
    switch (type)
    {
    case SDPType::Offer:
        return rtc::Description::Type::Offer;
    case SDPType::Answer:
        return rtc::Description::Type::Answer;
    case SDPType::Pranswer:
        return rtc::Description::Type::Pranswer;
    case SDPType::Rollback:
        return rtc::Description::Type::Rollback;
    default:
        return rtc::Description::Type::Offer;
    }
}

SDPType RtcTypeToSDPType(rtc::Description::Type type)
{
    switch (type)
    {
    case rtc::Description::Type::Offer:
        return SDPType::Offer;
    case rtc::Description::Type::Answer:
        return SDPType::Answer;
    case rtc::Description::Type::Pranswer:
        return SDPType::Pranswer;
    case rtc::Description::Type::Rollback:
        return SDPType::Rollback;
    default:
        return SDPType::Offer;
    }
}

const char * GetPeerConnectionStateStr(rtc::PeerConnection::State state)
{
    switch (state)
    {
    case rtc::PeerConnection::State::New:
        return "New";

    case rtc::PeerConnection::State::Connecting:
        return "Connecting";

    case rtc::PeerConnection::State::Connected:
        return "Connected";

    case rtc::PeerConnection::State::Disconnected:
        return "Disconnected";

    case rtc::PeerConnection::State::Failed:
        return "Failed";

    case rtc::PeerConnection::State::Closed:
        return "Closed";
    }
    return "N/A";
}

const char * GetGatheringStateStr(rtc::PeerConnection::GatheringState state)
{
    switch (state)
    {
    case rtc::PeerConnection::GatheringState::New:
        return "New";

    case rtc::PeerConnection::GatheringState::InProgress:
        return "InProgress";

    case rtc::PeerConnection::GatheringState::Complete:
        return "Complete";
    }
    return "N/A";
}

class LibDataChannelTrack : public WebRTCTrack
{
public:
    LibDataChannelTrack(std::shared_ptr<rtc::Track> track, int payloadType = -1, const std::string & videoCodec = "H264") :
        mTrack(track), mPayloadType(payloadType), mVideoCodec(videoCodec)
    {}

    ~LibDataChannelTrack()
    {
        if (mAudioRTPSocket != -1)
        {
            close(mAudioRTPSocket);
        }
    }

    // Initialize libdatachannel's RTP packetizer for H.264
    void InitH264Packetizer()
    {
        int payloadType = mPayloadType == -1 ? kVideoH264PayloadType : mPayloadType;
        // 90 kHz clock for H.264
        mRtpCfg = std::make_shared<rtc::RtpPacketizationConfig>(kSSRC, "videosrc", payloadType, rtc::H264RtpPacketizer::ClockRate);
        mRtpCfg->mid = mTrack->description().mid();

        // Setting MTU size to 1200 as default size used in the libdatachannel is 1400
        // Pick separator:
        // - StartSequence : for Annex-B (00 00 01 / 00 00 00 01)
        // - Length : for 4-byte length-prefixed NAL units
        mPacketizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, mRtpCfg, kMaxFragmentSize);

        // RTCP helpers (recommended)
        mSr   = std::make_shared<rtc::RtcpSrReporter>(mRtpCfg);
        mNack = std::make_shared<rtc::RtcpNackResponder>();
        mPacketizer->addToChain(mSr);
        mPacketizer->addToChain(mNack);

        // Attach handler chain to the sending track
        mTrack->setMediaHandler(mPacketizer);
        ChipLogProgress(Camera, "WEBRTC_CODEC: H.264 RTP packetizer initialized (payloadType=%d, mid=%s)", payloadType,
                        mTrack->description().mid().c_str());
    }

    // Initialize libdatachannel's RTP packetizer for H.265. Mirrors InitH264Packetizer exactly:
    // H265RtpPacketizer has an identical constructor signature (same NalUnit::Separator enum,
    // same 90 kHz VideoClockRate) and derives from the same RtpPacketizer base, so the RTCP
    // wiring below is codec-agnostic.
    void InitH265Packetizer()
    {
        int payloadType = mPayloadType == -1 ? kVideoH265PayloadType : mPayloadType;
        mRtpCfg = std::make_shared<rtc::RtpPacketizationConfig>(kSSRC, "videosrc", payloadType, rtc::H265RtpPacketizer::ClockRate);
        mRtpCfg->mid = mTrack->description().mid();

        mPacketizer = std::make_shared<rtc::H265RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, mRtpCfg, kMaxFragmentSize);

        mSr   = std::make_shared<rtc::RtcpSrReporter>(mRtpCfg);
        mNack = std::make_shared<rtc::RtcpNackResponder>();
        mPacketizer->addToChain(mSr);
        mPacketizer->addToChain(mNack);

        mTrack->setMediaHandler(mPacketizer);
        ChipLogProgress(Camera, "WEBRTC_CODEC: H.265 RTP packetizer initialized (payloadType=%d, mid=%s)", payloadType,
                        mTrack->description().mid().c_str());
    }

    // Picks the packetizer matching this track's real stream codec. Both are pure passthrough
    // (raw NAL units in, RTP packets out) — no transcode either way.
    void InitVideoPacketizer()
    {
        if (mVideoCodec == "H265")
        {
            InitH265Packetizer();
        }
        else
        {
            InitH264Packetizer();
        }
    }

    void InitOpusPacketizer()
    {
        int payloadType = mPayloadType == -1 ? kOpusPayloadType : mPayloadType;
        mRtpCfgAudio =
            std::make_shared<rtc::RtpPacketizationConfig>(kAudioSSRC, "mic", payloadType, rtc::OpusRtpPacketizer::DefaultClockRate);

        mRtpCfgAudio->mid = mTrack->description().mid();

        mOpusPacketizer = std::make_shared<rtc::OpusRtpPacketizer>(mRtpCfgAudio);
        mOpusSr         = std::make_shared<rtc::RtcpSrReporter>(mRtpCfgAudio);
        mOpusNack       = std::make_shared<rtc::RtcpNackResponder>();

        if (LinuxDeviceOptions::GetInstance().cameraAudioPlayback)
        {
            // Receiving Session to recv audio from remote peer
            mOpusPacketizer->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
        }

        mOpusPacketizer->addToChain(mOpusSr);
        mOpusPacketizer->addToChain(mOpusNack);

        mTrack->setMediaHandler(mOpusPacketizer);

        // UDP socket to push audio data
        if (LinuxDeviceOptions::GetInstance().cameraAudioPlayback)
        {
            mAudioRTPSocket = socket(AF_INET, SOCK_DGRAM, 0);
            if (mAudioRTPSocket == -1)
            {
                ChipLogError(Camera, "Failed to create RTP Audio socket, Cannot play remote audio: %s", strerror(errno));
                return;
            }
            sockaddr_in audioAddr     = {};
            audioAddr.sin_family      = AF_INET;
            audioAddr.sin_addr.s_addr = inet_addr(kAudioPlaybackDestAddress);
            audioAddr.sin_port        = htons(kAudioPlaybackDestPort);
            mTrack->onMessage(
                [this, audioAddr](const rtc::binary data) {
                    sendto(mAudioRTPSocket, reinterpret_cast<const char *>(data.data()), static_cast<size_t>(data.size()), 0,
                           reinterpret_cast<const struct sockaddr *>(&audioAddr), sizeof(audioAddr));
                },
                nullptr);
        }
    }

    void SendData(const chip::ByteSpan & data) override
    {
        if (!(mTrack && mTrack->isOpen()))
        {
            ChipLogError(Camera, "Track is closed");
            return;
        }
        // The isOpen() check above is racy: SendData runs on the GStreamer streaming thread while
        // libdatachannel closes the track on its own worker thread when the peer disconnects. If the
        // track closes in that window, mTrack->send() throws std::runtime_error("Track is not open").
        // An uncaught throw here aborts the whole process (SIGABRT) and takes the Matter node offline,
        // so swallow it and drop the frame instead.
        try
        {
            const std::string kind = mTrack->description().type();
            if (kind == "video" && !mVideoInitDone)
            {
                InitVideoPacketizer();
                mVideoInitDone = true;
            }
            else if (kind == "audio" && !mAudioInitDone)
            {
                InitOpusPacketizer();
                mAudioInitDone = true;
            }
            // Feed RAW H.264 access unit. Packetizer does NAL split, FU-A/STAP-A, RTP headers, marker bit, SR/NACK.
            TraceH265Video(data);
            rtc::binary frame(data.size());
            std::memcpy(frame.data(), data.data(), data.size());
            mTrack->send(std::move(frame));
        }
        catch (const std::exception & e)
        {
            ChipLogError(Camera, "Track send dropped (peer closing): %s", e.what());
        }
    }

    void SendFrame(const chip::ByteSpan & data, int64_t timestamp) override
    {
        if (!IsReady())
        {
            ChipLogError(Camera, "Track is closed");
            return;
        }
        // See SendData: the IsReady()/isOpen() check is racy against the peer disconnecting on
        // libdatachannel's worker thread, so mTrack->sendFrame() can throw "Track is not open" here.
        // Catch it — an uncaught throw on this (GStreamer) thread aborts the process and drops the node.
        try
        {
            const std::string kind = mTrack->description().type();
            if (kind == "video" && !mVideoInitDone)
            {
                InitVideoPacketizer();
                mVideoInitDone = true;
            }
            else if (kind == "audio" && !mAudioInitDone)
            {
                InitOpusPacketizer();
                mAudioInitDone = true;
            }
            // Feed RAW H.264 access unit. Packetizer does NAL split, FU-A/STAP-A, RTP headers, marker bit, SR/NACK.
            TraceH265Video(data);
            rtc::binary frame(data.size());
            std::memcpy(frame.data(), data.data(), data.size());
            // `timestamp` is in MILLISECONDS (CameraDevice::HandleVideoBufferTimestamp). The
            // FrameInfo(uint32_t) overload takes raw RTP clock ticks — feeding ms there stamps the
            // wire at 1 kHz instead of the negotiated rate (90 kHz video / 48 kHz Opus), compressing
            // stream time ~90×. The chrono overload stores seconds and lets libdatachannel scale by
            // each track's own clockRate (rtppacketizer.cpp secondsToTimestamp), correct for both.
            rtc::FrameInfo info(std::chrono::duration<double, std::milli>(static_cast<double>(timestamp)));
            mTrack->sendFrame(std::move(frame), info);
        }
        catch (const std::exception & e)
        {
            ChipLogError(Camera, "Track sendFrame dropped (peer closing): %s", e.what());
        }
    }

    bool IsReady() override { return mTrack != nullptr && mTrack->isOpen(); }

    std::string GetType() override
    {
        if (mTrack)
        {
            auto description = mTrack->description();
            return std::string(description.type());
        }
        return "";
    }

private:
    // ---- H.265 TX tracing (grep "CAM_H265_TX"): prove the hub emits well-formed H.265 ----
    // Parses the Annex-B access unit handed to the packetizer and logs its NAL breakdown, whether
    // the parameter sets (VPS/SPS/PPS) and a keyframe are present, and how many RTP packets it
    // fragments into. If the trigger file /data/onvif-bridge/h265dump exists, also writes the raw
    // elementary stream to /data/log/h265-tx-<mid>.265 (pull it and decode offline to prove the
    // content itself is valid). H.265 only; logs the first frames of a session + every keyframe.
    static const char * H265NalName(uint8_t t)
    {
        switch (t)
        {
        case 0:
        case 1:
            return "TRAIL";
        case 19:
            return "IDR_W_RADL";
        case 20:
            return "IDR_N_LP";
        case 21:
            return "CRA";
        case 32:
            return "VPS";
        case 33:
            return "SPS";
        case 34:
            return "PPS";
        case 35:
            return "AUD";
        case 39:
        case 40:
            return "SEI";
        default:
            return "NAL";
        }
    }

    void TraceH265Video(const chip::ByteSpan & data)
    {
        if (mVideoCodec != "H265")
            return;
        const uint8_t * p = data.data();
        const size_t n    = data.size();
        const int frameNo = mVideoFrameCount++;
        const std::string mid = mTrack ? std::string(mTrack->description().mid()) : std::string("?");

        auto isStart = [&](size_t k, size_t & scLen) -> bool {
            if (k + 3 <= n && p[k] == 0 && p[k + 1] == 0 && p[k + 2] == 1)
            {
                scLen = 3;
                return true;
            }
            if (k + 4 <= n && p[k] == 0 && p[k + 1] == 0 && p[k + 2] == 0 && p[k + 3] == 1)
            {
                scLen = 4;
                return true;
            }
            return false;
        };

        bool hasVps = false, hasSps = false, hasPps = false, keyframe = false;
        int nalCount = 0, rtpPkts = 0;
        char list[256];
        size_t listLen = 0;
        size_t i       = 0;
        while (i < n)
        {
            size_t scLen = 0;
            if (!isStart(i, scLen))
            {
                i++;
                continue;
            }
            size_t nalStart = i + scLen;
            size_t j        = nalStart;
            while (j < n)
            {
                size_t sc2 = 0;
                if (isStart(j, sc2))
                    break;
                j++;
            }
            size_t nalLen = j - nalStart;
            if (nalLen >= 2)
            {
                uint8_t t = (p[nalStart] >> 1) & 0x3F;
                nalCount++;
                hasVps |= (t == 32);
                hasSps |= (t == 33);
                hasPps |= (t == 34);
                keyframe |= (t >= 16 && t <= 23);
                rtpPkts += (nalLen > (size_t) kMaxFragmentSize)
                    ? (int) ((nalLen + kMaxFragmentSize - 1) / kMaxFragmentSize)
                    : 1;
                if (nalCount <= 12 && listLen + 40 < sizeof(list))
                {
                    int w = std::snprintf(list + listLen, sizeof(list) - listLen, "%s(%u):%zu ", H265NalName(t), t, nalLen);
                    if (w > 0)
                        listLen += (size_t) w;
                }
            }
            i = j;
        }

        if (frameNo < 150 || keyframe)
            // rtpPktsMax = pre-aggregation upper bound (1/small NAL + FU count); the packetizer's
            // AP aggregation (fork change in h265rtppacketizer.cpp) can merge small NALs into
            // fewer actual packets — a whole keyframe often ships as ONE AP packet.
            ChipLogProgress(Camera,
                            "CAM_H265_TX mid=%s frame#%d auSize=%zu nals=%d [%s] KEY=%d vps=%d sps=%d pps=%d ~rtpPktsMax=%d",
                            mid.c_str(), frameNo, n, nalCount, list, keyframe, hasVps, hasSps, hasPps, rtpPkts);

        // Raw elementary-stream dump (offline decode proof) — only if trigger file exists.
        if (frameNo == 0)
        {
            struct stat st;
            if (::stat("/data/onvif-bridge/h265dump", &st) == 0)
            {
                char path[256];
                std::snprintf(path, sizeof(path), "/data/log/h265-tx-%s.265", mid.c_str());
                mH265Dump = std::fopen(path, "wb");
                if (mH265Dump)
                    ChipLogProgress(Camera, "CAM_H265_TX dumping raw elementary stream to %s (cap 8MB)", path);
            }
        }
        if (mH265Dump && mH265DumpBytes < (8u << 20))
        {
            std::fwrite(p, 1, n, mH265Dump);
            std::fflush(mH265Dump);
            mH265DumpBytes += n;
        }
    }

    int mVideoFrameCount    = 0;
    std::FILE * mH265Dump   = nullptr;
    size_t mH265DumpBytes   = 0;

    // Lazy-init state
    bool mAudioInitDone = false;
    bool mVideoInitDone = false;

    std::shared_ptr<rtc::Track> mTrack;
    int mPayloadType;
    int mAudioRTPSocket = -1;
    std::string mVideoCodec; // "H264" or "H265" — selects Init{H264,H265}Packetizer() below

    // For Video. mPacketizer is held as the common RtpPacketizer base so either codec's
    // packetizer (identical construction/RTCP-chain pattern) fits the same member.
    std::shared_ptr<rtc::RtpPacketizationConfig> mRtpCfg;
    std::shared_ptr<rtc::RtpPacketizer> mPacketizer;
    std::shared_ptr<rtc::RtcpSrReporter> mSr;
    std::shared_ptr<rtc::RtcpNackResponder> mNack;

    // For audio
    std::shared_ptr<rtc::RtpPacketizationConfig> mRtpCfgAudio;
    std::shared_ptr<rtc::OpusRtpPacketizer> mOpusPacketizer;
    std::shared_ptr<rtc::RtcpSrReporter> mOpusSr;
    std::shared_ptr<rtc::RtcpNackResponder> mOpusNack;
};

class LibDataChannelPeerConnection : public WebRTCPeerConnection
{
public:
    LibDataChannelPeerConnection(const std::vector<ICEServerInfo> & servers = {})
    {
        rtc::Configuration config;
        for (const auto & server : servers)
        {
            for (const auto & url : server.urls)
            {
                rtc::IceServer iceServer(url);
                iceServer.username = server.username;
                iceServer.password = server.credential;
                config.iceServers.push_back(iceServer);
            }
        }
        mPeerConnection = std::make_shared<rtc::PeerConnection>(config);
    }

    void SetCallbacks(OnLocalDescriptionCallback onLocalDescription, OnICECandidateCallback onICECandidate,
                      OnConnectionStateCallback onConnectionState, OnTrackCallback onTrack) override
    {
        mPeerConnection->onLocalDescription([onLocalDescription, onICECandidate](rtc::Description desc) {
            // First, notify about the local description
            onLocalDescription(std::string(desc), RtcTypeToSDPType(desc.type()));

            // Extract any candidates embedded in the SDP description
            std::vector<rtc::Candidate> candidates = desc.candidates();
            ChipLogProgress(Camera, "Extracted %zu candidates from SDP description", candidates.size());

            for (const auto & candidate : candidates)
            {
                ICECandidateInfo candidateInfo;
                candidateInfo.candidate  = std::string(candidate);
                candidateInfo.mid        = candidate.mid();
                candidateInfo.mlineIndex = -1; // libdatachannel doesn't provide mlineIndex

                ChipLogProgress(Camera, "[From SDP] Candidate: %s, mid: %s", candidateInfo.candidate.c_str(),
                                candidateInfo.mid.c_str());

                onICECandidate(candidateInfo);
            }
        });

        mPeerConnection->onLocalCandidate([onICECandidate](rtc::Candidate candidate) {
            ICECandidateInfo candidateInfo;
            candidateInfo.candidate = std::string(candidate);
            candidateInfo.mid       = candidate.mid();

            // Note: libdatachannel doesn't directly provide mlineIndex, so we use -1 to indicate it is not present.
            candidateInfo.mlineIndex = -1;

            onICECandidate(candidateInfo);
        });

        mPeerConnection->onStateChange([onConnectionState](rtc::PeerConnection::State state) {
            ChipLogProgress(Camera, "[PeerConnection State: %s]", GetPeerConnectionStateStr(state));
            if (state == rtc::PeerConnection::State::Connected)
            {
                onConnectionState(true);
            }
            else if (state == rtc::PeerConnection::State::Failed || state == rtc::PeerConnection::State::Closed)
            {
                // rtc::PeerConnection::State::Disconnected as a terminal state will trigger teardown on transient ICE disconnects;
                // per WebRTC semantics, Disconnected can recover to Connected. Limit the false signal to Failed and Closed only to
                // avoid prematurely ending sessions.
                onConnectionState(false);
            }
        });

        mPeerConnection->onGatheringStateChange([](rtc::PeerConnection::GatheringState state) {
            ChipLogProgress(Camera, "[PeerConnection Gathering State: %s]", GetGatheringStateStr(state));
        });

        mPeerConnection->onTrack(
            [onTrack](std::shared_ptr<rtc::Track> track) { onTrack(std::make_shared<LibDataChannelTrack>(track)); });
    }

    void Close() override
    {
        if (mPeerConnection)
        {
            mPeerConnection->close();
        }
    }

    void CreateOffer() override { mPeerConnection->setLocalDescription(); }

    void CreateAnswer() override { mPeerConnection->createAnswer(); }

    void SetRemoteDescription(const std::string & sdp, SDPType type) override
    {
        rtc::Description::Type rtcType = SDPTypeToRtcType(type);
        mPeerConnection->setRemoteDescription(rtc::Description(sdp, rtcType));
    }

    int GetPayloadType(const std::string & sdp, SDPType type, const std::string & codec) override
    {
        rtc::Description::Type rtcType = SDPTypeToRtcType(type);
        rtc::Description desc(sdp, rtcType);
        for (int mid = 0; mid < desc.mediaCount(); mid++)
        {
            auto media = desc.media(mid);
            if (!std::holds_alternative<rtc::Description::Media *>(media))
                continue;

            rtc::Description::Media * mediaDesc = std::get<rtc::Description::Media *>(media);

            if (mediaDesc == nullptr)
            {
                ChipLogError(Camera, "Media Description is null at index=%d", mid);
                continue;
            }

            for (int pt : mediaDesc->payloadTypes())
            {
                auto * map = mediaDesc->rtpMap(pt);
                if (map == nullptr)
                {
                    ChipLogError(Camera, "No RTP map found for payload type: %d", pt);
                    continue;
                }
                if (map->format == codec)
                {
                    ChipLogProgress(Camera, "WEBRTC_CODEC: codec %s matched an OFFERED payload type: %d", codec.c_str(),
                                    pt);
                    return pt;
                }
            }
        }
        ChipLogError(Camera,
                     "WEBRTC_CODEC: payload type for codec %s NOT present in the peer's SDP — the controller did "
                     "not offer this codec. Falling back to a HARDCODED default PT; the answer will advertise a "
                     "payload type the peer never proposed and the stream may be undecodable.",
                     codec.c_str());
        // Return default values for the supported codec
        if (codec == "H264")
        {
            ChipLogError(Camera, "WEBRTC_CODEC: using FALLBACK H264 pt=%d (not from offer)", kVideoH264PayloadType);
            return kVideoH264PayloadType;
        }
        else if (codec == "H265")
        {
            ChipLogError(Camera, "WEBRTC_CODEC: using FALLBACK H265 pt=%d (not from offer)", kVideoH265PayloadType);
            return kVideoH265PayloadType;
        }
        else if (codec == "opus")
        {
            return kOpusPayloadType;
        }
        return -1;
    }

    void AddRemoteCandidate(const std::string & candidate, const std::string & mid) override
    {
        if (mid.empty())
        {
            mPeerConnection->addRemoteCandidate(rtc::Candidate(candidate));
        }
        else
        {
            mPeerConnection->addRemoteCandidate(rtc::Candidate(candidate, mid));
        }
    }

    std::shared_ptr<WebRTCTrack> AddTrack(MediaType mediaType, const std::string & mid, int payloadType,
                                          const std::string & codec = "H264") override
    {
        if (mediaType == MediaType::Video)
        {
            std::string videoMid = mid.empty() ? "video" : mid;
            rtc::Description::Video vMedia(videoMid, rtc::Description::Direction::SendOnly);
            // H264/H265 passthrough are both pure RTP repacketization (no transcode); only the
            // SDP codec descriptor and RTP packetizer differ.
            if (codec == "H265")
            {
                // Unlike addH264Codec (whose default emits a full fmtp), addH265Codec's default
                // profile is nullopt = NO a=fmtp line at all. SmartThings' Android player matches
                // decoder configs strictly on these params ("Trying to create decoder for
                // unsupported format" with a bare H265), so state the RFC 7798 defaults explicitly
                // — same string a working Samsung S1 camera answers with: Main profile (1), Main
                // tier (0), level 3.1 (93), single-stream mode.
                vMedia.addH265Codec(payloadType, "level-id=93;profile-id=1;tier-flag=0;tx-mode=SRST");
            }
            else
            {
                vMedia.addH264Codec(payloadType);
            }
            ChipLogProgress(Camera, "WEBRTC_CODEC: answer video track mid=%s codec=%s payloadType=%d", videoMid.c_str(),
                            codec.c_str(), payloadType);
            vMedia.setBitrate(kVideoBitRate);
            vMedia.addSSRC(kSSRC, "video-stream", "stream1", "video-stream");
            auto track = mPeerConnection->addTrack(vMedia);
            return std::make_shared<LibDataChannelTrack>(track, payloadType, codec);
        }

        if (mediaType == MediaType::Audio)
        {
            std::string audioMid = mid.empty() ? "audio" : mid;
            rtc::Description::Audio aMedia(audioMid, rtc::Description::Direction::SendOnly);
            aMedia.addOpusCodec(payloadType);
            aMedia.setBitrate(kAudioBitRate);
            aMedia.addSSRC(kAudioSSRC, "audio-stream", "stream1", "audio-stream");
            auto track = mPeerConnection->addTrack(aMedia);
            return std::make_shared<LibDataChannelTrack>(track, payloadType);
        }
        return nullptr;
    }

private:
    std::shared_ptr<rtc::PeerConnection> mPeerConnection;
};

} // namespace

std::shared_ptr<WebRTCPeerConnection> CreateWebRTCPeerConnection(const std::vector<ICEServerInfo> & iceServers)
{
    return std::make_shared<LibDataChannelPeerConnection>(iceServers);
}
