/*
 * Copyright (C) 2011 The Android Open Source Project
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

//#define USE_LOG SLAndroidLogLevel_Verbose

#include "sles_allinclusive.h"
#include "android/android_AudioSfDecoder.h"

#include <media/stagefright/foundation/ADebug.h>


#define SIZE_CACHED_HIGH_BYTES 1000000
#define SIZE_CACHED_MED_BYTES   700000
#define SIZE_CACHED_LOW_BYTES   400000

namespace android {

//--------------------------------------------------------------------------------------------------
AudioSfDecoder::AudioSfDecoder(const AudioPlayback_Parameters* params) : GenericPlayer(params),
        mDataSource(0),
        mAudioSource(0),
        mAudioSourceStarted(false),
        mBitrate(-1),
        mChannelMask(UNKNOWN_CHANNELMASK),
        mDurationUsec(ANDROID_UNKNOWN_TIME),
        mDecodeBuffer(NULL),
        mSeekTimeMsec(0),
        mLastDecodedPositionUs(ANDROID_UNKNOWN_TIME),
        mPcmFormatKeyCount(0)
{
    SL_LOGD("AudioSfDecoder::AudioSfDecoder()");
}


AudioSfDecoder::~AudioSfDecoder() {
    SL_LOGD("AudioSfDecoder::~AudioSfDecoder()");
}


void AudioSfDecoder::preDestroy() {
    GenericPlayer::preDestroy();
    SL_LOGD("AudioSfDecoder::preDestroy()");
    {
        Mutex::Autolock _l(mBufferSourceLock);

        if (NULL != mDecodeBuffer) {
            mDecodeBuffer->release();
            mDecodeBuffer = NULL;
        }

        if ((mAudioSource != 0) && mAudioSourceStarted) {
            mAudioSource->stop();
            mAudioSourceStarted = false;
        }
    }
}


//--------------------------------------------------
void AudioSfDecoder::play() {
    SL_LOGD("AudioSfDecoder::play");

    GenericPlayer::play();
    (new AMessage(kWhatDecode, id()))->post();
}


void AudioSfDecoder::getPositionMsec(int* msec) {
    int64_t timeUsec = getPositionUsec();
    if (timeUsec == ANDROID_UNKNOWN_TIME) {
        *msec = ANDROID_UNKNOWN_TIME;
    } else {
        *msec = timeUsec / 1000;
    }
}


void AudioSfDecoder::startPrefetch_async() {
    SL_LOGV("AudioSfDecoder::startPrefetch_async()");

    if (wantPrefetch()) {
        SL_LOGV("AudioSfDecoder::startPrefetch_async(): sending check cache msg");

        mStateFlags |= kFlagPreparing | kFlagBuffering;

        (new AMessage(kWhatCheckCache, id()))->post();
    }
}


//--------------------------------------------------
uint32_t AudioSfDecoder::getPcmFormatKeyCount() {
    android::Mutex::Autolock autoLock(mPcmFormatLock);
    return mPcmFormatKeyCount;
}


//--------------------------------------------------
bool AudioSfDecoder::getPcmFormatKeySize(uint32_t index, uint32_t* pKeySize) {
    uint32_t keyCount = getPcmFormatKeyCount();
    if (index >= keyCount) {
        return false;
    } else {
        *pKeySize = strlen(kPcmDecodeMetadataKeys[index]) +1;
        return true;
    }
}


//--------------------------------------------------
bool AudioSfDecoder::getPcmFormatKeyName(uint32_t index, uint32_t keySize, char* keyName) {
    uint32_t actualKeySize;
    if (!getPcmFormatKeySize(index, &actualKeySize)) {
        return false;
    }
    if (keySize < actualKeySize) {
        return false;
    }
    strncpy(keyName, kPcmDecodeMetadataKeys[index], actualKeySize);
    return true;
}


//--------------------------------------------------
bool AudioSfDecoder::getPcmFormatValueSize(uint32_t index, uint32_t* pValueSize) {
    uint32_t keyCount = getPcmFormatKeyCount();
    if (index >= keyCount) {
        *pValueSize = 0;
        return false;
    } else {
        *pValueSize = sizeof(uint32_t);
        return true;
    }
}


//--------------------------------------------------
bool AudioSfDecoder::getPcmFormatKeyValue(uint32_t index, uint32_t size, uint32_t* pValue) {
    uint32_t valueSize = 0;
    if (!getPcmFormatValueSize(index, &valueSize)) {
        return false;
    } else if (size != valueSize) {
        // this ensures we are accessing mPcmFormatValues with a valid size for that index
        SL_LOGE("Error retrieving metadata value at index %d: using size of %d, should be %d",
                index, size, valueSize);
        return false;
    } else {
        *pValue = mPcmFormatValues[index];
        return true;
    }
}


//--------------------------------------------------
// Event handlers
//  it is strictly verboten to call those methods outside of the event loop

// Initializes the data and audio sources, and update the PCM format info
// post-condition: upon successful initialization based on the player data locator
//    GenericPlayer::onPrepare() was called
//    mDataSource != 0
//    mAudioSource != 0
//    mAudioSourceStarted == true
// All error returns from this method are via notifyPrepared(status) followed by "return".
void AudioSfDecoder::onPrepare() {
    SL_LOGD("AudioSfDecoder::onPrepare()");
    Mutex::Autolock _l(mBufferSourceLock);

    // Initialize the PCM format info with the known parameters before the start of the decode
    mPcmFormatValues[ANDROID_KEY_INDEX_PCMFORMAT_BITSPERSAMPLE] = SL_PCMSAMPLEFORMAT_FIXED_16;
    mPcmFormatValues[ANDROID_KEY_INDEX_PCMFORMAT_CONTAINERSIZE] = 16;
    mPcmFormatValues[ANDROID_KEY_INDEX_PCMFORMAT_ENDIANNESS] = SL_BYTEORDER_LITTLEENDIAN;
    //    initialization with the default values: they will be replaced by the actual values
    //      once the decoder has figured them out
    mPcmFormatValues[ANDROID_KEY_INDEX_PCMFORMAT_NUMCHANNELS] = mChannelCount;
    mPcmFormatValues[ANDROID_KEY_INDEX_PCMFORMAT_SAMPLESPERSEC] = mSampleRateHz;
    mPcmFormatValues[ANDROID_KEY_INDEX_PCMFORMAT_CHANNELMASK] = mChannelMask;

    //---------------------------------
    // Instantiate and initialize the data source for the decoder
    sp<DataSource> dataSource;

    switch (mDataLocatorType) {

    case kDataLocatorNone:
        SL_LOGE("AudioSfDecoder::onPrepare: no data locator set");
        notifyPrepared(MEDIA_ERROR_BASE);
        return;

    case kDataLocatorUri:
        dataSource = DataSource::CreateFromURI(mDataLocator.uriRef);
        if (dataSource == NULL) {
            SL_LOGE("AudioSfDecoder::onPrepare(): Error opening %s", mDataLocator.uriRef);
            notifyPrepared(MEDIA_ERROR_BASE);
            return;
        }
        break;

    case kDataLocatorFd:
    {
        // As FileSource unconditionally takes ownership of the fd and closes it, then
        // we have to make a dup for FileSource if the app wants to keep ownership itself
        int fd = mDataLocator.fdi.fd;
        if (mDataLocator.fdi.mCloseAfterUse) {
            mDataLocator.fdi.mCloseAfterUse = false;
        } else {
            fd = ::dup(fd);
        }
        dataSource = new FileSource(fd, mDataLocator.fdi.offset, mDataLocator.fdi.length);
        status_t err = dataSource->initCheck();
        if (err != OK) {
            notifyPrepared(err);
            return;
        }
        break;
    }

    default:
        TRESPASS();
    }

    //---------------------------------
    // Instanciate and initialize the decoder attached to the data source
    sp<MediaExtractor> extractor = MediaExtractor::Create(dataSource);
    if (extractor == NULL) {
        SL_LOGE("AudioSfDecoder::onPrepare: Could not instantiate extractor.");
        notifyPrepared(ERROR_UNSUPPORTED);
        return;
    }

    ssize_t audioTrackIndex = -1;
    bool isRawAudio = false;
    for (size_t i = 0; i < extractor->countTracks(); ++i) {
        sp<MetaData> meta = extractor->getTrackMetaData(i);

        const char *mime;
        CHECK(meta->findCString(kKeyMIMEType, &mime));

        if (!strncasecmp("audio/", mime, 6)) {
            audioTrackIndex = i;

            if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_RAW, mime)) {
                isRawAudio = true;
            }
            break;
        }
    }

    if (audioTrackIndex < 0) {
        SL_LOGE("AudioSfDecoder::onPrepare: Could not find a supported audio track.");
        notifyPrepared(ERROR_UNSUPPORTED);
        return;
    }

    sp<MediaSource> source = extractor->getTrack(audioTrackIndex);
    sp<MetaData> meta = source->getFormat();

    // we can't trust the OMXCodec (if there is one) to issue a INFO_FORMAT_CHANGED so we want
    // to have some meaningful values as soon as possible.
    bool hasChannelCount = meta->findInt32(kKeyChannelCount, &mChannelCount);
    int32_t sr;
    bool hasSampleRate = meta->findInt32(kKeySampleRate, &sr);
    if (hasSampleRate) {
        mSampleRateHz = (uint32_t) sr;
    }

    off64_t size;
    int64_t durationUs;
    if (dataSource->getSize(&size) == OK
            && meta->findInt64(kKeyDuration, &durationUs)) {
        if (durationUs != 0) {
            mBitrate = size * 8000000ll / durationUs;  // in bits/sec
        } else {
            mBitrate = -1;
        }
        mDurationUsec = durationUs;
        mDurationMsec = durationUs / 1000;
    } else {
        mBitrate = -1;
        mDurationUsec = ANDROID_UNKNOWN_TIME;
        mDurationMsec = ANDROID_UNKNOWN_TIME;
    }

    // the audio content is not raw PCM, so we need a decoder
    if (!isRawAudio) {
        OMXClient client;
        CHECK_EQ(client.connect(), (status_t)OK);

        source = OMXCodec::Create(
                client.interface(), meta, false /* createEncoder */,
                source);

        if (source == NULL) {
            SL_LOGE("AudioSfDecoder::onPrepare: Could not instantiate decoder.");
            notifyPrepared(ERROR_UNSUPPORTED);
            return;
        }

        meta = source->getFormat();
    }


    if (source->start() != OK) {
        SL_LOGE("AudioSfDecoder::onPrepare: Failed to start source/decoder.");
        notifyPrepared(MEDIA_ERROR_BASE);
        return;
    }

    //---------------------------------
    // The data source, and audio source (a decoder if required) are ready to be used
    mDataSource = dataSource;
    mAudioSource = source;
    mAudioSourceStarted = true;

    if (!hasChannelCount) {
        CHECK(meta->findInt32(kKeyChannelCount, &mChannelCount));
    }

    if (!hasSampleRate) {
        CHECK(meta->findInt32(kKeySampleRate, &sr));
        mSampleRateHz = (uint32_t) sr;
    }
    // FIXME add code below once channel mask support is in, currently initialized to default
    //    if (meta->findInt32(kKeyChannelMask, &mChannelMask)) {
    //        mPcmFormatValues[ANDROID_KEY_INDEX_PCMFORMAT_CHANNELMASK] = mChannelMask;
    //    }

    if (!wantPrefetch()) {
        SL_LOGV("AudioSfDecoder::onPrepare: no need to prefetch");
        // doesn't need prefetching, notify good to go
        mCacheStatus = kStatusHigh;
        mCacheFill = 1000;
        notifyStatus();
        notifyCacheFill();
    }

    {
        android::Mutex::Autolock autoLock(mPcmFormatLock);
        mPcmFormatKeyCount = NB_PCMMETADATA_KEYS;
        mPcmFormatValues[ANDROID_KEY_INDEX_PCMFORMAT_SAMPLESPERSEC] = mSampleRateHz;
        mPcmFormatValues[ANDROID_KEY_INDEX_PCMFORMAT_NUMCHANNELS] = mChannelCount;
    }

    // at this point we have enough information about the source to create the sink that
    // will consume the data
    createAudioSink();

    // signal successful completion of prepare
    mStateFlags |= kFlagPrepared;

    GenericPlayer::onPrepare();
    SL_LOGD("AudioSfDecoder::onPrepare() done, mStateFlags=0x%x", mStateFlags);
}


void AudioSfDecoder::onPause() {
    SL_LOGV("AudioSfDecoder::onPause()");
    GenericPlayer::onPause();
    pauseAudioSink();
}


void AudioSfDecoder::onPlay() {
    SL_LOGV("AudioSfDecoder::onPlay()");
    GenericPlayer::onPlay();
    startAudioSink();
}


void AudioSfDecoder::onSeek(const sp<AMessage> &msg) {
    SL_LOGV("AudioSfDecoder::onSeek");
    int64_t timeMsec;
    CHECK(msg->findInt64(WHATPARAM_SEEK_SEEKTIME_MS, &timeMsec));

    Mutex::Autolock _l(mTimeLock);
    mStateFlags |= kFlagSeeking;
    mSeekTimeMsec = timeMsec;
    mLastDecodedPositionUs = ANDROID_UNKNOWN_TIME;
}


void AudioSfDecoder::onLoop(const sp<AMessage> &msg) {
    SL_LOGV("AudioSfDecoder::onLoop");
    int32_t loop;
    CHECK(msg->findInt32(WHATPARAM_LOOP_LOOPING, &loop));

    if (loop) {
        //SL_LOGV("AudioSfDecoder::onLoop start looping");
        mStateFlags |= kFlagLooping;
    } else {
        //SL_LOGV("AudioSfDecoder::onLoop stop looping");
        mStateFlags &= ~kFlagLooping;
    }
}


void AudioSfDecoder::onCheckCache(const sp<AMessage> &msg) {
    //SL_LOGV("AudioSfDecoder::onCheckCache");
    bool eos;
    CacheStatus_t status = getCacheRemaining(&eos);

    if (eos || status == kStatusHigh
            || ((mStateFlags & kFlagPreparing) && (status >= kStatusEnough))) {
        if (mStateFlags & kFlagPlaying) {
            startAudioSink();
        }
        mStateFlags &= ~kFlagBuffering;

        SL_LOGV("AudioSfDecoder::onCheckCache: buffering done.");

        if (mStateFlags & kFlagPreparing) {
            //SL_LOGV("AudioSfDecoder::onCheckCache: preparation done.");
            mStateFlags &= ~kFlagPreparing;
        }

        if (mStateFlags & kFlagPlaying) {
            (new AMessage(kWhatDecode, id()))->post();
        }
        return;
    }

    msg->post(100000);
}


void AudioSfDecoder::onDecode() {
    SL_LOGV("AudioSfDecoder::onDecode");

    //-------------------------------- Need to buffer some more before decoding?
    bool eos;
    if (mDataSource == 0) {
        // application set play state to paused which failed, then set play state to playing
        return;
    }

    if (wantPrefetch()
            && (getCacheRemaining(&eos) == kStatusLow)
            && !eos) {
        SL_LOGV("buffering more.");

        if (mStateFlags & kFlagPlaying) {
            pauseAudioSink();
        }
        mStateFlags |= kFlagBuffering;
        (new AMessage(kWhatCheckCache, id()))->post(100000);
        return;
    }

    if (!(mStateFlags & (kFlagPlaying | kFlagBuffering | kFlagPreparing))) {
        // don't decode if we're not buffering, prefetching or playing
        //SL_LOGV("don't decode: not buffering, prefetching or playing");
        return;
    }

    //-------------------------------- Decode
    status_t err;
    MediaSource::ReadOptions readOptions;
    if (mStateFlags & kFlagSeeking) {
        assert(mSeekTimeMsec != ANDROID_UNKNOWN_TIME);
        readOptions.setSeekTo(mSeekTimeMsec * 1000);
    }

    int64_t timeUsec = ANDROID_UNKNOWN_TIME;
    {
        Mutex::Autolock _l(mBufferSourceLock);

        if (NULL != mDecodeBuffer) {
            // the current decoded buffer hasn't been rendered, drop it
            mDecodeBuffer->release();
            mDecodeBuffer = NULL;
        }
        if(!mAudioSourceStarted) {
            return;
        }
        err = mAudioSource->read(&mDecodeBuffer, &readOptions);
        if (err == OK) {
            CHECK(mDecodeBuffer->meta_data()->findInt64(kKeyTime, &timeUsec));
        }
    }

    {
        Mutex::Autolock _l(mTimeLock);
        if (mStateFlags & kFlagSeeking) {
            mStateFlags &= ~kFlagSeeking;
            mSeekTimeMsec = ANDROID_UNKNOWN_TIME;
        }
        if (timeUsec != ANDROID_UNKNOWN_TIME) {
            mLastDecodedPositionUs = timeUsec;
        }
    }

    //-------------------------------- Handle return of decode
    if (err != OK) {
        bool continueDecoding = false;
        switch(err) {
            case ERROR_END_OF_STREAM:
                if (0 < mDurationUsec) {
                    Mutex::Autolock _l(mTimeLock);
                    mLastDecodedPositionUs = mDurationUsec;
                }
                // handle notification and looping at end of stream
                if (mStateFlags & kFlagPlaying) {
                    notify(PLAYEREVENT_ENDOFSTREAM, 1, true);
                }
                if (mStateFlags & kFlagLooping) {
                    seek(0);
                    // kick-off decoding again
                    continueDecoding = true;
                }
                break;
            case INFO_FORMAT_CHANGED:
                SL_LOGD("MediaSource::read encountered INFO_FORMAT_CHANGED");
                // reconfigure output
                {
                    Mutex::Autolock _l(mBufferSourceLock);
                    hasNewDecodeParams();
                }
                continueDecoding = true;
                break;
            case INFO_DISCONTINUITY:
                SL_LOGD("MediaSource::read encountered INFO_DISCONTINUITY");
                continueDecoding = true;
                break;
            default:
                SL_LOGE("MediaSource::read returned error %d", err);
                break;
        }
        if (continueDecoding) {
            if (NULL == mDecodeBuffer) {
                (new AMessage(kWhatDecode, id()))->post();
                return;
            }
        } else {
            return;
        }
    }

    //-------------------------------- Render
    sp<AMessage> msg = new AMessage(kWhatRender, id());
    msg->post();
}


void AudioSfDecoder::onRender() {
    //SL_LOGV("AudioSfDecoder::onRender");

    Mutex::Autolock _l(mBufferSourceLock);

    if (NULL == mDecodeBuffer) {
        // nothing to render, move along
        SL_LOGV("AudioSfDecoder::onRender NULL buffer, exiting");
        return;
    }

    mDecodeBuffer->release();
    mDecodeBuffer = NULL;

}


void AudioSfDecoder::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatPrepare:
            onPrepare();
            break;

        case kWhatDecode:
            onDecode();
            break;

        case kWhatRender:
            onRender();
            break;

        case kWhatCheckCache:
            onCheckCache(msg);
            break;

        case kWhatNotif:
            onNotify(msg);
            break;

        case kWhatPlay:
            onPlay();
            break;

        case kWhatPause:
            onPause();
            break;

/*
        case kWhatSeek:
            onSeek(msg);
            break;

        case kWhatLoop:
            onLoop(msg);
            break;
*/
        default:
            GenericPlayer::onMessageReceived(msg);
            break;
    }
}

//--------------------------------------------------
// Prepared state, prefetch status notifications
void AudioSfDecoder::notifyPrepared(status_t prepareRes) {
    assert(!(mStateFlags & (kFlagPrepared | kFlagPreparedUnsuccessfully)));
    if (NO_ERROR == prepareRes) {
        // The "then" fork is not currently used, but is kept here to make it easier
        // to replace by a new signalPrepareCompletion(status) if we re-visit this later.
        mStateFlags |= kFlagPrepared;
    } else {
        mStateFlags |= kFlagPreparedUnsuccessfully;
    }
    // Do not call the superclass onPrepare to notify, because it uses a default error
    // status code but we can provide a more specific one.
    // GenericPlayer::onPrepare();
    notify(PLAYEREVENT_PREPARED, (int32_t)prepareRes, true);
    SL_LOGD("AudioSfDecoder::onPrepare() done, mStateFlags=0x%x", mStateFlags);
}


void AudioSfDecoder::onNotify(const sp<AMessage> &msg) {
    notif_cbf_t notifyClient;
    void*       notifyUser;
    {
        android::Mutex::Autolock autoLock(mNotifyClientLock);
        if (NULL == mNotifyClient) {
            return;
        } else {
            notifyClient = mNotifyClient;
            notifyUser   = mNotifyUser;
        }
    }
    int32_t val;
    if (msg->findInt32(PLAYEREVENT_PREFETCHSTATUSCHANGE, &val)) {
        SL_LOGV("\tASfPlayer notifying %s = %d", PLAYEREVENT_PREFETCHSTATUSCHANGE, val);
        notifyClient(kEventPrefetchStatusChange, val, 0, notifyUser);
    }
    else if (msg->findInt32(PLAYEREVENT_PREFETCHFILLLEVELUPDATE, &val)) {
        SL_LOGV("\tASfPlayer notifying %s = %d", PLAYEREVENT_PREFETCHFILLLEVELUPDATE, val);
        notifyClient(kEventPrefetchFillLevelUpdate, val, 0, notifyUser);
    }
    else if (msg->findInt32(PLAYEREVENT_ENDOFSTREAM, &val)) {
        SL_LOGV("\tASfPlayer notifying %s = %d", PLAYEREVENT_ENDOFSTREAM, val);
        notifyClient(kEventEndOfStream, val, 0, notifyUser);
    }
    else {
        GenericPlayer::onNotify(msg);
    }
}


//--------------------------------------------------
// Private utility functions

bool AudioSfDecoder::wantPrefetch() {
    if (mDataSource != 0) {
        return (mDataSource->flags() & DataSource::kWantsPrefetching);
    } else {
        // happens if an improper data locator was passed, if the media extractor couldn't be
        //  initialized, if there is no audio track in the media, if the OMX decoder couldn't be
        //  instantiated, if the source couldn't be opened, or if the MediaSource
        //  couldn't be started
        SL_LOGV("AudioSfDecoder::wantPrefetch() tries to access NULL mDataSource");
        return false;
    }
}


int64_t AudioSfDecoder::getPositionUsec() {
    Mutex::Autolock _l(mTimeLock);
    if (mStateFlags & kFlagSeeking) {
        return mSeekTimeMsec * 1000;
    } else {
        if (mLastDecodedPositionUs < 0) {
            return ANDROID_UNKNOWN_TIME;
        } else {
            return mLastDecodedPositionUs;
        }
    }
}


CacheStatus_t AudioSfDecoder::getCacheRemaining(bool *eos) {
    sp<NuCachedSource2> cachedSource =
        static_cast<NuCachedSource2 *>(mDataSource.get());

    CacheStatus_t oldStatus = mCacheStatus;

    status_t finalStatus;
    size_t dataRemaining = cachedSource->approxDataRemaining(&finalStatus);
    *eos = (finalStatus != OK);

    CHECK_GE(mBitrate, 0);

    int64_t dataRemainingUs = dataRemaining * 8000000ll / mBitrate;
    //SL_LOGV("AudioSfDecoder::getCacheRemaining: approx %.2f secs remaining (eos=%d)",
    //       dataRemainingUs / 1E6, *eos);

    if (*eos) {
        // data is buffered up to the end of the stream, it can't get any better than this
        mCacheStatus = kStatusHigh;
        mCacheFill = 1000;

    } else {
        if (mDurationUsec > 0) {
            // known duration:

            //   fill level is ratio of how much has been played + how much is
            //   cached, divided by total duration
            uint32_t currentPositionUsec = getPositionUsec();
            if (currentPositionUsec == ANDROID_UNKNOWN_TIME) {
                // if we don't know where we are, assume the worst for the fill ratio
                currentPositionUsec = 0;
            }
            if (mDurationUsec > 0) {
                mCacheFill = (int16_t) ((1000.0
                        * (double)(currentPositionUsec + dataRemainingUs) / mDurationUsec));
            } else {
                mCacheFill = 0;
            }
            //SL_LOGV("cacheFill = %d", mCacheFill);

            //   cache status is evaluated against duration thresholds
            if (dataRemainingUs > DURATION_CACHED_HIGH_MS*1000) {
                mCacheStatus = kStatusHigh;
                //LOGV("high");
            } else if (dataRemainingUs > DURATION_CACHED_MED_MS*1000) {
                //LOGV("enough");
                mCacheStatus = kStatusEnough;
            } else if (dataRemainingUs < DURATION_CACHED_LOW_MS*1000) {
                //LOGV("low");
                mCacheStatus = kStatusLow;
            } else {
                mCacheStatus = kStatusIntermediate;
            }

        } else {
            // unknown duration:

            //   cache status is evaluated against cache amount thresholds
            //   (no duration so we don't have the bitrate either, could be derived from format?)
            if (dataRemaining > SIZE_CACHED_HIGH_BYTES) {
                mCacheStatus = kStatusHigh;
            } else if (dataRemaining > SIZE_CACHED_MED_BYTES) {
                mCacheStatus = kStatusEnough;
            } else if (dataRemaining < SIZE_CACHED_LOW_BYTES) {
                mCacheStatus = kStatusLow;
            } else {
                mCacheStatus = kStatusIntermediate;
            }
        }

    }

    if (oldStatus != mCacheStatus) {
        notifyStatus();
    }

    if (abs(mCacheFill - mLastNotifiedCacheFill) > mCacheFillNotifThreshold) {
        notifyCacheFill();
    }

    return mCacheStatus;
}


void AudioSfDecoder::hasNewDecodeParams() {

    if ((mAudioSource != 0) && mAudioSourceStarted) {
        sp<MetaData> meta = mAudioSource->getFormat();

        SL_LOGV("old sample rate = %d, channel count = %d", mSampleRateHz, mChannelCount);

        CHECK(meta->findInt32(kKeyChannelCount, &mChannelCount));
        int32_t sr;
        CHECK(meta->findInt32(kKeySampleRate, &sr));
        mSampleRateHz = (uint32_t) sr;
        SL_LOGV("format changed: new sample rate = %d, channel count = %d",
                mSampleRateHz, mChannelCount);

        {
            android::Mutex::Autolock autoLock(mPcmFormatLock);
            mPcmFormatValues[ANDROID_KEY_INDEX_PCMFORMAT_NUMCHANNELS] = mChannelCount;
            mPcmFormatValues[ANDROID_KEY_INDEX_PCMFORMAT_SAMPLESPERSEC] = mSampleRateHz;
        }
    }

    // alert users of those params
    updateAudioSink();
}

} // namespace android
