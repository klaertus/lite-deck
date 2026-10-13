// src/modules/common/icon_loader.cpp: the icons of action keys, read on a worker thread: a
// program's or a file's icon from the shell, a picture file through WIC.
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <format>
#include "modules/common/action_key.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace ld {
namespace {

using Microsoft::WRL::ComPtr;

constexpr ULONGLONG kMaxImageFile = 32ull << 20;  // larger picture files are not read
constexpr UINT kMaxImageSide = 16384;
constexpr UINT64 kMaxImagePixels = 40'000'000;  // the decode and the resampling hold the whole source

bool decodable(UINT w, UINT h) {
  return w > 0 && h > 0 && w <= kMaxImageSide && h <= kMaxImageSide && UINT64{w} * h <= kMaxImagePixels;
}

// COM on this worker thread, for the life of the thread (the shell needs an apartment).
void ensure_com() {
  thread_local const bool ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
  (void)ok;
}

// `source` converted to premultiplied BGRA and fitted in size x size with its proportions.
std::shared_ptr<const Bitmap> fitted(IWICImagingFactory* wic, IWICBitmapSource* source, int size) {
  UINT w = 0, h = 0;
  if (FAILED(source->GetSize(&w, &h)) || !decodable(w, h)) return nullptr;
  const double scale = std::min(static_cast<double>(size) / w, static_cast<double>(size) / h);
  const UINT tw = std::max(1u, static_cast<UINT>(w * scale + 0.5)), th = std::max(1u, static_cast<UINT>(h * scale + 0.5));
  ComPtr<IWICFormatConverter> conv;
  if (FAILED(wic->CreateFormatConverter(&conv)) ||
      FAILED(conv->Initialize(source, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                              WICBitmapPaletteTypeCustom)))
    return nullptr;
  ComPtr<IWICBitmapSource> out = conv;
  if (tw != w || th != h) {
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(conv.Get(), tw, th, WICBitmapInterpolationModeHighQualityCubic)))
      return nullptr;
    out = scaler;
  }
  auto b = std::make_shared<Bitmap>();
  b->width = static_cast<int>(tw);
  b->height = static_cast<int>(th);
  b->bgra.resize(static_cast<std::size_t>(tw) * th * 4);
  if (FAILED(out->CopyPixels(nullptr, tw * 4, static_cast<UINT>(b->bgra.size()), b->bgra.data()))) return nullptr;
  return b;
}

ComPtr<IWICImagingFactory> wic_factory() {
  ComPtr<IWICImagingFactory> wic;
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
  return wic;
}

// A picture file: its largest frame (an .ico holds several sizes).
std::shared_ptr<const Bitmap> decode_image(const std::wstring& path, int size) {
  ComPtr<IWICImagingFactory> wic = wic_factory();
  if (!wic) return nullptr;
  ComPtr<IWICBitmapDecoder> dec;
  if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)))
    return nullptr;
  UINT frames = 0;
  if (FAILED(dec->GetFrameCount(&frames)) || frames == 0) return nullptr;
  ComPtr<IWICBitmapFrameDecode> best;
  UINT best_area = 0;
  for (UINT i = 0; i < std::min(frames, 64u); ++i) {
    ComPtr<IWICBitmapFrameDecode> f;
    UINT w = 0, h = 0;
    if (FAILED(dec->GetFrame(i, &f)) || FAILED(f->GetSize(&w, &h))) continue;
    if (!decodable(w, h)) continue;
    if (!best || w * h > best_area) {
      best = f;
      best_area = w * h;
    }
  }
  return best ? fitted(wic.Get(), best.Get(), size) : nullptr;
}

// A file's icon from the shell, at the largest size it has up to `size` (IShellItemImageFactory),
// else the large icon of SHGetFileInfo.
std::shared_ptr<const Bitmap> shell_icon(const std::wstring& path, int size) {
  ComPtr<IWICImagingFactory> wic = wic_factory();
  if (!wic) return nullptr;
  ComPtr<IShellItemImageFactory> factory;
  if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory)))) {
    HBITMAP hbm = nullptr;
    if (SUCCEEDED(factory->GetImage(SIZE{size, size}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbm)) && hbm) {
      ComPtr<IWICBitmap> bmp;
      std::shared_ptr<const Bitmap> out;
      if (SUCCEEDED(wic->CreateBitmapFromHBITMAP(hbm, nullptr, WICBitmapUsePremultipliedAlpha, &bmp)))
        out = fitted(wic.Get(), bmp.Get(), size);
      DeleteObject(hbm);
      if (out) return out;
    }
  }
  SHFILEINFOW info{};
  if (!SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON) || !info.hIcon) return nullptr;
  ComPtr<IWICBitmap> bmp;
  std::shared_ptr<const Bitmap> out;
  if (SUCCEEDED(wic->CreateBitmapFromHICON(info.hIcon, &bmp))) out = fitted(wic.Get(), bmp.Get(), size);
  DestroyIcon(info.hIcon);
  return out;
}

// The program that opens a URL's scheme (the default browser for http and https), or "".
std::wstring url_handler(const std::wstring& url) {
  const std::wstring scheme = url.substr(0, url.find(L':'));
  wchar_t exe[MAX_PATH] = {};
  DWORD n = MAX_PATH;
  if (FAILED(AssocQueryStringW(ASSOCF_IS_PROTOCOL, ASSOCSTR_EXECUTABLE, scheme.c_str(), L"open", exe, &n))) return {};
  return exe;
}

// A bare program name ("notepad.exe") found on the PATH; anything else as it is.
std::wstring full_path(const std::wstring& path) {
  if (path.find_first_of(L"\\/") != std::wstring::npos) return path;
  const std::wstring found = find_on_path(path);
  return found.empty() ? path : found;
}

class SystemIconLoader final : public IconLoader {
public:
  std::shared_ptr<const Bitmap> load(const IconRequest& r) override {
    ensure_com();
    std::wstring path = r.path;
    if (r.source == IconSource::Program) {
      path = is_url(path) ? url_handler(path) : full_path(path);
      if (path.empty()) return nullptr;
    } else if (r.source != IconSource::Image) {
      return nullptr;
    }
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr)) return nullptr;
    const ULONGLONG file_size = (static_cast<ULONGLONG>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
    const ULONGLONG mtime =
        (static_cast<ULONGLONG>(attr.ftLastWriteTime.dwHighDateTime) << 32) | attr.ftLastWriteTime.dwLowDateTime;
    const std::wstring key = std::format(L"{}|{}|{}|{}", static_cast<int>(r.source), path, mtime, r.size);
    if (auto hit = cache_.get(key)) return hit;
    std::shared_ptr<const Bitmap> icon;
    if (r.source == IconSource::Program) icon = shell_icon(path, r.size);
    else if (file_size <= kMaxImageFile && !(attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
      icon = decode_image(path, r.size);
    cache_.put(key, icon);
    return icon;
  }

private:
  IconCache cache_;  // 64 icons, 4 MB
};

}  // namespace

std::shared_ptr<IconLoader> system_icon_loader() {
  static const std::shared_ptr<IconLoader> loader = std::make_shared<SystemIconLoader>();
  return loader;
}

}  // namespace ld
