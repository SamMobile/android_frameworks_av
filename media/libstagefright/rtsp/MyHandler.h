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

#ifndef MY_HANDLER_H_

#define MY_HANDLER_H_

#include "APacketSource.h"
#include "ARTPConnection.h"
#include "ARTSPConnection.h"
#include "ASessionDescription.h"

#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaErrors.h>

namespace android {

struct MyHandler : public AHandler {
    MyHandler(const char *url, const sp<ALooper> &looper)
        : mLooper(looper),
          mConn(new ARTSPConnection),
          mRTPConn(new ARTPConnection),
          mSessionURL(url),
          mSetupTracksSuccessful(false),
          mFirstAccessUnit(true),
          mFirstAccessUnitNTP(-1) {
        mLooper->registerHandler(this);
        mLooper->registerHandler(mConn);
        mLooper->registerHandler(mRTPConn);
        sp<AMessage> reply = new AMessage('conn', id());
        mConn->connect(mSessionURL.c_str(), reply);
    }

    virtual void onMessageReceived(const sp<AMessage> &msg) {
        switch (msg->what()) {
            case 'conn':
            {
                int32_t result;
                CHECK(msg->findInt32("result", &result));

                LOG(INFO) << "connection request completed with result "
                     << result << " (" << strerror(-result) << ")";

                if (result == OK) {
                    AString request;
                    request = "DESCRIBE ";
                    request.append(mSessionURL);
                    request.append(" RTSP/1.0\r\n");
                    request.append("Accept: application/sdp\r\n");
                    request.append("\r\n");

                    sp<AMessage> reply = new AMessage('desc', id());
                    mConn->sendRequest(request.c_str(), reply);
                }
                break;
            }

            case 'disc':
            {
                LOG(INFO) << "disconnect completed";

                (new AMessage('quit', id()))->post();
                break;
            }

            case 'desc':
            {
                int32_t result;
                CHECK(msg->findInt32("result", &result));

                LOG(INFO) << "DESCRIBE completed with result "
                     << result << " (" << strerror(-result) << ")";

                if (result == OK) {
                    sp<RefBase> obj;
                    CHECK(msg->findObject("response", &obj));
                    sp<ARTSPResponse> response =
                        static_cast<ARTSPResponse *>(obj.get());

                    if (response->mStatusCode == 302) {
                        ssize_t i = response->mHeaders.indexOfKey("location");
                        CHECK_GE(i, 0);

                        mSessionURL = response->mHeaders.valueAt(i);

                        AString request;
                        request = "DESCRIBE ";
                        request.append(mSessionURL);
                        request.append(" RTSP/1.0\r\n");
                        request.append("Accept: application/sdp\r\n");
                        request.append("\r\n");

                        sp<AMessage> reply = new AMessage('desc', id());
                        mConn->sendRequest(request.c_str(), reply);
                        break;
                    }

                    CHECK_EQ(response->mStatusCode, 200u);

                    mSessionDesc = new ASessionDescription;

                    mSessionDesc->setTo(
                            response->mContent->data(),
                            response->mContent->size());

                    CHECK(mSessionDesc->isValid());

                    ssize_t i = response->mHeaders.indexOfKey("content-base");
                    if (i >= 0) {
                        mBaseURL = response->mHeaders.valueAt(i);
                    } else {
                        i = response->mHeaders.indexOfKey("content-location");
                        if (i >= 0) {
                            mBaseURL = response->mHeaders.valueAt(i);
                        } else {
                            mBaseURL = mSessionURL;
                        }
                    }

                    CHECK_GT(mSessionDesc->countTracks(), 1u);
                    setupTrack(1);
                } else {
                    sp<AMessage> reply = new AMessage('disc', id());
                    mConn->disconnect(reply);
                }
                break;
            }

            case 'setu':
            {
                size_t index;
                CHECK(msg->findSize("index", &index));

                size_t trackIndex;
                CHECK(msg->findSize("track-index", &trackIndex));

                int32_t result;
                CHECK(msg->findInt32("result", &result));

                LOG(INFO) << "SETUP(" << index << ") completed with result "
                     << result << " (" << strerror(-result) << ")";

                TrackInfo *track = &mTracks.editItemAt(trackIndex);

                if (result == OK) {
                    sp<RefBase> obj;
                    CHECK(msg->findObject("response", &obj));
                    sp<ARTSPResponse> response =
                        static_cast<ARTSPResponse *>(obj.get());

                    CHECK_EQ(response->mStatusCode, 200u);

                    ssize_t i = response->mHeaders.indexOfKey("session");
                    CHECK_GE(i, 0);

                    if (index == 1) {
                        mSessionID = response->mHeaders.valueAt(i);
                        i = mSessionID.find(";");
                        if (i >= 0) {
                            // Remove options, i.e. ";timeout=90"
                            mSessionID.erase(i, mSessionID.size() - i);
                        }
                    }

                    sp<AMessage> notify = new AMessage('accu', id());
                    notify->setSize("track-index", trackIndex);

                    mRTPConn->addStream(
                            track->mRTPSocket, track->mRTCPSocket,
                            mSessionDesc, index,
                            notify);

                    track->mPacketSource =
                        new APacketSource(mSessionDesc, index);

                    mSetupTracksSuccessful = true;

                    ++index;
                    if (index < mSessionDesc->countTracks()) {
                        setupTrack(index);
                        break;
                    }
                } else {
                    close(track->mRTPSocket);
                    close(track->mRTCPSocket);

                    mTracks.removeItemsAt(mTracks.size() - 1);
                }

                if (mSetupTracksSuccessful) {
                    AString request = "PLAY ";
                    request.append(mSessionURL);
                    request.append(" RTSP/1.0\r\n");

                    request.append("Session: ");
                    request.append(mSessionID);
                    request.append("\r\n");

                    request.append("\r\n");

                    sp<AMessage> reply = new AMessage('play', id());
                    mConn->sendRequest(request.c_str(), reply);
                } else {
                    sp<AMessage> reply = new AMessage('disc', id());
                    mConn->disconnect(reply);
                }
                break;
            }

            case 'play':
            {
                int32_t result;
                CHECK(msg->findInt32("result", &result));

                LOG(INFO) << "PLAY completed with result "
                     << result << " (" << strerror(-result) << ")";

                if (result == OK) {
                    sp<RefBase> obj;
                    CHECK(msg->findObject("response", &obj));
                    sp<ARTSPResponse> response =
                        static_cast<ARTSPResponse *>(obj.get());

                    CHECK_EQ(response->mStatusCode, 200u);

                    sp<AMessage> msg = new AMessage('abor', id());
                    msg->post(60000000ll);
                } else {
                    sp<AMessage> reply = new AMessage('disc', id());
                    mConn->disconnect(reply);
                }

                break;
            }

            case 'abor':
            {
                for (size_t i = 0; i < mTracks.size(); ++i) {
                    mTracks.editItemAt(i).mPacketSource->signalEOS(
                            ERROR_END_OF_STREAM);
                }

                sp<AMessage> reply = new AMessage('tear', id());

                AString request;
                request = "TEARDOWN ";

                // XXX should use aggregate url from SDP here...
                request.append(mSessionURL);
                request.append(" RTSP/1.0\r\n");

                request.append("Session: ");
                request.append(mSessionID);
                request.append("\r\n");

                request.append("\r\n");

                mConn->sendRequest(request.c_str(), reply);
                break;
            }

            case 'tear':
            {
                int32_t result;
                CHECK(msg->findInt32("result", &result));

                LOG(INFO) << "TEARDOWN completed with result "
                     << result << " (" << strerror(-result) << ")";

                sp<AMessage> reply = new AMessage('disc', id());
                mConn->disconnect(reply);
                break;
            }

            case 'quit':
            {
                mLooper->stop();
                break;
            }

            case 'accu':
            {
                size_t trackIndex;
                CHECK(msg->findSize("track-index", &trackIndex));

                sp<RefBase> obj;
                CHECK(msg->findObject("access-unit", &obj));

                sp<ABuffer> accessUnit = static_cast<ABuffer *>(obj.get());

                uint64_t ntpTime;
                CHECK(accessUnit->meta()->findInt64(
                            "ntp-time", (int64_t *)&ntpTime));

                if (mFirstAccessUnit) {
                    mFirstAccessUnit = false;
                    mFirstAccessUnitNTP = ntpTime;
                }
                if (ntpTime > mFirstAccessUnitNTP) {
                    ntpTime -= mFirstAccessUnitNTP;
                } else {
                    ntpTime = 0;
                }

                accessUnit->meta()->setInt64("ntp-time", ntpTime);

                TrackInfo *track = &mTracks.editItemAt(trackIndex);
                track->mPacketSource->queueAccessUnit(accessUnit);
                break;
            }

            default:
                TRESPASS();
                break;
        }
    }

    sp<APacketSource> getPacketSource(size_t index) {
        CHECK_GE(index, 0u);
        CHECK_LT(index, mTracks.size());

        return mTracks.editItemAt(index).mPacketSource;
    }

    size_t countTracks() const {
        return mTracks.size();
    }

private:
    sp<ALooper> mLooper;
    sp<ARTSPConnection> mConn;
    sp<ARTPConnection> mRTPConn;
    sp<ASessionDescription> mSessionDesc;
    AString mSessionURL;
    AString mBaseURL;
    AString mSessionID;
    bool mSetupTracksSuccessful;
    bool mFirstAccessUnit;
    uint64_t mFirstAccessUnitNTP;

    struct TrackInfo {
        int mRTPSocket;
        int mRTCPSocket;

        sp<APacketSource> mPacketSource;
    };
    Vector<TrackInfo> mTracks;

    void setupTrack(size_t index) {
        AString url;
        CHECK(mSessionDesc->findAttribute(index, "a=control", &url));

        AString trackURL;
        CHECK(MakeURL(mBaseURL.c_str(), url.c_str(), &trackURL));

        mTracks.push(TrackInfo());
        TrackInfo *info = &mTracks.editItemAt(mTracks.size() - 1);

        unsigned rtpPort;
        ARTPConnection::MakePortPair(
                &info->mRTPSocket, &info->mRTCPSocket, &rtpPort);

        AString request = "SETUP ";
        request.append(trackURL);
        request.append(" RTSP/1.0\r\n");

        request.append("Transport: RTP/AVP/UDP;unicast;client_port=");
        request.append(rtpPort);
        request.append("-");
        request.append(rtpPort + 1);
        request.append("\r\n");

        if (index > 1) {
            request.append("Session: ");
            request.append(mSessionID);
            request.append("\r\n");
        }

        request.append("\r\n");

        sp<AMessage> reply = new AMessage('setu', id());
        reply->setSize("index", index);
        reply->setSize("track-index", mTracks.size() - 1);
        mConn->sendRequest(request.c_str(), reply);
    }

    static bool MakeURL(const char *baseURL, const char *url, AString *out) {
        out->clear();

        if (strncasecmp("rtsp://", baseURL, 7)) {
            // Base URL must be absolute
            return false;
        }

        if (!strncasecmp("rtsp://", url, 7)) {
            // "url" is already an absolute URL, ignore base URL.
            out->setTo(url);
            return true;
        }

        size_t n = strlen(baseURL);
        if (baseURL[n - 1] == '/') {
            out->setTo(baseURL);
            out->append(url);
        } else {
            const char *slashPos = strrchr(baseURL, '/');

            if (slashPos > &baseURL[6]) {
                out->setTo(baseURL, slashPos - baseURL);
            } else {
                out->setTo(baseURL);
            }

            out->append("/");
            out->append(url);
        }

        return true;
    }

    DISALLOW_EVIL_CONSTRUCTORS(MyHandler);
};

}  // namespace android

#endif  // MY_HANDLER_H_
