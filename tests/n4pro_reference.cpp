// tests/n4pro_reference.cpp
#include "n4pro_reference.h"
#include <doctest/doctest.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>

namespace ref {
using Microsoft::WRL::ComPtr;

namespace {
void append_chunks(std::vector<Bytes>& out, const Bytes& data) {
  for (std::size_t off = 0; off < data.size(); off += 1024) {
    Bytes r(1025, 0);
    const std::size_t n = std::min<std::size_t>(1024, data.size() - off);
    std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(off), n, r.begin() + 1);
    out.push_back(std::move(r));
  }
}
std::uint8_t byte_of(std::uint32_t v, int shift) { return static_cast<std::uint8_t>((v >> shift) & 0xFF); }
}  // namespace

Bytes command(std::string_view tag, std::initializer_list<std::uint8_t> args) {
  Bytes r(1025, 0);
  const std::uint8_t header[] = {'C', 'R', 'T', 0, 0};
  std::copy(std::begin(header), std::end(header), r.begin() + 1);
  std::copy(tag.begin(), tag.end(), r.begin() + 6);
  std::copy(args.begin(), args.end(), r.begin() + 6 + static_cast<std::ptrdiff_t>(tag.size()));
  return r;
}

std::vector<Bytes> image_key(std::uint8_t index, const Bytes& jpeg) {
  const auto n = static_cast<std::uint32_t>(jpeg.size());
  std::vector<Bytes> out{command("BAT", {byte_of(n, 24), byte_of(n, 16), byte_of(n, 8), byte_of(n, 0), index})};
  append_chunks(out, jpeg);
  return out;
}

std::vector<Bytes> image_bg(const Bytes& jpeg, std::uint16_t x, std::uint16_t y, std::uint16_t w, std::uint16_t h) {
  const auto n = static_cast<std::uint32_t>(jpeg.size());
  std::vector<Bytes> out{command("BGPIC", {byte_of(n, 24), byte_of(n, 16), byte_of(n, 8), byte_of(n, 0),
                                           byte_of(x, 8), byte_of(x, 0), byte_of(y, 8), byte_of(y, 0),
                                           byte_of(w, 8), byte_of(w, 0), byte_of(h, 8), byte_of(h, 0), 0, 0})};
  append_chunks(out, jpeg);
  return out;
}

std::uint8_t key_index(int slot) { return static_cast<std::uint8_t>(slot < 5 ? 11 + slot : 1 + slot); }

Bytes jpeg(ld::Renderer& r, ld::Surface& s, int quality, bool rotate180) {
  const std::vector<std::uint8_t> px = s.pixels();
  const UINT w = static_cast<UINT>(s.width()), h = static_cast<UINT>(s.height());
  ComPtr<IWICBitmap> bmp;
  REQUIRE(SUCCEEDED(r.wic()->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppPBGRA, w * 4,
                                                     static_cast<UINT>(px.size()), const_cast<BYTE*>(px.data()), &bmp)));
  ComPtr<IWICBitmapSource> src = bmp;
  if (rotate180) {
    ComPtr<IWICBitmapFlipRotator> rot;
    REQUIRE(SUCCEEDED(r.wic()->CreateBitmapFlipRotator(&rot)));
    REQUIRE(SUCCEEDED(rot->Initialize(bmp.Get(), WICBitmapTransformRotate180)));
    src = rot;
  }
  ComPtr<IWICFormatConverter> conv;
  REQUIRE(SUCCEEDED(r.wic()->CreateFormatConverter(&conv)));
  REQUIRE(SUCCEEDED(conv->Initialize(src.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)));
  ComPtr<IStream> stream;
  REQUIRE(SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)));
  ComPtr<IWICBitmapEncoder> enc;
  REQUIRE(SUCCEEDED(r.wic()->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)));
  REQUIRE(SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)));
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> props;
  REQUIRE(SUCCEEDED(enc->CreateNewFrame(&frame, &props)));
  PROPBAG2 opt{};
  opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
  VARIANT v;
  VariantInit(&v);
  v.vt = VT_R4;
  v.fltVal = quality / 100.f;
  REQUIRE(SUCCEEDED(props->Write(1, &opt, &v)));
  REQUIRE(SUCCEEDED(frame->Initialize(props.Get())));
  REQUIRE(SUCCEEDED(frame->SetSize(w, h)));
  WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
  REQUIRE(SUCCEEDED(frame->SetPixelFormat(&fmt)));
  REQUIRE(SUCCEEDED(frame->WriteSource(conv.Get(), nullptr)));
  REQUIRE(SUCCEEDED(frame->Commit()));
  REQUIRE(SUCCEEDED(enc->Commit()));
  STATSTG st{};
  REQUIRE(SUCCEEDED(stream->Stat(&st, STATFLAG_NONAME)));
  HGLOBAL mem = nullptr;
  REQUIRE(SUCCEEDED(GetHGlobalFromStream(stream.Get(), &mem)));
  Bytes out(static_cast<std::size_t>(st.cbSize.QuadPart));
  void* p = GlobalLock(mem);
  REQUIRE(p != nullptr);
  std::memcpy(out.data(), p, out.size());
  GlobalUnlock(mem);
  return out;
}

}  // namespace ref
