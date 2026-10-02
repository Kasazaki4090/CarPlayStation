#include "D3D11Renderer.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <string>

using Microsoft::WRL::ComPtr;

// HLSL Shader for fullscreen quad with procedural vertices
static const char* kShaderSource = R"(
struct VS_OUTPUT {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VS_OUTPUT vs_main(uint id : SV_VertexID) {
    VS_OUTPUT output;
    // id: 0 -> (0,0), 1 -> (1,0), 2 -> (0,1), 3 -> (1,1)
    output.uv = float2((id == 1 || id == 3) ? 1.0f : 0.0f, (id >= 2) ? 1.0f : 0.0f);
    output.pos = float4(output.uv.x * 2.0f - 1.0f, 1.0f - output.uv.y * 2.0f, 0.0f, 1.0f);
    return output;
}

Texture2D g_texture : register(t0);
SamplerState g_sampler : register(s0);

float4 ps_main(VS_OUTPUT input) : SV_Target {
    return g_texture.Sample(g_sampler, input.uv);
}
)";

namespace CarPlayStation::UI {

D3D11Renderer::D3D11Renderer() = default;

D3D11Renderer::~D3D11Renderer() {
    Shutdown();
}

Core::Common::Status D3D11Renderer::Init(HWND hwnd, uint32_t initialWidth, uint32_t initialHeight) {
    if (initialized_) {
        return Core::Common::Status::Ok();
    }

    hwnd_ = hwnd;
    client_width_ = (initialWidth > 0) ? initialWidth : 1280;
    client_height_ = (initialHeight > 0) ? initialHeight : 720;
    current_viewport_ = CalculateLetterbox(client_width_, client_height_, video_width_, video_height_);

    // 1. Create Device and Immediate Context with BGRA support for D2D interop
    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL featureLevelOut;

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createFlags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &device_,
        &featureLevelOut,
        &context_
    );

    // If hardware failed or debug layer not present, retry without debug
    if (FAILED(hr)) {
        createFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            createFlags,
            featureLevels,
            ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            &device_,
            &featureLevelOut,
            &context_
        );
        if (FAILED(hr)) {
            return Core::Common::MakeError(Core::Common::StatusCode::MediaPipelineError,
                "D3D11CreateDevice failed: hr=0x" + std::to_string(static_cast<uint32_t>(hr)));
        }
    }

    // 2. Query DXGI Factory & Create SwapChain
    ComPtr<IDXGIDevice> dxgiDevice;
    hr = device_.As(&dxgiDevice);
    if (FAILED(hr)) {
        return Core::Common::MakeError(Core::Common::StatusCode::MediaPipelineError, "Query IDXGIDevice failed");
    }

    ComPtr<IDXGIAdapter> dxgiAdapter;
    hr = dxgiDevice->GetAdapter(&dxgiAdapter);
    if (FAILED(hr)) {
        return Core::Common::MakeError(Core::Common::StatusCode::MediaPipelineError, "GetAdapter failed");
    }

    ComPtr<IDXGIFactory2> dxgiFactory;
    hr = dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory));
    if (FAILED(hr)) {
        return Core::Common::MakeError(Core::Common::StatusCode::MediaPipelineError, "Get IDXGIFactory2 failed");
    }

    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.Width = client_width_;
    scDesc.Height = client_height_;
    scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scDesc.SampleDesc.Count = 1;
    scDesc.SampleDesc.Quality = 0;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = 2;
    scDesc.Scaling = DXGI_SCALING_STRETCH;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    hr = dxgiFactory->CreateSwapChainForHwnd(
        device_.Get(),
        hwnd_,
        &scDesc,
        nullptr,
        nullptr,
        &swap_chain_
    );

    // Fallback for older Windows if FLIP_DISCARD is not supported
    if (FAILED(hr)) {
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        hr = dxgiFactory->CreateSwapChainForHwnd(
            device_.Get(),
            hwnd_,
            &scDesc,
            nullptr,
            nullptr,
            &swap_chain_
        );
        if (FAILED(hr)) {
            return Core::Common::MakeError(Core::Common::StatusCode::MediaPipelineError,
                "CreateSwapChainForHwnd failed: hr=0x" + std::to_string(static_cast<uint32_t>(hr)));
        }
    }

    // Prevent DXGI from monitoring Alt+Enter automatically (we handle it in Win32)
    dxgiFactory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);

    // 3. Create Render Target View
    auto rtStatus = CreateRenderTarget();
    if (rtStatus.IsError()) {
        return rtStatus;
    }

    // 4. Create Shaders and Sampler
    auto shStatus = CreateShaders();
    if (shStatus.IsError()) {
        return shStatus;
    }

    initialized_ = true;
    return Core::Common::Status::Ok();
}

Core::Common::Status D3D11Renderer::CreateRenderTarget() {
    if (!swap_chain_ || !device_) {
        return Core::Common::MakeError(Core::Common::StatusCode::InvalidState, "Device or swapchain null");
    }

    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = swap_chain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) {
        return Core::Common::MakeError(Core::Common::StatusCode::MediaPipelineError, "GetBuffer(0) failed");
    }

    hr = device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &render_target_view_);
    if (FAILED(hr)) {
        return Core::Common::MakeError(Core::Common::StatusCode::MediaPipelineError, "CreateRenderTargetView failed");
    }

    return Core::Common::Status::Ok();
}

void D3D11Renderer::CleanupRenderTarget() {
    render_target_view_.Reset();
}

Core::Common::Status D3D11Renderer::CreateShaders() {
    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> psBlob;
    ComPtr<ID3DBlob> errorBlob;

    HRESULT hr = D3DCompile(
        kShaderSource,
        strlen(kShaderSource),
        "D3D11Renderer_Shaders",
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "vs_main",
        "vs_5_0",
        0,
        0,
        &vsBlob,
        &errorBlob
    );
    if (FAILED(hr)) {
        std::string err = errorBlob ? static_cast<char*>(errorBlob->GetBufferPointer()) : "Unknown VS error";
        return Core::Common::MakeError(Core::Common::StatusCode::MediaPipelineError, "VS Compile failed: " + err);
    }

    hr = device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vertex_shader_);
    if (FAILED(hr)) {
        return Core::Common::MakeError(Core::Common::StatusCode::MediaPipelineError, "CreateVertexShader failed");
    }

    hr = D3DCompile(
        kShaderSource,
        strlen(kShaderSource),
        "D3D11Renderer_Shaders",
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "ps_main",
        "ps_5_0",
        0,
        0,
        &psBlob,
        &errorBlob
    );
    if (FAILED(hr)) {
        std::string err = errorBlob ? static_cast<char*>(errorBlob->GetBufferPointer()) : "Unknown PS error";
        return Core::Common::MakeError(Core::Common::StatusCode::MediaPipelineError, "PS Compile failed: " + err);
    }

    hr = device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &pixel_shader_);
    if (FAILED(hr)) {
        return Core::Common::MakeError(Core::Common::StatusCode::MediaPipelineError, "CreatePixelShader failed");
    }

    // Bilinear clamp sampler
    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;

    hr = device_->CreateSamplerState(&sampDesc, &sampler_state_);
    if (FAILED(hr)) {
        return Core::Common::MakeError(Core::Common::StatusCode::MediaPipelineError, "CreateSamplerState failed");
    }

    return Core::Common::Status::Ok();
}

Core::Common::Status D3D11Renderer::Resize(uint32_t newWidth, uint32_t newHeight) {
    if (!initialized_ || !swap_chain_) {
        return Core::Common::Status::Ok();
    }

    if (newWidth == 0 || newHeight == 0) {
        return Core::Common::Status::Ok();
    }

    client_width_ = newWidth;
    client_height_ = newHeight;
    current_viewport_ = CalculateLetterbox(client_width_, client_height_, video_width_, video_height_);

    CleanupRenderTarget();

    HRESULT hr = swap_chain_->ResizeBuffers(0, client_width_, client_height_, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        return Core::Common::MakeError(Core::Common::StatusCode::MediaPipelineError,
            "ResizeBuffers failed: hr=0x" + std::to_string(static_cast<uint32_t>(hr)));
    }

    return CreateRenderTarget();
}

void D3D11Renderer::ConsumeFrame(uintptr_t textureHandle, uint32_t width, uint32_t height, uint64_t ptsUs) {
    (void)ptsUs;
    std::lock_guard<std::mutex> lock(frame_mutex_);

    if (width > 0 && height > 0) {
        if (width != video_width_ || height != video_height_) {
            video_width_ = width;
            video_height_ = height;
            current_viewport_ = CalculateLetterbox(client_width_, client_height_, video_width_, video_height_);
        }
    }

    if (textureHandle != 0) {
        if (textureHandle != last_texture_handle_) {
            UpdateSharedTexture(textureHandle, width, height);
        }
        active_frame_available_ = true;
    }
}

void D3D11Renderer::UpdateSharedTexture(uintptr_t handle, uint32_t width, uint32_t height) {
    (void)width;
    (void)height;
    if (!device_) {
        return;
    }

    current_srv_.Reset();
    current_keyed_mutex_.Reset();
    current_texture_.Reset();
    last_texture_handle_ = handle;

    // 1. Try to open as shared resource handle
    ComPtr<ID3D11Texture2D> sharedTex;
    HRESULT hr = device_->OpenSharedResource(
        reinterpret_cast<HANDLE>(handle),
        __uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(sharedTex.GetAddressOf())
    );

    // 2. If failed, handle may be a direct ID3D11Texture2D* from same device
    if (FAILED(hr)) {
        auto* rawTex = reinterpret_cast<ID3D11Texture2D*>(handle);
        sharedTex = rawTex;
    }

    if (!sharedTex) {
        return;
    }

    current_texture_ = sharedTex;

    // Check KeyedMutex support
    (void)current_texture_.As(&current_keyed_mutex_);

    // Create Shader Resource View
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;

    // Get actual format from texture if possible
    D3D11_TEXTURE2D_DESC texDesc{};
    current_texture_->GetDesc(&texDesc);
    if (texDesc.Format != DXGI_FORMAT_UNKNOWN) {
        srvDesc.Format = texDesc.Format;
    }

    hr = device_->CreateShaderResourceView(current_texture_.Get(), &srvDesc, &current_srv_);
    if (FAILED(hr)) {
        // Fallback with null desc for default SRV
        device_->CreateShaderResourceView(current_texture_.Get(), nullptr, &current_srv_);
    }
}

Core::Common::Status D3D11Renderer::Render(bool autoPresent) {
    if (!initialized_ || !context_ || !render_target_view_) {
        return Core::Common::MakeError(Core::Common::StatusCode::InvalidState, "Renderer not initialized");
    }

    // 1. Clear backbuffer to solid black (Letterbox bars)
    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    context_->ClearRenderTargetView(render_target_view_.Get(), clearColor);

    // 2. Render active video quad if texture available
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        if (active_frame_available_ && current_srv_) {
            // Acquire keyed mutex if present
            if (current_keyed_mutex_) {
                current_keyed_mutex_->AcquireSync(0, 10);
            }

            // Set Render Target
            ID3D11RenderTargetView* rtv = render_target_view_.Get();
            context_->OMSetRenderTargets(1, &rtv, nullptr);

            // Set Viewport matching letterbox
            D3D11_VIEWPORT vp{};
            vp.TopLeftX = current_viewport_.x;
            vp.TopLeftY = current_viewport_.y;
            vp.Width = current_viewport_.width;
            vp.Height = current_viewport_.height;
            vp.MinDepth = 0.0f;
            vp.MaxDepth = 1.0f;
            context_->RSSetViewports(1, &vp);

            // Bind Shaders & Pipeline
            context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
            context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);

            ID3D11ShaderResourceView* srv = current_srv_.Get();
            context_->PSSetShaderResources(0, 1, &srv);

            ID3D11SamplerState* sampler = sampler_state_.Get();
            context_->PSSetSamplers(0, 1, &sampler);

            // Draw procedural 4-vertex quad (SV_VertexID generates coordinates)
            context_->Draw(4, 0);

            // Unbind SRV to allow writer updates
            ID3D11ShaderResourceView* nullSrv = nullptr;
            context_->PSSetShaderResources(0, 1, &nullSrv);

            // Release keyed mutex
            if (current_keyed_mutex_) {
                current_keyed_mutex_->ReleaseSync(0);
            }
        }
    }

    // Unbind render target view to allow Direct2D interop render target to bind
    ID3D11RenderTargetView* nullRtv = nullptr;
    context_->OMSetRenderTargets(1, &nullRtv, nullptr);

    if (autoPresent) {
        return Present();
    }

    return Core::Common::Status::Ok();
}

Core::Common::Status D3D11Renderer::Present() {
    if (!initialized_ || !swap_chain_) {
        return Core::Common::MakeError(Core::Common::StatusCode::InvalidState, "SwapChain not initialized");
    }

    // Present SwapChain (VSync: syncInterval=1)
    HRESULT hr = swap_chain_->Present(1, 0);
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            return Core::Common::MakeError(Core::Common::StatusCode::Unavailable, "Device removed or reset");
        }
    }

    presented_frame_count_++;
    return Core::Common::Status::Ok();
}

ViewportRect D3D11Renderer::CalculateLetterbox(
    uint32_t clientW, uint32_t clientH,
    uint32_t videoW, uint32_t videoH) const noexcept {

    ViewportRect vp{};
    if (clientW == 0 || clientH == 0 || videoW == 0 || videoH == 0) {
        vp.width = static_cast<float>(clientW);
        vp.height = static_cast<float>(clientH);
        return vp;
    }

    const float targetAspect = static_cast<float>(videoW) / static_cast<float>(videoH);
    const float clientAspect = static_cast<float>(clientW) / static_cast<float>(clientH);

    if (clientAspect > targetAspect) {
        // Window is wider than video: letterbox on left/right (pillarboxing)
        vp.height = static_cast<float>(clientH);
        vp.width = vp.height * targetAspect;
        vp.x = (static_cast<float>(clientW) - vp.width) * 0.5f;
        vp.y = 0.0f;
    } else {
        // Window is taller than video: letterbox on top/bottom
        vp.width = static_cast<float>(clientW);
        vp.height = vp.width / targetAspect;
        vp.x = 0.0f;
        vp.y = (static_cast<float>(clientH) - vp.height) * 0.5f;
    }

    return vp;
}

NormalizedPoint D3D11Renderer::WindowToNormalized(float clientX, float clientY) const noexcept {
    NormalizedPoint pt{};
    if (current_viewport_.width <= 0.0f || current_viewport_.height <= 0.0f) {
        pt.x = 0.5f;
        pt.y = 0.5f;
        pt.inside_viewport = false;
        return pt;
    }

    const float relX = clientX - current_viewport_.x;
    const float relY = clientY - current_viewport_.y;

    if (relX >= 0.0f && relX <= current_viewport_.width &&
        relY >= 0.0f && relY <= current_viewport_.height) {
        pt.inside_viewport = true;
    } else {
        pt.inside_viewport = false;
    }

    pt.x = std::clamp(relX / current_viewport_.width, 0.0f, 1.0f);
    pt.y = std::clamp(relY / current_viewport_.height, 0.0f, 1.0f);
    return pt;
}

void D3D11Renderer::Shutdown() {
    initialized_ = false;
    CleanupRenderTarget();
    current_srv_.Reset();
    current_keyed_mutex_.Reset();
    current_texture_.Reset();
    sampler_state_.Reset();
    pixel_shader_.Reset();
    vertex_shader_.Reset();
    swap_chain_.Reset();
    context_.Reset();
    device_.Reset();
}

} // namespace CarPlayStation::UI
