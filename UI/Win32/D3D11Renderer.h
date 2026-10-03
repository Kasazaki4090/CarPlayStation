#pragma once

#include "Core/Common/Result.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdint>
#include <mutex>

namespace CarPlayStation::UI {

struct ViewportRect {
    float x{0.0f};
    float y{0.0f};
    float width{1280.0f};
    float height{720.0f};
};

struct NormalizedPoint {
    float x{0.5f};
    float y{0.5f};
    bool inside_viewport{true};
};

class D3D11Renderer {
public:
    D3D11Renderer();
    ~D3D11Renderer();

    D3D11Renderer(const D3D11Renderer&) = delete;
    D3D11Renderer& operator=(const D3D11Renderer&) = delete;

    /**
     * @brief Initialize D3D11 Device, SwapChain, shaders, and render targets.
     * @param hwnd Target Win32 window handle.
     * @param initialWidth Client area width.
     * @param initialHeight Client area height.
     */
    [[nodiscard]] Core::Common::Status Init(HWND hwnd, uint32_t initialWidth, uint32_t initialHeight);

    /**
     * @brief Resize swapchain buffers upon WM_SIZE / WM_DPICHANGED.
     */
    [[nodiscard]] Core::Common::Status Resize(uint32_t newWidth, uint32_t newHeight);

    /**
     * @brief Callback invoked by the video decoder when a new frame texture is ready.
     */
    void ConsumeFrame(uintptr_t textureHandle, uint32_t width, uint32_t height, uint64_t ptsUs);

    /**
     * @brief Render the current video frame into the SwapChain with Letterbox letterboxing.
     * @param autoPresent If true, Present() is called immediately. If false, caller must call Present() after any overlay drawing.
     */
    [[nodiscard]] Core::Common::Status Render(bool autoPresent = false);

    /**
     * @brief Present the SwapChain backbuffer to the screen (VSync: syncInterval=1).
     */
    [[nodiscard]] Core::Common::Status Present();

    /**
     * @brief Release all D3D11 / DXGI resources.
     */
    void Shutdown();

    // --- Geometry & Coordinate Mapping Helpers ---
    [[nodiscard]] ViewportRect CalculateLetterbox(
        uint32_t clientW, uint32_t clientH,
        uint32_t videoW, uint32_t videoH) const noexcept;

    [[nodiscard]] NormalizedPoint WindowToNormalized(float clientX, float clientY) const noexcept;

    // --- Accessors ---
    [[nodiscard]] ID3D11Device* GetDevice() const noexcept { return device_.Get(); }
    [[nodiscard]] ID3D11DeviceContext* GetContext() const noexcept { return context_.Get(); }
    [[nodiscard]] IDXGISwapChain1* GetSwapChain() const noexcept { return swap_chain_.Get(); }
    [[nodiscard]] ViewportRect GetCurrentViewport() const noexcept { return current_viewport_; }
    // Index of the swapchain buffer that is the current back buffer. For a
    // flip-model swapchain this is BufferCount-1 (buffer 0 is the front buffer
    // on screen); for a discard-model swapchain it is 0. Both the D3D11 video
    // and the D2D overlay must draw into this buffer, and Present() flips it.
    [[nodiscard]] uint32_t GetBackBufferIndex() const noexcept { return back_buffer_index_; }
    [[nodiscard]] uint64_t GetPresentedFrameCount() const noexcept { return presented_frame_count_; }
    [[nodiscard]] bool HasActiveFrame() const noexcept { return active_frame_available_; }

private:
    [[nodiscard]] Core::Common::Status CreateShaders();
    [[nodiscard]] Core::Common::Status CreateRenderTarget();
    void CleanupRenderTarget();
    void UpdateSharedTexture(uintptr_t handle, uint32_t width, uint32_t height);

    HWND hwnd_{nullptr};
    uint32_t client_width_{1280};
    uint32_t client_height_{720};
    uint32_t video_width_{1280};
    uint32_t video_height_{720};
    ViewportRect current_viewport_{};

    // D3D11 & DXGI interfaces
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> render_target_view_;

    // Presentation pipeline
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_state_;

    // Active video frame texture & SRV
    uintptr_t last_texture_handle_{0};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> current_texture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> current_srv_;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> current_keyed_mutex_;

    mutable std::mutex frame_mutex_;
    uint32_t back_buffer_index_{0};
    bool active_frame_available_{false};
    uint64_t presented_frame_count_{0};
    bool initialized_{false};
};

} // namespace CarPlayStation::UI
