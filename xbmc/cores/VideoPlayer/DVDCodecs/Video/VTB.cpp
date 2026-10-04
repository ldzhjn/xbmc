/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VTB.h"

#include "DVDCodecs/DVDCodecUtils.h"
#include "DVDCodecs/DVDFactoryCodec.h"
#include "DVDVideoCodec.h"
#include "ServiceBroker.h"
#include "cores/VideoPlayer/Process/ProcessInfo.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/log.h"
#if defined(TARGET_DARWIN_TVOS)
#include "windowing/tvos/WinSystemTVOS.h"
#endif

#include <mutex>

extern "C" {
#include <libavcodec/videotoolbox.h>
}

using namespace VTB;

//------------------------------------------------------------------------------
// Video Buffers
//------------------------------------------------------------------------------

CVideoBufferVTB::CVideoBufferVTB(IVideoBufferPool &pool, int id)
: CVideoBuffer(id)
{
  m_pFrame = av_frame_alloc();
}

CVideoBufferVTB::~CVideoBufferVTB()
{
  av_frame_free(&m_pFrame);
}

void CVideoBufferVTB::SetRef(AVFrame *frame)
{
  av_frame_unref(m_pFrame);
  av_frame_ref(m_pFrame, frame);
  m_pbRef = (CVPixelBufferRef)m_pFrame->data[3];
}

void CVideoBufferVTB::Unref()
{
  av_frame_unref(m_pFrame);
}

CVPixelBufferRef CVideoBufferVTB::GetPB()
{
  return m_pbRef;
}

//------------------------------------------------------------------------------

class VTB::CVideoBufferPoolVTB : public IVideoBufferPool
{
public:
  ~CVideoBufferPoolVTB() override;
  void Return(int id) override;
  CVideoBuffer* Get() override;

protected:
  CCriticalSection m_critSection;
  std::vector<CVideoBufferVTB*> m_all;
  std::deque<int> m_used;
  std::deque<int> m_free;
};

CVideoBufferPoolVTB::~CVideoBufferPoolVTB()
{
  for (auto buf : m_all)
  {
    delete buf;
  }
}

CVideoBuffer* CVideoBufferPoolVTB::Get()
{
  std::unique_lock lock(m_critSection);

  CVideoBufferVTB *buf = nullptr;
  if (!m_free.empty())
  {
    int idx = m_free.front();
    m_free.pop_front();
    m_used.push_back(idx);
    buf = m_all[idx];
  }
  else
  {
    int id = m_all.size();
    buf = new CVideoBufferVTB(*this, id);
    m_all.push_back(buf);
    m_used.push_back(id);
  }

  buf->Acquire(GetPtr());
  return buf;
}

void CVideoBufferPoolVTB::Return(int id)
{
  std::unique_lock lock(m_critSection);

  m_all[id]->Unref();
  auto it = m_used.begin();
  while (it != m_used.end())
  {
    if (*it == id)
    {
      m_used.erase(it);
      break;
    }
    else
      ++it;
  }
  m_free.push_back(id);
}

#if defined(TARGET_DARWIN_TVOS)
CSoftwareHDR::CSoftwareHDR()
{
  auto* winSystem = dynamic_cast<CWinSystemTVOS*>(CServiceBroker::GetWinSystem());
  m_enabled = winSystem && winSystem->CanUseHDRVideoLayer() && winSystem->IsHDRDisplay();
  m_videoBufferPool = std::make_shared<CVideoBufferPoolVTB>();
}

CSoftwareHDR::~CSoftwareHDR()
{
  sws_freeContext(m_swsContext);
  if (m_pixelBufferPool)
    CVPixelBufferPoolRelease(m_pixelBufferPool);
}

bool CSoftwareHDR::CanConvert(const AVFrame* frame, const CDVDStreamInfo& hints) const
{
  if (!m_enabled || !frame || !frame->data[0] || (frame->flags & AV_FRAME_FLAG_INTERLACED))
    return false;

  if (frame->format != AV_PIX_FMT_YUV420P10LE && frame->format != AV_PIX_FMT_P010LE)
    return false;

  const auto primaries = frame->color_primaries == AVCOL_PRI_UNSPECIFIED
                             ? hints.colorPrimaries : frame->color_primaries;
  const auto transfer = frame->color_trc == AVCOL_TRC_UNSPECIFIED
                            ? hints.colorTransferCharacteristic : frame->color_trc;
  return primaries == AVCOL_PRI_BT2020 &&
         (transfer == AVCOL_TRC_SMPTE2084 || transfer == AVCOL_TRC_ARIB_STD_B67) &&
         hints.dovi.dv_profile == 0;
}

CVideoBuffer* CSoftwareHDR::Convert(const AVFrame* frame)
{
  if (!m_pixelBufferPool || frame->width != m_width || frame->height != m_height)
  {
    if (m_pixelBufferPool)
    {
      CVPixelBufferPoolRelease(m_pixelBufferPool);
      m_pixelBufferPool = nullptr;
    }
    const int pixelFormat = kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange;
    CFNumberRef width = CFNumberCreate(nullptr, kCFNumberIntType, &frame->width);
    CFNumberRef height = CFNumberCreate(nullptr, kCFNumberIntType, &frame->height);
    CFNumberRef format = CFNumberCreate(nullptr, kCFNumberIntType, &pixelFormat);
    CFDictionaryRef surface = CFDictionaryCreate(nullptr, nullptr, nullptr, 0,
                                                 &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks);
    const void* keys[] = {kCVPixelBufferWidthKey, kCVPixelBufferHeightKey,
                          kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferIOSurfacePropertiesKey};
    const void* values[] = {width, height, format, surface};
    CFDictionaryRef attributes = CFDictionaryCreate(nullptr, keys, values, 4,
                                                    &kCFTypeDictionaryKeyCallBacks,
                                                    &kCFTypeDictionaryValueCallBacks);
    const CVReturn status = CVPixelBufferPoolCreate(nullptr, nullptr, attributes, &m_pixelBufferPool);
    CFRelease(attributes);
    CFRelease(surface);
    CFRelease(format);
    CFRelease(height);
    CFRelease(width);
    if (status != kCVReturnSuccess)
      return nullptr;
    m_width = frame->width;
    m_height = frame->height;
  }

  m_swsContext = sws_getCachedContext(m_swsContext, frame->width, frame->height,
                                    static_cast<AVPixelFormat>(frame->format),
                                    frame->width, frame->height, AV_PIX_FMT_P010LE,
                                    SWS_POINT, nullptr, nullptr, nullptr);
  if (!m_swsContext)
    return nullptr;
  const int* coefficients = sws_getCoefficients(SWS_CS_BT2020);
  if (sws_setColorspaceDetails(m_swsContext, coefficients,
                              frame->color_range == AVCOL_RANGE_JPEG,
                              coefficients, 0, 0, 1 << 16, 1 << 16) < 0)
    return nullptr;

  CVPixelBufferRef pixelBuffer = nullptr;
  if (CVPixelBufferPoolCreatePixelBuffer(nullptr, m_pixelBufferPool, &pixelBuffer) != kCVReturnSuccess)
    return nullptr;
  if (CVPixelBufferLockBaseAddress(pixelBuffer, 0) != kCVReturnSuccess)
  {
    CVPixelBufferRelease(pixelBuffer);
    return nullptr;
  }
  uint8_t* planes[4] = {
      static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0)),
      static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1)), nullptr, nullptr};
  int strides[4] = {static_cast<int>(CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0)),
                    static_cast<int>(CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1)), 0, 0};
  const int rows = sws_scale(m_swsContext, frame->data, frame->linesize, 0, frame->height,
                           planes, strides);
  CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
  if (rows != frame->height)
  {
    CVPixelBufferRelease(pixelBuffer);
    return nullptr;
  }

  AVFrame* output = av_frame_alloc();
  if (!output)
  {
    CVPixelBufferRelease(pixelBuffer);
    return nullptr;
  }
  output->buf[0] = av_buffer_create(
      reinterpret_cast<uint8_t*>(pixelBuffer), 1,
      [](void* opaque, uint8_t*) { CVPixelBufferRelease(static_cast<CVPixelBufferRef>(opaque)); },
      pixelBuffer, 0);
  if (!output->buf[0])
  {
    CVPixelBufferRelease(pixelBuffer);
    av_frame_free(&output);
    return nullptr;
  }
  output->data[3] = reinterpret_cast<uint8_t*>(pixelBuffer);
  output->format = AV_PIX_FMT_VIDEOTOOLBOX;
  output->width = frame->width;
  output->height = frame->height;
  auto* buffer = static_cast<CVideoBufferVTB*>(m_videoBufferPool->Get());
  buffer->SetRef(output);
  av_frame_free(&output);
  if (!m_loggedFirstPicture)
  {
    CLog::Log(LOGINFO, "VTB::SoftwareHDR: uploading {}x{} 10-bit frames to HDR video layer",
              frame->width, frame->height);
    m_loggedFirstPicture = true;
  }
  return buffer;
}
#endif

//------------------------------------------------------------------------------
// main class
//------------------------------------------------------------------------------

IHardwareDecoder* CDecoder::Create(CDVDStreamInfo &hint, CProcessInfo &processInfo, AVPixelFormat fmt)
{
#if defined(TARGET_DARWIN_EMBEDDED)
  // force disable HW acceleration for live streams
  // to avoid absent image issue on interlaced videos
  if (processInfo.IsRealtimeStream() && hint.interlaced)
    return nullptr;
#endif

  if (fmt == AV_PIX_FMT_VIDEOTOOLBOX && CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_VIDEOPLAYER_USEVTB))
  {
    bool hdrOutput = false;
#if defined(TARGET_DARWIN_TVOS)
    auto* winSystem = dynamic_cast<CWinSystemTVOS*>(CServiceBroker::GetWinSystem());
    if (winSystem)
    {
      const bool canUseHDRVideoLayer = winSystem->CanUseHDRVideoLayer();
      const CHDRCapabilities caps = winSystem->GetDisplayHDRCapabilities();
      // InputStream addons can report the HDR transfer without filling in
      // CDVDStreamInfo::bitdepth. The decoder negotiates the actual P010 format.
      // Dolby Vision profile 8 can carry an HDR10 or HLG compatible base layer.
      // Use only that base layer; other Dolby Vision profiles need their own path.
      const bool hdr10BaseLayer = hint.dovi.dv_profile == 8 &&
                                  hint.dovi.dv_bl_signal_compatibility_id == 1;
      const bool hlgBaseLayer = hint.dovi.dv_profile == 8 &&
                                hint.dovi.dv_bl_signal_compatibility_id == 4;
      hdrOutput = canUseHDRVideoLayer &&
                  (((hint.hdrType == StreamHdrType::HDR_TYPE_HDR10 || hdr10BaseLayer) &&
                    caps.SupportsHDR10()) ||
                   ((hint.hdrType == StreamHdrType::HDR_TYPE_HLG || hlgBaseLayer) &&
                    (caps.SupportsHLG() || caps.SupportsHDR10())));
      CLog::Log(LOGDEBUG,
                "VTB::Create: HDR video layer {}, type {}, bitdepth {}, transfer {}, HDR10 {}, "
                "HLG {}, display matching {}, DV profile {}, base compatibility {}",
                hdrOutput ? "selected" : "skipped", static_cast<int>(hint.hdrType),
                hint.bitdepth, static_cast<int>(hint.colorTransferCharacteristic),
                caps.SupportsHDR10(), caps.SupportsHLG(), canUseHDRVideoLayer,
                hint.dovi.dv_profile, hint.dovi.dv_bl_signal_compatibility_id);
    }
#endif
    return new VTB::CDecoder(processInfo, hdrOutput, hint.dovi.dv_profile == 0);
  }

  return nullptr;
}

bool CDecoder::Register()
{
  CDVDFactoryCodec::RegisterHWAccel("vtb", CDecoder::Create);
  return true;
}

CDecoder::CDecoder(CProcessInfo& processInfo,
                   bool hdrOutput,
                   bool allowColorTransferFallback)
  : m_processInfo(processInfo),
    m_hdrOutput(hdrOutput),
    m_allowColorTransferFallback(allowColorTransferFallback),
    m_videoBufferPool(std::make_shared<CVideoBufferPoolVTB>())
{
  m_avctx = nullptr;
}

CDecoder::~CDecoder()
{
  if (m_renderBuffer)
    m_renderBuffer->Release();
  Close();
}

void CDecoder::Close()
{

}

bool CDecoder::Open(AVCodecContext *avctx, AVCodecContext* mainctx, enum AVPixelFormat fmt)
{
  if (!CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_VIDEOPLAYER_USEVTB))
    return false;

  CLog::Log(LOGDEBUG, "VTB::Open: transfer {}, primaries {}, bits {}, pixel format {}",
            static_cast<int>(avctx->color_trc), static_cast<int>(avctx->color_primaries),
            avctx->bits_per_raw_sample, static_cast<int>(avctx->sw_pix_fmt));

#if defined(TARGET_DARWIN_TVOS)
  if (!m_hdrOutput && m_allowColorTransferFallback &&
      avctx->color_primaries == AVCOL_PRI_BT2020)
  {
    auto* winSystem = dynamic_cast<CWinSystemTVOS*>(CServiceBroker::GetWinSystem());
    if (winSystem && winSystem->CanUseHDRVideoLayer())
    {
      const CHDRCapabilities caps = winSystem->GetDisplayHDRCapabilities();
      m_hdrOutput = (avctx->color_trc == AVCOL_TRC_SMPTE2084 && caps.SupportsHDR10()) ||
                    (avctx->color_trc == AVCOL_TRC_ARIB_STD_B67 &&
                     (caps.SupportsHLG() || caps.SupportsHDR10()));
    }
  }
  CLog::Log(LOGDEBUG, "VTB::Open: HDR video layer {}",
            m_hdrOutput ? "selected" : "skipped");
#endif

  AVBufferRef *deviceRef =  av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VIDEOTOOLBOX);
  AVBufferRef *framesRef = av_hwframe_ctx_alloc(deviceRef);
  AVHWFramesContext *framesCtx = (AVHWFramesContext*)framesRef->data;
  framesCtx->format = AV_PIX_FMT_VIDEOTOOLBOX;
  // The tvOS HDR video layer consumes P010 directly. The GLES renderer cannot
  // preserve this precision, so all other playback keeps the existing NV12 path.
  framesCtx->sw_format = m_hdrOutput ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
  avctx->hw_frames_ctx = framesRef;
  m_avctx = avctx;

  m_processInfo.SetVideoDeintMethod("none");

  std::list<EINTERLACEMETHOD> deintMethods;
  deintMethods.push_back(EINTERLACEMETHOD::VS_INTERLACEMETHOD_NONE);
  m_processInfo.UpdateDeinterlacingMethods(deintMethods);

  return true;
}

CDVDVideoCodec::VCReturn CDecoder::Decode(AVCodecContext* avctx, AVFrame* frame)
{
  CDVDVideoCodec::VCReturn status = Check(avctx);
  if(status)
    return status;

  if(frame)
  {
    if (frame->flags & AV_FRAME_FLAG_INTERLACED)
      return CDVDVideoCodec::VC_FATAL;

    if (m_renderBuffer)
      m_renderBuffer->Release();
    m_renderBuffer = dynamic_cast<CVideoBufferVTB*>(m_videoBufferPool->Get());
    m_renderBuffer->SetRef(frame);
    return CDVDVideoCodec::VC_PICTURE;
  }
  else
    return CDVDVideoCodec::VC_BUFFER;
}

bool CDecoder::GetPicture(AVCodecContext* avctx, VideoPicture* picture)
{
  ((ICallbackHWAccel*)avctx->opaque)->GetPictureCommon(picture);

  if (!m_loggedFirstPicture)
  {
    CLog::Log(LOGDEBUG, "VTB::GetPicture: type {}, transfer {}, primaries {}, bits {}, pixel buffer {}",
              static_cast<int>(picture->hdrType), static_cast<int>(picture->color_transfer),
              static_cast<int>(picture->color_primaries), picture->colorBits,
              m_renderBuffer && m_renderBuffer->GetPB()
                  ? CVPixelBufferGetPixelFormatType(m_renderBuffer->GetPB())
                  : 0);
    m_loggedFirstPicture = true;
  }

  if (picture->videoBuffer)
    picture->videoBuffer->Release();

  picture->videoBuffer = m_renderBuffer;
  picture->videoBuffer->Acquire();
  return true;
}

CDVDVideoCodec::VCReturn CDecoder::Check(AVCodecContext* avctx)
{
  return CDVDVideoCodec::VC_NONE;
}

unsigned CDecoder::GetAllowedReferences()
{
  return 5;
}
