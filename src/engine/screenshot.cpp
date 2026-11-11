#include "screenshot.h"

#include <windows.h>
#include <wincodec.h>

#include <obs.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "../shared/files.h"
#include "../shared/log.h"

namespace bx {
namespace {

template <typename T>
void safe_release(T*& object) {
  if (object) {
    object->Release();
    object = nullptr;
  }
}

}  // namespace

bool encode_image_wic(const uint8_t* bgra, int width, int height, const std::string& path, bool jpeg,
                      std::string& error) {
  if (!bgra || width <= 0 || height <= 0) {
    error = "no image data";
    return false;
  }
  // WIC is COM, and the threads that reach this are not all COM-aware: the pipe worker and the CLI
  // never initialised an apartment. S_FALSE means "already initialised by this thread" and still needs
  // a matching CoUninitialize; RPC_E_CHANGED_MODE means another apartment is in place, which WIC
  // accepts as it is.
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool uninitialize = SUCCEEDED(com);
  struct ComGuard {
    bool active;
    ~ComGuard() {
      if (active) CoUninitialize();
    }
  } guard{uninitialize};

  IWICImagingFactory* factory = nullptr;
  HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  if (FAILED(result) || !factory) {
    error = "Windows imaging component unavailable";
    return false;
  }
  IWICStream* stream = nullptr;
  IWICBitmapEncoder* encoder = nullptr;
  IWICBitmapFrameEncode* frame = nullptr;
  IPropertyBag2* properties = nullptr;
  bool ok = false;
  do {
    const std::wstring wide = wide_path(path);
    if (FAILED(factory->CreateStream(&stream))) { error = "could not create the output stream"; break; }
    if (FAILED(stream->InitializeFromFilename(wide.c_str(), GENERIC_WRITE))) { error = "could not open the output file"; break; }
    const GUID format = jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng;
    if (FAILED(factory->CreateEncoder(format, nullptr, &encoder))) { error = "no encoder for that format"; break; }
    if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) { error = "could not initialise the encoder"; break; }
    if (FAILED(encoder->CreateNewFrame(&frame, &properties))) { error = "could not create a frame"; break; }
    if (FAILED(frame->Initialize(properties))) { error = "could not initialise the frame"; break; }
    if (FAILED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)))) { error = "could not set the frame size"; break; }
    WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&pixel_format))) { error = "could not set the pixel format"; break; }
    const UINT stride = static_cast<UINT>(width) * 4;
    const UINT bytes = stride * static_cast<UINT>(height);
    if (IsEqualGUID(pixel_format, GUID_WICPixelFormat32bppBGRA)) {
      if (FAILED(frame->WritePixels(static_cast<UINT>(height), stride, bytes, const_cast<BYTE*>(bgra)))) {
        error = "could not write the image data";
        break;
      }
    } else {
      // The encoder chose its own layout (the JPEG encoder often wants 24bppBGR). Handing it BGRA
      // bytes anyway is how a screenshot comes out as stripes; convert instead.
      IWICBitmap* bitmap = nullptr;
      IWICFormatConverter* converter = nullptr;
      if (FAILED(factory->CreateBitmapFromMemory(static_cast<UINT>(width), static_cast<UINT>(height),
                                                 GUID_WICPixelFormat32bppBGRA, stride, bytes,
                                                 const_cast<BYTE*>(bgra), &bitmap))) {
        error = "could not stage the image data";
        break;
      }
      const HRESULT created = factory->CreateFormatConverter(&converter);
      const HRESULT initialized =
          SUCCEEDED(created)
              ? converter->Initialize(bitmap, pixel_format, WICBitmapDitherTypeNone, nullptr, 0.0,
                                      WICBitmapPaletteTypeCustom)
              : created;
      const HRESULT written = SUCCEEDED(initialized) ? frame->WriteSource(converter, nullptr) : initialized;
      safe_release(converter);
      safe_release(bitmap);
      if (FAILED(written)) {
        error = "could not write the converted image data";
        break;
      }
    }
    if (FAILED(frame->Commit())) { error = "could not finalise the frame"; break; }
    if (FAILED(encoder->Commit())) { error = "could not finalise the image"; break; }
    ok = true;
  } while (false);

  safe_release(properties);
  safe_release(frame);
  safe_release(encoder);
  safe_release(stream);
  safe_release(factory);
  if (!ok) DeleteFileA(path.c_str());
  return ok;
}

bool write_screenshot(obs_source* source, const std::string& path, bool jpeg, std::string& error) {
  if (!source) {
    error = "no capture source";
    return false;
  }
  const uint32_t width = obs_source_get_width(source);
  const uint32_t height = obs_source_get_height(source);
  if (width == 0 || height == 0) {
    error = "the capture source has no frame yet";
    return false;
  }

  std::vector<uint8_t> bgra(static_cast<size_t>(width) * height * 4);
  bool captured = false;

  // One GPU pass: render the source into a texture, stage it down to CPU memory, copy it out. The
  // copy is the only readback in the product and it happens once per user-initiated screenshot.
  BX_LOG_INFO("screenshot: entering graphics (%ux%u)", width, height);
  obs_enter_graphics();
  BX_LOG_INFO("screenshot: graphics entered");
  gs_texrender_t* texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
  gs_stagesurf_t* stagesurface = nullptr;
  if (texrender) {
    gs_texrender_reset(texrender);
    if (gs_texrender_begin(texrender, width, height)) {
      struct vec4 clear{};
      vec4_zero(&clear);
      clear.w = 1.0f;
      gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
      gs_ortho(0.0f, static_cast<float>(width), 0.0f, static_cast<float>(height), -100.0f, 100.0f);
      obs_source_video_render(source);
      gs_texrender_end(texrender);
      BX_LOG_INFO("screenshot: source rendered");

      stagesurface = gs_stagesurface_create(width, height, GS_BGRA);
      if (stagesurface) {
        gs_stage_texture(stagesurface, gs_texrender_get_texture(texrender));
        BX_LOG_INFO("screenshot: texture staged");
        uint8_t* data = nullptr;
        uint32_t linesize = 0;
        if (gs_stagesurface_map(stagesurface, &data, &linesize) && data) {
          BX_LOG_INFO("screenshot: staged surface mapped");
          for (uint32_t y = 0; y < height; ++y) {
            std::memcpy(&bgra[static_cast<size_t>(y) * width * 4], data + static_cast<size_t>(y) * linesize,
                        static_cast<size_t>(width) * 4);
          }
          gs_stagesurface_unmap(stagesurface);
          captured = true;
        }
      }
    }
  }
  if (stagesurface) gs_stagesurface_destroy(stagesurface);
  if (texrender) gs_texrender_destroy(texrender);
  obs_leave_graphics();

  if (!captured) {
    error = "the frame could not be read back";
    return false;
  }
  return encode_image_wic(bgra.data(), static_cast<int>(width), static_cast<int>(height), path, jpeg, error);
}

}  // namespace bx
