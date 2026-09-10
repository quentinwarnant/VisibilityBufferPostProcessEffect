#pragma once

#include "sail_mesh.h"

#include <DirectXMath.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <chrono>
#include <cstdint>

class Renderer
{
public:
    static constexpr uint32_t FrameCount = 2;
    Renderer(HWND window, uint32_t width, uint32_t height);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void Render();
    void Resize(uint32_t width, uint32_t height);
    LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

private:
    struct FrameContext
    {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12Resource> constants;
        uint8_t* constantsMapped = nullptr;
        uint64_t fenceValue = 0;
    };

    struct FrameConstants
    {
        DirectX::XMFLOAT4X4 view;
        DirectX::XMFLOAT4X4 projection;
        DirectX::XMFLOAT4X4 viewProjection;
        DirectX::XMFLOAT4X4 inverseViewProjection;
        DirectX::XMFLOAT4 cameraTime;
        DirectX::XMFLOAT4 viewportDelta;
        DirectX::XMFLOAT4 wind;
        DirectX::XMFLOAT4 deformation;
        DirectX::XMFLOAT4 dynamics;
        DirectX::XMFLOAT4 effectDirectionStrength;
        DirectX::XMUINT4 sailEffectIds;
        uint32_t debugMode;
        uint32_t tileCountX;
        uint32_t tileCountY;
        uint32_t padding;
    };

    struct EffectParameters
    {
        int sailIds[2]{1, 2};
        DirectX::XMFLOAT3 direction{-0.35f, 0.65f, -0.7f};
        float signedStrength = 3.0f;
    };

    void CreateDeviceResources(HWND window);
    void CreatePipelineResources();
    void CreateSizeResources();
    void DestroySizeResources();
    void CreateGeometryResources();
    void CreateTexture();
    void CreateImGui(HWND window);
    void Update();
    void PopulateCommands();
    void DrawUi();
    void WaitForGpu();
    void WaitForFrame(FrameContext& frame);
    void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
    void UavBarrier(ID3D12Resource* resource);
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(uint32_t index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE Dsv() const;
    D3D12_CPU_DESCRIPTOR_HANDLE DescriptorCpu(uint32_t index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE DescriptorGpu(uint32_t index) const;

    uint32_t m_width;
    uint32_t m_height;
    bool m_minimized = false;
    bool m_paused = false;
    uint32_t m_frameIndex = 0;
    uint64_t m_nextFenceValue = 1;
    HANDLE m_fenceEvent = nullptr;
    float m_time = 0.0f;
    float m_deltaTime = 0.0f;
    EffectParameters m_effectParameters{};
    int m_debugMode = 0;
    SailParameters m_sailParameters{};
    SailMesh m_sail;
    std::chrono::steady_clock::time_point m_lastTick;

    Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    uint32_t m_rtvStride = 0;
    uint32_t m_srvStride = 0;
    std::array<FrameContext, FrameCount> m_frames;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, FrameCount> m_backBuffers;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depth;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_baseAlbedo;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_normalMaterial;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_effectsVBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_signedDelta;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 4> m_effectTileLists;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 4> m_indirectArgs;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_graphicsRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_computeRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_sailGBufferPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_mastGBufferPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_compositePso;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4> m_classifyEffectPsos;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_finalizeIndirectPso;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4> m_effectPsos;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_dispatchCommandSignature;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sailVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sailIndexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_mastVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_mastIndexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_clothTexture;
    D3D12_VERTEX_BUFFER_VIEW m_sailVbv{};
    D3D12_INDEX_BUFFER_VIEW m_sailIbv{};
    D3D12_VERTEX_BUFFER_VIEW m_mastVbv{};
    D3D12_INDEX_BUFFER_VIEW m_mastIbv{};
    uint32_t m_sailIndexCount = 0;
    uint32_t m_mastIndexCount = 0;
};
