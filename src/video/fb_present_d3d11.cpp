#include "fb_present_d3d11.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

using Microsoft::WRL::ComPtr;

struct FbGpuPresenter {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapchain;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> source;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11SamplerState> sampler;
    uint32_t source_width, source_height, width, height;

    HRESULT create_target()
    {
        ComPtr<ID3D11Texture2D> buffer;
        HRESULT hr = swapchain->GetBuffer(0, IID_PPV_ARGS(&buffer));
        if (SUCCEEDED(hr)) hr = device->CreateRenderTargetView(buffer.Get(), nullptr, &target);
        return hr;
    }
};

static int fb_gpu_error(const char *operation, HRESULT hr)
{
    std::fprintf(stderr, "[FBWIN] D3D11 %s failed: HRESULT 0x%08lX\n",
                 operation, static_cast<unsigned long>(hr));
    return 0;
}

static HRESULT fb_compile(const char *entry, const char *profile, ID3DBlob **output)
{
    static const char shader[] =
        "Texture2D pixels : register(t0); SamplerState nearest : register(s0);\n"
        "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
        "V vs(uint id : SV_VertexID) { V v;\n"
        " v.uv = float2((id << 1) & 2, id & 2);\n"
        " v.p = float4(v.uv.x * 2 - 1, 1 - v.uv.y * 2, 0, 1); return v; }\n"
        "float4 ps(V v) : SV_Target {\n"
        " return float4(pixels.SampleLevel(nearest, v.uv, 0).rgb, 1); }\n";
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(shader, sizeof(shader) - 1, "framebuffer-present",
                           nullptr, nullptr, entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3,
                           0, output, &errors);
    if (FAILED(hr) && errors)
        std::fprintf(stderr, "[FBWIN] D3D11 shader diagnostics: %.*s\n",
                     static_cast<int>(errors->GetBufferSize()), static_cast<const char *>(errors->GetBufferPointer()));
    return hr;
}

FbGpuPresenter *fb_gpu_create(void *window, uint32_t source_width, uint32_t source_height,
                              uint32_t client_width, uint32_t client_height)
{
    std::unique_ptr<FbGpuPresenter> p(new (std::nothrow) FbGpuPresenter{});
    if (!p) {
        fb_gpu_error("presenter allocation", E_OUTOFMEMORY);
        return nullptr;
    }
    p->source_width = source_width;
    p->source_height = source_height;
    p->width = client_width;
    p->height = client_height;
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.Width = client_width;
    desc.BufferDesc.Height = client_height;
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.OutputWindow = static_cast<HWND>(window);
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED,
        nullptr, 0, D3D11_SDK_VERSION, &desc, &p->swapchain, &p->device, nullptr, &p->context);
    if (FAILED(hr)) {
        fb_gpu_error("device/swapchain creation", hr);
        return nullptr;
    }
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory> factory;
    hr = p->device.As(&dxgi_device);
    if (SUCCEEDED(hr)) hr = dxgi_device->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->MakeWindowAssociation(static_cast<HWND>(window), DXGI_MWA_NO_ALT_ENTER);
    if (SUCCEEDED(hr)) hr = p->create_target();
    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = source_width;
    texture.Height = source_height;
    texture.MipLevels = texture.ArraySize = 1;
    texture.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texture.SampleDesc.Count = 1;
    texture.Usage = D3D11_USAGE_DYNAMIC;
    texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texture.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (SUCCEEDED(hr)) hr = p->device->CreateTexture2D(&texture, nullptr, &p->texture);
    if (SUCCEEDED(hr)) hr = p->device->CreateShaderResourceView(p->texture.Get(), nullptr, &p->source);
    ComPtr<ID3DBlob> vs, ps;
    if (SUCCEEDED(hr)) hr = fb_compile("vs", "vs_4_0", &vs);
    if (SUCCEEDED(hr)) hr = fb_compile("ps", "ps_4_0", &ps);
    if (SUCCEEDED(hr)) hr = p->device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &p->vs);
    if (SUCCEEDED(hr)) hr = p->device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &p->ps);
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (SUCCEEDED(hr)) hr = p->device->CreateSamplerState(&sampler, &p->sampler);
    if (FAILED(hr)) {
        fb_gpu_error("presentation resources", hr);
        return nullptr;
    }
    std::fprintf(stderr, "[FBWIN] D3D11 GPU presentation enabled (native uploads, point scaling, flip swapchain)\n");
    return p.release();
}

int fb_gpu_draw(FbGpuPresenter *p, const uint32_t *pixels,
                uint32_t client_width, uint32_t client_height, int upload)
{
    HRESULT hr;
    if (client_width != p->width || client_height != p->height) {
        p->context->OMSetRenderTargets(0, nullptr, nullptr);
        p->target.Reset();
        hr = p->swapchain->ResizeBuffers(0, client_width, client_height, DXGI_FORMAT_UNKNOWN, 0);
        if (SUCCEEDED(hr)) hr = p->create_target();
        if (FAILED(hr)) return fb_gpu_error("swapchain resize", hr);
        p->width = client_width;
        p->height = client_height;
    }
    if (upload) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = p->context->Map(p->texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) return fb_gpu_error("native framebuffer upload", hr);
        for (uint32_t y = 0; y < p->source_height; ++y)
            std::memcpy(static_cast<unsigned char *>(mapped.pData) + y * mapped.RowPitch,
                        pixels + y * p->source_width, p->source_width * sizeof(uint32_t));
        p->context->Unmap(p->texture.Get(), 0);
    }
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(client_width);
    viewport.Height = static_cast<float>(client_height);
    viewport.MaxDepth = 1;
    ID3D11RenderTargetView *target = p->target.Get();
    ID3D11ShaderResourceView *source = p->source.Get();
    ID3D11SamplerState *sampler = p->sampler.Get();
    p->context->RSSetViewports(1, &viewport);
    p->context->OMSetRenderTargets(1, &target, nullptr);
    p->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    p->context->VSSetShader(p->vs.Get(), nullptr, 0);
    p->context->PSSetShader(p->ps.Get(), nullptr, 0);
    p->context->PSSetShaderResources(0, 1, &source);
    p->context->PSSetSamplers(0, 1, &sampler);
    p->context->Draw(3, 0);
    hr = p->swapchain->Present(0, 0);
    if (FAILED(hr)) return fb_gpu_error("swapchain presentation", hr);
    return 1;
}

void fb_gpu_destroy(FbGpuPresenter *presenter)
{
    delete presenter;
}
