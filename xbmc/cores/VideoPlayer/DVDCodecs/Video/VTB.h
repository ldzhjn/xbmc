/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#ifdef HAS_GL
#include <OpenGL/gl.h>
#else
#include <OpenGLES/ES2/gl.h>
#endif

#include "DVDVideoCodecFFmpeg.h"
#include "cores/VideoPlayer/Buffers/VideoBuffer.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"

#include <CoreVideo/CVPixelBuffer.h>
#if defined(TARGET_DARWIN_TVOS)
#include <CoreVideo/CVPixelBufferPool.h>
#endif

class CProcessInfo;

namespace VTB
{
class CVideoBufferVTB;
class CVideoBufferPoolVTB;

class CVideoBufferVTB: public CVideoBuffer
{
public:
  CVideoBufferVTB(IVideoBufferPool &pool, int id);
  ~CVideoBufferVTB() override;
  void SetRef(AVFrame *frame);
  void Unref();
  CVPixelBufferRef GetPB();

  GLuint m_fence = 0;
protected:
  CVPixelBufferRef m_pbRef = nullptr;
  AVFrame *m_pFrame;
};

#if defined(TARGET_DARWIN_TVOS)
// Upload software decoded HDR frames to the same 10-bit video layer as VTB frames.
class CSoftwareHDR
{
public:
  CSoftwareHDR();
  ~CSoftwareHDR();
  bool CanConvert(const AVFrame* frame, const CDVDStreamInfo& hints) const;
  CVideoBuffer* Convert(const AVFrame* frame);

private:
  bool m_enabled = false;
  bool m_loggedFirstPicture = false;
  int m_width = 0;
  int m_height = 0;
  CVPixelBufferPoolRef m_pixelBufferPool = nullptr;
  SwsContext* m_swsContext = nullptr;
  std::shared_ptr<CVideoBufferPoolVTB> m_videoBufferPool;
};
#endif

class CDecoder: public IHardwareDecoder
{
public:
  CDecoder(CProcessInfo& processInfo,
           bool hdrOutput = false,
           bool allowColorTransferFallback = false);
  ~CDecoder() override;
  static IHardwareDecoder* Create(CDVDStreamInfo &hint, CProcessInfo &processInfo, AVPixelFormat fmt);
  static bool Register();
  bool Open(AVCodecContext* avctx, AVCodecContext* mainctx, const enum AVPixelFormat) override;
  CDVDVideoCodec::VCReturn Decode(AVCodecContext* avctx, AVFrame* frame) override;
  bool GetPicture(AVCodecContext* avctx, VideoPicture* picture) override;
  CDVDVideoCodec::VCReturn Check(AVCodecContext* avctx) override;
  const std::string Name() override { return "vtb"; }
  unsigned GetAllowedReferences() override;

  void Close();

protected:
  unsigned m_renderbuffers_count;
  AVCodecContext *m_avctx;
  CProcessInfo& m_processInfo;
  bool m_hdrOutput = false;
  bool m_allowColorTransferFallback = false;
  bool m_loggedFirstPicture = false;
  CVideoBufferVTB *m_renderBuffer = nullptr;
  std::shared_ptr<CVideoBufferPoolVTB> m_videoBufferPool;
};

}
