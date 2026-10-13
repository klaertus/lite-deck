// src/render/jpeg.cpp
#include "render/jpeg.h"
#include <objbase.h>
#include <algorithm>
#include <cstring>
#include <format>
#include <stdexcept>
#include <utility>

namespace ld {
using Microsoft::WRL::ComPtr;

namespace {
void check(HRESULT hr, const char* what) {
  if (FAILED(hr)) throw std::runtime_error(std::format("{} failed: 0x{:08x}", what, static_cast<unsigned long>(hr)));
}
}  // namespace

WICBitmapTransformOptions wic_transform(Orientation o) {
  switch (o) {
    case Orientation::Rotate90: return WICBitmapTransformRotate90;
    case Orientation::Rotate180: return WICBitmapTransformRotate180;
    case Orientation::Rotate270: return WICBitmapTransformRotate270;
    case Orientation::FlipHorizontal: return WICBitmapTransformFlipHorizontal;
    case Orientation::FlipVertical: return WICBitmapTransformFlipVertical;
    case Orientation::None: break;
  }
  return WICBitmapTransformRotate0;
}

bool swaps_size(Orientation o) { return o == Orientation::Rotate90 || o == Orientation::Rotate270; }

std::vector<std::uint8_t> encode_jpeg_source(IWICImagingFactory* wic, IWICBitmapSource* source, int width, int height,
                                             int quality, Orientation orientation) {
  ComPtr<IWICBitmapSource> src = source;
  if (orientation != Orientation::None) {
    ComPtr<IWICBitmapFlipRotator> rot;
    check(wic->CreateBitmapFlipRotator(&rot), "CreateBitmapFlipRotator");
    check(rot->Initialize(source, wic_transform(orientation)), "FlipRotator.Initialize");
    src = rot;
  }
  if (swaps_size(orientation)) std::swap(width, height);
  ComPtr<IWICFormatConverter> conv;
  check(wic->CreateFormatConverter(&conv), "CreateFormatConverter");
  check(conv->Initialize(src.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0,
                         WICBitmapPaletteTypeCustom), "FormatConverter.Initialize");

  ComPtr<IStream> stream;
  check(CreateStreamOnHGlobal(nullptr, TRUE, &stream), "CreateStreamOnHGlobal");
  ComPtr<IWICBitmapEncoder> enc;
  check(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc), "CreateEncoder");
  check(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Encoder.Initialize");
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> props;
  check(enc->CreateNewFrame(&frame, &props), "CreateNewFrame");
  PROPBAG2 opt{};
  opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
  VARIANT v;
  VariantInit(&v);
  v.vt = VT_R4;
  v.fltVal = quality / 100.f;
  check(props->Write(1, &opt, &v), "PropertyBag.Write");
  check(frame->Initialize(props.Get()), "Frame.Initialize");
  check(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)), "Frame.SetSize");
  WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
  check(frame->SetPixelFormat(&fmt), "Frame.SetPixelFormat");
  check(frame->WriteSource(conv.Get(), nullptr), "Frame.WriteSource");
  check(frame->Commit(), "Frame.Commit");
  check(enc->Commit(), "Encoder.Commit");

  STATSTG st{};
  check(stream->Stat(&st, STATFLAG_NONAME), "Stream.Stat");
  HGLOBAL mem = nullptr;
  check(GetHGlobalFromStream(stream.Get(), &mem), "GetHGlobalFromStream");
  std::vector<std::uint8_t> out(static_cast<std::size_t>(st.cbSize.QuadPart));
  void* p = GlobalLock(mem);
  if (!p) throw std::runtime_error("GlobalLock failed");
  std::memcpy(out.data(), p, out.size());
  GlobalUnlock(mem);
  return out;
}

IWICImagingFactory* JpegEncoder::wic() {
  if (!wic_)
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_)),
          "CoCreateInstance(WIC)");
  return wic_.Get();
}

IWICBitmap* JpegEncoder::scratch(int w, int h) {
  for (const Scratch& s : scratch_)
    if (s.w == w && s.h == h) return s.bitmap.Get();
  if (scratch_.size() >= kMaxScratch) scratch_.erase(scratch_.begin());
  Scratch s{w, h, nullptr};
  check(wic()->CreateBitmap(static_cast<UINT>(w), static_cast<UINT>(h), GUID_WICPixelFormat32bppPBGRA,
                            WICBitmapCacheOnLoad, &s.bitmap), "CreateBitmap");
  scratch_.push_back(std::move(s));
  return scratch_.back().bitmap.Get();
}

std::vector<std::uint8_t> JpegEncoder::encode(const Image& img, int quality, Orientation orientation) {
  const std::size_t row = img.width > 0 ? static_cast<std::size_t>(img.width) * 4 : 0;
  if (img.width <= 0 || img.height <= 0 || img.stride < 0 || static_cast<std::size_t>(img.stride) < row ||
      img.bgra.size() < static_cast<std::size_t>(img.stride) * static_cast<std::size_t>(img.height - 1) + row)
    throw std::invalid_argument("JpegEncoder: the image is smaller than its size");
  IWICBitmap* bmp = scratch(img.width, img.height);
  {
    WICRect rc{0, 0, img.width, img.height};
    ComPtr<IWICBitmapLock> lock;
    check(bmp->Lock(&rc, WICBitmapLockWrite, &lock), "Lock");
    UINT stride = 0, size = 0;
    BYTE* p = nullptr;
    check(lock->GetStride(&stride), "GetStride");
    check(lock->GetDataPointer(&size, &p), "GetDataPointer");
    if (!p || stride < row || size < static_cast<std::size_t>(stride) * static_cast<std::size_t>(img.height - 1) + row)
      throw std::runtime_error("GetDataPointer returned a short buffer");
    for (int y = 0; y < img.height; ++y)
      std::memcpy(p + static_cast<std::size_t>(stride) * y, img.bgra.data() + static_cast<std::size_t>(img.stride) * y, row);
  }
  return encode_jpeg_source(wic(), bmp, img.width, img.height, quality, orientation);
}

void BandJpegEncoder::encode(const Image& img, int quality, std::vector<std::uint8_t>& out) {
  const std::size_t row = img.width > 0 ? static_cast<std::size_t>(img.width) * 4 : 0;
  if (img.width <= 0 || img.height <= 0 || img.stride < 0 || static_cast<std::size_t>(img.stride) < row ||
      img.bgra.size() < static_cast<std::size_t>(img.stride) * static_cast<std::size_t>(img.height - 1) + row)
    throw std::invalid_argument("BandJpegEncoder: the image is smaller than its size");
  if (!wic_)
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_)),
          "CoCreateInstance(WIC)");
  if (!stream_) check(CreateStreamOnHGlobal(nullptr, TRUE, &stream_), "CreateStreamOnHGlobal");
  check(stream_->Seek(LARGE_INTEGER{}, STREAM_SEEK_SET, nullptr), "Stream.Seek");

  ComPtr<IWICBitmapEncoder> enc;
  check(wic_->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc), "CreateEncoder");
  check(enc->Initialize(stream_.Get(), WICBitmapEncoderNoCache), "Encoder.Initialize");
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> props;
  check(enc->CreateNewFrame(&frame, &props), "CreateNewFrame");
  PROPBAG2 opt{};
  opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
  VARIANT v;
  VariantInit(&v);
  v.vt = VT_R4;
  v.fltVal = quality / 100.f;
  check(props->Write(1, &opt, &v), "PropertyBag.Write");
  check(frame->Initialize(props.Get()), "Frame.Initialize");
  check(frame->SetSize(static_cast<UINT>(img.width), static_cast<UINT>(img.height)), "Frame.SetSize");
  WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
  check(frame->SetPixelFormat(&fmt), "Frame.SetPixelFormat");
  if (fmt != GUID_WICPixelFormat24bppBGR) throw std::runtime_error("the JPEG encoder refused 24bppBGR");

  const std::size_t out_row = static_cast<std::size_t>(img.width) * 3;
  band_.resize(out_row * kBand);
  for (int y0 = 0; y0 < img.height; y0 += kBand) {
    const int lines = (std::min)(kBand, img.height - y0);
    for (int y = 0; y < lines; ++y) {
      const std::uint8_t* src = img.bgra.data() + static_cast<std::size_t>(img.stride) * static_cast<std::size_t>(y0 + y);
      std::uint8_t* dst = band_.data() + out_row * static_cast<std::size_t>(y);
      for (int x = 0; x < img.width; ++x, src += 4, dst += 3) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
      }
    }
    check(frame->WritePixels(static_cast<UINT>(lines), static_cast<UINT>(out_row), static_cast<UINT>(out_row * lines),
                             band_.data()),
          "Frame.WritePixels");
  }
  check(frame->Commit(), "Frame.Commit");
  check(enc->Commit(), "Encoder.Commit");

  ULARGE_INTEGER end{};
  check(stream_->Seek(LARGE_INTEGER{}, STREAM_SEEK_CUR, &end), "Stream.Seek");
  HGLOBAL mem = nullptr;
  check(GetHGlobalFromStream(stream_.Get(), &mem), "GetHGlobalFromStream");
  const void* p = GlobalLock(mem);
  if (!p) throw std::runtime_error("GlobalLock failed");
  struct Unlock {
    HGLOBAL mem;
    ~Unlock() { GlobalUnlock(mem); }
  } unlock{mem};  // also when assign throws: a lock left counted would stop the stream from growing
  const auto* bytes = static_cast<const std::uint8_t*>(p);
  out.assign(bytes, bytes + end.QuadPart);
}

}  // namespace ld
