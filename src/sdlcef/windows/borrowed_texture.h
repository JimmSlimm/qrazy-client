#pragma once
// Windows port foundation. Use only inside OnAcceleratedPaint on the owning
// device thread. No CEF handle/resource survives this function.
#include <windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <exception>

namespace QrazyWindows {
using Microsoft::WRL::ComPtr;

class BorrowedTextureCopy {
  ComPtr<ID3D11Device1> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<ID3D11Texture2D> owned_;
  ComPtr<ID3D11Query> completed_;
 public:
  bool Initialize(ID3D11Device* device) {
    if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&device_)))) return false;
    device->GetImmediateContext(&context_);
    D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
    return SUCCEEDED(device->CreateQuery(&query, &completed_));
  }
  ID3D11Texture2D* Owned() const { return owned_.Get(); }

  bool Copy(HANDLE borrowed, UINT x, UINT y, UINT width, UINT height) {
    if (!device_ || !borrowed || !width || !height) return false;
    ComPtr<ID3D11Texture2D> source;
    // CEF NT shared handles require OpenSharedResource1, not the older API.
    // Opening on the presentation device refuses incompatible adapter resources.
    if (FAILED(device_->OpenSharedResource1(borrowed, IID_PPV_ARGS(&source)))) return false;
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    if (desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1 ||
        (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) ||
        x > desc.Width || y > desc.Height || width > desc.Width-x || height > desc.Height-y) return false;
    D3D11_TEXTURE2D_DESC current{};
    if (owned_) owned_->GetDesc(&current);
    if (!owned_ || current.Width != width || current.Height != height || current.Format != desc.Format) {
      D3D11_TEXTURE2D_DESC target{};
      target.Width=width; target.Height=height; target.MipLevels=1; target.ArraySize=1;
      target.Format=desc.Format; target.SampleDesc.Count=1; target.Usage=D3D11_USAGE_DEFAULT;
      target.BindFlags=D3D11_BIND_SHADER_RESOURCE;
      ComPtr<ID3D11Texture2D> replacement;
      if (FAILED(device_->CreateTexture2D(&target, nullptr, &replacement))) return false;
      owned_=replacement;
    }
    D3D11_BOX region{x,y,0,x+width,y+height,1};
    context_->CopySubresourceRegion(owned_.Get(),0,0,0,0,source.Get(),0,&region);
    context_->End(completed_.Get()); context_->Flush();
    // Submission/Flush alone is insufficient. The producer may reuse its pool
    // immediately when CEF's callback returns, so wait for completed GPU work.
    for (;;) {
      BOOL done=FALSE;
      HRESULT result=context_->GetData(completed_.Get(),&done,sizeof(done),0);
      if (result==S_OK && done) return SUCCEEDED(device_->GetDeviceRemovedReason());
      if (FAILED(result)) {
        if (FAILED(device_->GetDeviceRemovedReason())) return false;
        // An unexplained query failure cannot establish borrowed-resource safety.
        // Do not return that resource to CEF while work may still be in flight.
        std::terminate();
      }
      Sleep(0);
    }
  }
};
} // namespace QrazyWindows
