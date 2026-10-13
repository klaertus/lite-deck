#include <windows.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include "core/log.h"
#include "modules/audio/audio_endpoint.h"

namespace ld {
namespace {
using Microsoft::WRL::ComPtr;

class Endpoint final : public AudioEndpoint {
public:
  explicit Endpoint(ComPtr<IAudioEndpointVolume> v) : v_(std::move(v)) {}
  std::optional<float> volume() override {
    float x = 0;
    return SUCCEEDED(v_->GetMasterVolumeLevelScalar(&x)) ? std::optional<float>(x) : std::nullopt;
  }
  bool set_volume(float x) override { return SUCCEEDED(v_->SetMasterVolumeLevelScalar(x, nullptr)); }
  std::optional<bool> muted() override {
    BOOL m = FALSE;
    return SUCCEEDED(v_->GetMute(&m)) ? std::optional<bool>(m != FALSE) : std::nullopt;
  }
  bool set_muted(bool m) override { return SUCCEEDED(v_->SetMute(m ? TRUE : FALSE, nullptr)); }
  IAudioEndpointVolume* raw() const { return v_.Get(); }

private:
  ComPtr<IAudioEndpointVolume> v_;
};

// Shared by both COM callbacks; holds the user's callback under a mutex so it can be cleared.
struct Sink {
  std::mutex mu;
  std::function<void(bool)> cb;
  void fire(bool changed) {
    std::function<void(bool)> f;
    {
      std::lock_guard lock(mu);
      f = cb;
    }
    if (f) f(changed);
  }
};

class VolumeCallback final : public IAudioEndpointVolumeCallback {
public:
  explicit VolumeCallback(std::shared_ptr<Sink> s) : sink_(std::move(s)) {}
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = --refs_;
    if (r == 0) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioEndpointVolumeCallback)) {
      *out = static_cast<IAudioEndpointVolumeCallback*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA) override {
    sink_->fire(false);
    return S_OK;
  }

private:
  std::atomic<ULONG> refs_{1};
  std::shared_ptr<Sink> sink_;
};

class DeviceCallback final : public IMMNotificationClient {
public:
  DeviceCallback(std::shared_ptr<Sink> s, EDataFlow flow) : sink_(std::move(s)), flow_(flow) {}
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = --refs_;
    if (r == 0) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
      *out = static_cast<IMMNotificationClient*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
    if (flow == flow_ && role == eConsole) sink_->fire(true);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
  std::atomic<ULONG> refs_{1};
  std::shared_ptr<Sink> sink_;
  EDataFlow flow_;
};

class CoreAudio final : public AudioEndpointProvider {
public:
  explicit CoreAudio(AudioFlow flow) : flow_(flow == AudioFlow::Render ? eRender : eCapture) {
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enum_)))) {
      log_warn("core audio: no device enumerator");
      return;
    }
    devices_cb_ = new DeviceCallback(sink_, flow_);
    enum_->RegisterEndpointNotificationCallback(devices_cb_);
    reopen();
  }
  ~CoreAudio() override {
    {
      std::lock_guard lock(sink_->mu);
      sink_->cb = nullptr;
    }
    unregister_volume();
    if (enum_ && devices_cb_) enum_->UnregisterEndpointNotificationCallback(devices_cb_);
    if (devices_cb_) devices_cb_->Release();
    if (volume_cb_) volume_cb_->Release();
  }
  AudioEndpoint* endpoint() override { return ep_.get(); }
  void reopen() override {
    unregister_volume();
    ep_.reset();
    if (!enum_) return;
    ComPtr<IMMDevice> dev;
    if (FAILED(enum_->GetDefaultAudioEndpoint(flow_, eConsole, &dev))) return;
    ComPtr<IAudioEndpointVolume> vol;
    if (FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                             reinterpret_cast<void**>(vol.GetAddressOf()))))
      return;
    if (!volume_cb_) volume_cb_ = new VolumeCallback(sink_);
    vol->RegisterControlChangeNotify(volume_cb_);
    ep_ = std::make_unique<Endpoint>(vol);
  }
  void watch(std::function<void(bool)> on_change) override {
    std::lock_guard lock(sink_->mu);
    sink_->cb = std::move(on_change);
  }

private:
  void unregister_volume() {
    if (ep_ && volume_cb_) ep_->raw()->UnregisterControlChangeNotify(volume_cb_);
  }
  EDataFlow flow_;
  ComPtr<IMMDeviceEnumerator> enum_;
  std::shared_ptr<Sink> sink_ = std::make_shared<Sink>();
  DeviceCallback* devices_cb_ = nullptr;
  VolumeCallback* volume_cb_ = nullptr;
  std::unique_ptr<Endpoint> ep_;
};

}  // namespace

std::unique_ptr<AudioEndpointProvider> make_core_audio(AudioFlow flow) { return std::make_unique<CoreAudio>(flow); }

}  // namespace ld
