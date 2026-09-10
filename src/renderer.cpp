#include "renderer.h"
#include "shaders.h"

#include <d3dcompiler.h>
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

namespace
{
constexpr uint32_t SrvFont = 0;
constexpr uint32_t SrvCloth = 1;
constexpr uint32_t SrvAlbedo = 2;
constexpr uint32_t SrvNormalMaterial = 3;
constexpr uint32_t SrvEffectsVBuffer = 4;
constexpr uint32_t SrvDepth = 5;
constexpr uint32_t SrvSignedDelta = 6;
constexpr uint32_t UavEffectBase = 7;
constexpr uint32_t UavEffectStride = 3;
constexpr uint32_t EffectCount = 4;
constexpr uint32_t TileSize = 8;
constexpr DXGI_FORMAT ColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT IntermediateFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT EffectsFormat = DXGI_FORMAT_R32G32B32A32_UINT;

constexpr uint32_t UavDelta(uint32_t effectIndex) { return UavEffectBase + effectIndex * UavEffectStride; }
constexpr uint32_t UavTileList(uint32_t effectIndex) { return UavDelta(effectIndex) + 1; }
constexpr uint32_t UavArguments(uint32_t effectIndex) { return UavDelta(effectIndex) + 2; }

void Check(HRESULT result, const char* operation)
{
    if (FAILED(result))
        throw std::runtime_error(std::string(operation) + " failed (HRESULT " + std::to_string(static_cast<uint32_t>(result)) + ")");
}

D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

D3D12_RESOURCE_DESC BufferDesc(uint64_t size)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

D3D12_RESOURCE_DESC UavBufferDesc(uint64_t size)
{
    D3D12_RESOURCE_DESC desc = BufferDesc(size);
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return desc;
}

ComPtr<ID3DBlob> Compile(const char* entry, const char* target)
{
    ComPtr<ID3DBlob> shader;
    ComPtr<ID3DBlob> errors;
    const UINT flags =
#if defined(_DEBUG)
        D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
    const HRESULT result = D3DCompile(kShaderSource, std::strlen(kShaderSource), "embedded.hlsl", nullptr, nullptr,
        entry, target, flags, 0, &shader, &errors);
    if (FAILED(result))
    {
        const std::string detail = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "";
        throw std::runtime_error("Shader compilation failed for " + std::string(entry) + ":\n" + detail);
    }
    return shader;
}

D3D12_BLEND_DESC OpaqueBlend()
{
    D3D12_BLEND_DESC desc{};
    for (D3D12_RENDER_TARGET_BLEND_DESC& target : desc.RenderTarget)
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    return desc;
}

D3D12_RASTERIZER_DESC Rasterizer()
{
    D3D12_RASTERIZER_DESC desc{};
    desc.FillMode = D3D12_FILL_MODE_SOLID;
    desc.CullMode = D3D12_CULL_MODE_NONE;
    desc.FrontCounterClockwise = FALSE;
    desc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    desc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    desc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    desc.DepthClipEnable = TRUE;
    return desc;
}

D3D12_DEPTH_STENCIL_DESC DepthState(bool enabled)
{
    D3D12_DEPTH_STENCIL_DESC desc{};
    desc.DepthEnable = enabled;
    desc.DepthWriteMask = enabled ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    desc.StencilEnable = FALSE;
    return desc;
}
}

Renderer::Renderer(HWND window, uint32_t width, uint32_t height)
    : m_width(width), m_height(height), m_lastTick(std::chrono::steady_clock::now())
{
    CreateDeviceResources(window);
    CreatePipelineResources();
    CreateGeometryResources();
    CreateTexture();
    CreateSizeResources();
    CreateImGui(window);
}

Renderer::~Renderer()
{
    if (m_device)
        WaitForGpu();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    if (ImGui::GetCurrentContext())
        ImGui::DestroyContext();
    for (FrameContext& frame : m_frames)
        if (frame.constants)
            frame.constants->Unmap(0, nullptr);
    if (m_fenceEvent)
        CloseHandle(m_fenceEvent);
}

void Renderer::CreateDeviceResources(HWND window)
{
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        debug->EnableDebugLayer();
#endif
    UINT factoryFlags = 0;
#if defined(_DEBUG)
    factoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif
    Check(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory)), "CreateDXGIFactory2");

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; m_factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
             IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++index)
    {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
            break;
        adapter.Reset();
    }
    Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)), "D3D12CreateDevice");

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Check(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_queue)), "CreateCommandQueue");

    DXGI_SWAP_CHAIN_DESC1 swapDesc{};
    swapDesc.Width = m_width;
    swapDesc.Height = m_height;
    swapDesc.Format = ColorFormat;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = FrameCount;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swapChain;
    Check(m_factory->CreateSwapChainForHwnd(m_queue.Get(), window, &swapDesc, nullptr, nullptr, &swapChain), "CreateSwapChainForHwnd");
    Check(swapChain.As(&m_swapChain), "Query IDXGISwapChain3");
    m_factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = FrameCount + 3;
    Check(m_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap)), "Create RTV heap");
    m_rtvStride = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
    dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvDesc.NumDescriptors = 1;
    Check(m_device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&m_dsvHeap)), "Create DSV heap");

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.NumDescriptors = 24;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Check(m_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&m_srvHeap)), "Create SRV heap");
    m_srvStride = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (FrameContext& frame : m_frames)
    {
        Check(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator)), "CreateCommandAllocator");
        const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
        const D3D12_RESOURCE_DESC cbDesc = BufferDesc(512);
        Check(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&frame.constants)), "Create constant buffer");
        Check(frame.constants->Map(0, nullptr, reinterpret_cast<void**>(&frame.constantsMapped)), "Map constant buffer");
    }
    Check(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_frames[0].allocator.Get(),
        nullptr, IID_PPV_ARGS(&m_commandList)), "CreateCommandList");
    Check(m_commandList->Close(), "Close initial command list");
    Check(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)), "CreateFence");
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent)
        throw std::runtime_error("CreateEvent failed");
}

void Renderer::CreatePipelineResources()
{
    D3D12_DESCRIPTOR_RANGE graphicsRange{};
    graphicsRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    graphicsRange.NumDescriptors = 6;
    graphicsRange.BaseShaderRegister = 0;
    graphicsRange.OffsetInDescriptorsFromTableStart = 0;

    std::array<D3D12_ROOT_PARAMETER, 2> graphicsParameters{};
    graphicsParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    graphicsParameters[0].Descriptor.ShaderRegister = 0;
    graphicsParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    graphicsParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    graphicsParameters[1].DescriptorTable.NumDescriptorRanges = 1;
    graphicsParameters[1].DescriptorTable.pDescriptorRanges = &graphicsRange;
    graphicsParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    std::array<D3D12_STATIC_SAMPLER_DESC, 2> samplers{};
    for (uint32_t i = 0; i < samplers.size(); ++i)
    {
        samplers[i].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[i].AddressU = i == 0 ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].AddressV = samplers[i].AddressU;
        samplers[i].AddressW = samplers[i].AddressU;
        samplers[i].ShaderRegister = i;
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
    }

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = static_cast<UINT>(graphicsParameters.size());
    rootDesc.pParameters = graphicsParameters.data();
    rootDesc.NumStaticSamplers = static_cast<UINT>(samplers.size());
    rootDesc.pStaticSamplers = samplers.data();
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    Check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error), "Serialize root signature");
    Check(m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
        IID_PPV_ARGS(&m_graphicsRootSignature)), "Create graphics root signature");

    std::array<D3D12_DESCRIPTOR_RANGE, 2> computeRanges{};
    computeRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    computeRanges[0].NumDescriptors = 4;
    computeRanges[0].BaseShaderRegister = 0;
    computeRanges[0].OffsetInDescriptorsFromTableStart = 0;
    computeRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    computeRanges[1].NumDescriptors = 3;
    computeRanges[1].BaseShaderRegister = 0;
    computeRanges[1].OffsetInDescriptorsFromTableStart = 0;
    std::array<D3D12_ROOT_PARAMETER, 3> computeParameters{};
    computeParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    computeParameters[0].Descriptor.ShaderRegister = 0;
    computeParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    computeParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParameters[1].DescriptorTable.NumDescriptorRanges = 1;
    computeParameters[1].DescriptorTable.pDescriptorRanges = &computeRanges[0];
    computeParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    computeParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParameters[2].DescriptorTable.NumDescriptorRanges = 1;
    computeParameters[2].DescriptorTable.pDescriptorRanges = &computeRanges[1];
    computeParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC computeRootDesc{};
    computeRootDesc.NumParameters = static_cast<UINT>(computeParameters.size());
    computeRootDesc.pParameters = computeParameters.data();
    computeRootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    signature.Reset();
    error.Reset();
    Check(D3D12SerializeRootSignature(&computeRootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error),
        "Serialize compute root signature");
    Check(m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
        IID_PPV_ARGS(&m_computeRootSignature)), "Create compute root signature");

    const ComPtr<ID3DBlob> sailVs = Compile("SailVS", "vs_5_1");
    const ComPtr<ID3DBlob> staticSceneVs = Compile("StaticSceneVS", "vs_5_1");
    const ComPtr<ID3DBlob> sailPs = Compile("SailGBufferPS", "ps_5_1");
    const ComPtr<ID3DBlob> mastPs = Compile("MastGBufferPS", "ps_5_1");
    const ComPtr<ID3DBlob> fullscreenVs = Compile("FullscreenVS", "vs_5_1");
    const ComPtr<ID3DBlob> compositePs = Compile("CompositePS", "ps_5_1");
    const ComPtr<ID3DBlob> finalizeCs = Compile("FinalizeIndirectArgs", "cs_5_1");
    const std::array<ComPtr<ID3DBlob>, EffectCount> classifyShaders{
        Compile("ClassifyIridescentTiles", "cs_5_1"),
        Compile("ClassifyFrostTiles", "cs_5_1"),
        Compile("ClassifyLightningTiles", "cs_5_1"),
        Compile("ClassifyWarpTiles", "cs_5_1")};
    const std::array<ComPtr<ID3DBlob>, EffectCount> effectShaders{
        Compile("IridescentWindSheenCS", "cs_5_1"),
        Compile("FrostCrystalCS", "cs_5_1"),
        Compile("StormLightningCS", "cs_5_1"),
        Compile("WarpDistortionCS", "cs_5_1")};
    const std::array<D3D12_INPUT_ELEMENT_DESC, 3> layout{{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, normal), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, uv), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
    }};

    D3D12_GRAPHICS_PIPELINE_STATE_DESC base{};
    base.pRootSignature = m_graphicsRootSignature.Get();
    base.VS = {sailVs->GetBufferPointer(), sailVs->GetBufferSize()};
    base.BlendState = OpaqueBlend();
    base.SampleMask = UINT_MAX;
    base.RasterizerState = Rasterizer();
    base.DepthStencilState = DepthState(true);
    base.InputLayout = {layout.data(), static_cast<UINT>(layout.size())};
    base.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    base.NumRenderTargets = 3;
    base.RTVFormats[0] = ColorFormat;
    base.RTVFormats[1] = IntermediateFormat;
    base.RTVFormats[2] = EffectsFormat;
    base.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    base.SampleDesc.Count = 1;

    base.PS = {sailPs->GetBufferPointer(), sailPs->GetBufferSize()};
    Check(m_device->CreateGraphicsPipelineState(&base, IID_PPV_ARGS(&m_sailGBufferPso)), "Create sail G-buffer PSO");
    base.VS = {staticSceneVs->GetBufferPointer(), staticSceneVs->GetBufferSize()};
    base.PS = {mastPs->GetBufferPointer(), mastPs->GetBufferSize()};
    Check(m_device->CreateGraphicsPipelineState(&base, IID_PPV_ARGS(&m_mastGBufferPso)), "Create mast G-buffer PSO");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC fullscreen{};
    fullscreen.pRootSignature = m_graphicsRootSignature.Get();
    fullscreen.VS = {fullscreenVs->GetBufferPointer(), fullscreenVs->GetBufferSize()};
    fullscreen.PS = {compositePs->GetBufferPointer(), compositePs->GetBufferSize()};
    fullscreen.BlendState = OpaqueBlend();
    fullscreen.SampleMask = UINT_MAX;
    fullscreen.RasterizerState = Rasterizer();
    fullscreen.DepthStencilState = DepthState(false);
    fullscreen.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    fullscreen.NumRenderTargets = 1;
    fullscreen.RTVFormats[0] = IntermediateFormat;
    fullscreen.SampleDesc.Count = 1;
    fullscreen.RTVFormats[0] = ColorFormat;
    Check(m_device->CreateGraphicsPipelineState(&fullscreen, IID_PPV_ARGS(&m_compositePso)), "Create composite PSO");

    D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
    compute.pRootSignature = m_computeRootSignature.Get();
    for (uint32_t effectIndex = 0; effectIndex < EffectCount; ++effectIndex)
    {
        compute.CS = {
            classifyShaders[effectIndex]->GetBufferPointer(),
            classifyShaders[effectIndex]->GetBufferSize()};
        Check(m_device->CreateComputePipelineState(
            &compute, IID_PPV_ARGS(&m_classifyEffectPsos[effectIndex])), "Create classify PSO");
    }
    compute.CS = {finalizeCs->GetBufferPointer(), finalizeCs->GetBufferSize()};
    Check(m_device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&m_finalizeIndirectPso)), "Create indirect finalize PSO");
    for (uint32_t effectIndex = 0; effectIndex < EffectCount; ++effectIndex)
    {
        compute.CS = {
            effectShaders[effectIndex]->GetBufferPointer(),
            effectShaders[effectIndex]->GetBufferSize()};
        Check(m_device->CreateComputePipelineState(
            &compute, IID_PPV_ARGS(&m_effectPsos[effectIndex])), "Create effect PSO");
    }

    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC commandSignature{};
    commandSignature.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    commandSignature.NumArgumentDescs = 1;
    commandSignature.pArgumentDescs = &argument;
    Check(m_device->CreateCommandSignature(&commandSignature, nullptr,
        IID_PPV_ARGS(&m_dispatchCommandSignature)), "Create dispatch command signature");
}

void Renderer::CreateGeometryResources()
{
    m_sailIndexCount = static_cast<uint32_t>(m_sail.Indices().size());
    const uint64_t vertexBytes = m_sail.Vertices().size() * sizeof(Vertex);
    const uint64_t indexBytes = m_sail.Indices().size() * sizeof(uint32_t);
    const D3D12_HEAP_PROPERTIES upload = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC desc = BufferDesc(vertexBytes);
    Check(m_device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&m_sailVertexBuffer)), "Create sail vertex buffer");
    void* mapped = nullptr;
    Check(m_sailVertexBuffer->Map(0, nullptr, &mapped), "Map sail vertex buffer");
    std::memcpy(mapped, m_sail.Vertices().data(), static_cast<size_t>(vertexBytes));
    m_sailVertexBuffer->Unmap(0, nullptr);
    m_sailVbv = {m_sailVertexBuffer->GetGPUVirtualAddress(), static_cast<UINT>(vertexBytes), sizeof(Vertex)};
    desc = BufferDesc(indexBytes);
    Check(m_device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&m_sailIndexBuffer)), "Create sail index buffer");
    Check(m_sailIndexBuffer->Map(0, nullptr, &mapped), "Map sail index buffer");
    std::memcpy(mapped, m_sail.Indices().data(), static_cast<size_t>(indexBytes));
    m_sailIndexBuffer->Unmap(0, nullptr);
    m_sailIbv = {m_sailIndexBuffer->GetGPUVirtualAddress(), static_cast<UINT>(indexBytes), DXGI_FORMAT_R32_UINT};

    const std::array<Vertex, 24> mastVertices{{
        {{-.16f,-2.8f,-.16f},{0,0,-1},{0,0}},{{.16f,-2.8f,-.16f},{0,0,-1},{1,0}},{{.16f,2.8f,-.16f},{0,0,-1},{1,1}},{{-.16f,2.8f,-.16f},{0,0,-1},{0,1}},
        {{.16f,-2.8f,.16f},{0,0,1},{0,0}},{{-.16f,-2.8f,.16f},{0,0,1},{1,0}},{{-.16f,2.8f,.16f},{0,0,1},{1,1}},{{.16f,2.8f,.16f},{0,0,1},{0,1}},
        {{-.16f,-2.8f,.16f},{-1,0,0},{0,0}},{{-.16f,-2.8f,-.16f},{-1,0,0},{1,0}},{{-.16f,2.8f,-.16f},{-1,0,0},{1,1}},{{-.16f,2.8f,.16f},{-1,0,0},{0,1}},
        {{.16f,-2.8f,-.16f},{1,0,0},{0,0}},{{.16f,-2.8f,.16f},{1,0,0},{1,0}},{{.16f,2.8f,.16f},{1,0,0},{1,1}},{{.16f,2.8f,-.16f},{1,0,0},{0,1}},
        {{-.16f,2.8f,-.16f},{0,1,0},{0,0}},{{.16f,2.8f,-.16f},{0,1,0},{1,0}},{{.16f,2.8f,.16f},{0,1,0},{1,1}},{{-.16f,2.8f,.16f},{0,1,0},{0,1}},
        {{-.16f,-2.8f,.16f},{0,-1,0},{0,0}},{{.16f,-2.8f,.16f},{0,-1,0},{1,0}},{{.16f,-2.8f,-.16f},{0,-1,0},{1,1}},{{-.16f,-2.8f,-.16f},{0,-1,0},{0,1}}
    }};
    const std::array<uint16_t, 36> mastIndices{{
        0,1,2,0,2,3,4,5,6,4,6,7,8,9,10,8,10,11,
        12,13,14,12,14,15,16,17,18,16,18,19,20,21,22,20,22,23
    }};
    m_mastIndexCount = static_cast<uint32_t>(mastIndices.size());
    desc = BufferDesc(sizeof(mastVertices));
    Check(m_device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&m_mastVertexBuffer)), "Create mast vertex buffer");
    Check(m_mastVertexBuffer->Map(0, nullptr, &mapped), "Map mast vertex buffer");
    std::memcpy(mapped, mastVertices.data(), sizeof(mastVertices));
    m_mastVertexBuffer->Unmap(0, nullptr);
    desc = BufferDesc(sizeof(mastIndices));
    Check(m_device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&m_mastIndexBuffer)), "Create mast index buffer");
    Check(m_mastIndexBuffer->Map(0, nullptr, &mapped), "Map mast index buffer");
    std::memcpy(mapped, mastIndices.data(), sizeof(mastIndices));
    m_mastIndexBuffer->Unmap(0, nullptr);
    m_mastVbv = {m_mastVertexBuffer->GetGPUVirtualAddress(), sizeof(mastVertices), sizeof(Vertex)};
    m_mastIbv = {m_mastIndexBuffer->GetGPUVirtualAddress(), sizeof(mastIndices), DXGI_FORMAT_R16_UINT};
}

void Renderer::CreateTexture()
{
    constexpr uint32_t size = 128;
    std::vector<uint32_t> pixels(size * size);
    for (uint32_t y = 0; y < size; ++y)
    {
        for (uint32_t x = 0; x < size; ++x)
        {
            const float weave = ((x / 3 + y / 3) & 1) ? 0.92f : 0.76f;
            const float thread = (x % 8 == 0 || y % 8 == 0) ? 0.82f : 1.0f;
            const uint8_t r = static_cast<uint8_t>(215.0f * weave * thread);
            const uint8_t g = static_cast<uint8_t>(201.0f * weave * thread);
            const uint8_t b = static_cast<uint8_t>(159.0f * weave * thread);
            pixels[y * size + x] = 0xff000000u | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) | r;
        }
    }

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = size;
    textureDesc.Height = size;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = ColorFormat;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    Check(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_clothTexture)), "Create cloth texture");

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    uint64_t uploadBytes = 0;
    m_device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, nullptr, nullptr, &uploadBytes);
    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC uploadDesc = BufferDesc(uploadBytes);
    ComPtr<ID3D12Resource> upload;
    Check(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "Create texture upload");
    uint8_t* mapped = nullptr;
    Check(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "Map texture upload");
    for (uint32_t y = 0; y < size; ++y)
        std::memcpy(mapped + footprint.Offset + y * footprint.Footprint.RowPitch, pixels.data() + y * size, size * 4);
    upload->Unmap(0, nullptr);

    Check(m_frames[0].allocator->Reset(), "Reset upload allocator");
    Check(m_commandList->Reset(m_frames[0].allocator.Get(), nullptr), "Reset upload command list");
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = m_clothTexture.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;
    m_commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    Transition(m_clothTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Check(m_commandList->Close(), "Close texture upload");
    ID3D12CommandList* lists[] = {m_commandList.Get()};
    m_queue->ExecuteCommandLists(1, lists);
    WaitForGpu();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = ColorFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_clothTexture.Get(), &srv, DescriptorCpu(SrvCloth));
}

void Renderer::CreateSizeResources()
{
    for (uint32_t i = 0; i < FrameCount; ++i)
    {
        Check(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])), "Get swap-chain buffer");
        m_device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, Rtv(i));
    }

    D3D12_RESOURCE_DESC depthDesc{};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = m_width;
    depthDesc.Height = m_height;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE depthClear{};
    depthClear.Format = DXGI_FORMAT_D32_FLOAT;
    depthClear.DepthStencil.Depth = 1.0f;
    const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    Check(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &depthDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &depthClear, IID_PPV_ARGS(&m_depth)), "Create depth buffer");
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    m_device->CreateDepthStencilView(m_depth.Get(), &dsv, Dsv());
    D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv{};
    depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depthSrv.Format = DXGI_FORMAT_R32_FLOAT;
    depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depthSrv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_depth.Get(), &depthSrv, DescriptorCpu(SrvDepth));

    auto createTarget = [&](ComPtr<ID3D12Resource>& resource, DXGI_FORMAT format, uint32_t rtvIndex, uint32_t srvIndex)
    {
        D3D12_RESOURCE_DESC targetDesc{};
        targetDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        targetDesc.Width = m_width;
        targetDesc.Height = m_height;
        targetDesc.DepthOrArraySize = 1;
        targetDesc.MipLevels = 1;
        targetDesc.Format = format;
        targetDesc.SampleDesc.Count = 1;
        targetDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = format;
        Check(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &targetDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, IID_PPV_ARGS(&resource)), "Create intermediate target");
        m_device->CreateRenderTargetView(resource.Get(), nullptr, Rtv(rtvIndex));
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(resource.Get(), &srv, DescriptorCpu(srvIndex));
    };
    createTarget(m_baseAlbedo, ColorFormat, FrameCount, SrvAlbedo);
    createTarget(m_normalMaterial, IntermediateFormat, FrameCount + 1, SrvNormalMaterial);
    createTarget(m_effectsVBuffer, EffectsFormat, FrameCount + 2, SrvEffectsVBuffer);

    D3D12_RESOURCE_DESC deltaDesc{};
    deltaDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    deltaDesc.Width = m_width;
    deltaDesc.Height = m_height;
    deltaDesc.DepthOrArraySize = 1;
    deltaDesc.MipLevels = 1;
    deltaDesc.Format = IntermediateFormat;
    deltaDesc.SampleDesc.Count = 1;
    deltaDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Check(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &deltaDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m_signedDelta)), "Create signed delta");
    D3D12_SHADER_RESOURCE_VIEW_DESC deltaSrv{};
    deltaSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    deltaSrv.Format = IntermediateFormat;
    deltaSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    deltaSrv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_signedDelta.Get(), &deltaSrv, DescriptorCpu(SrvSignedDelta));
    D3D12_UNORDERED_ACCESS_VIEW_DESC deltaUav{};
    deltaUav.Format = IntermediateFormat;
    deltaUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    for (uint32_t effectIndex = 0; effectIndex < EffectCount; ++effectIndex)
        m_device->CreateUnorderedAccessView(
            m_signedDelta.Get(), nullptr, &deltaUav, DescriptorCpu(UavDelta(effectIndex)));

    const uint32_t tileCapacity = ((m_width + TileSize - 1) / TileSize) * ((m_height + TileSize - 1) / TileSize);
    const D3D12_RESOURCE_DESC tileListDesc = UavBufferDesc(static_cast<uint64_t>(tileCapacity) * sizeof(uint32_t));
    D3D12_UNORDERED_ACCESS_VIEW_DESC listUav{};
    listUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    listUav.Format = DXGI_FORMAT_UNKNOWN;
    listUav.Buffer.NumElements = tileCapacity;
    listUav.Buffer.StructureByteStride = sizeof(uint32_t);

    const D3D12_RESOURCE_DESC argsDesc = UavBufferDesc(sizeof(D3D12_DISPATCH_ARGUMENTS));
    D3D12_UNORDERED_ACCESS_VIEW_DESC argsUav{};
    argsUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    argsUav.Format = DXGI_FORMAT_UNKNOWN;
    argsUav.Buffer.NumElements = 3;
    argsUav.Buffer.StructureByteStride = sizeof(uint32_t);
    for (uint32_t effectIndex = 0; effectIndex < EffectCount; ++effectIndex)
    {
        Check(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &tileListDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&m_effectTileLists[effectIndex])), "Create effect tile list");
        m_device->CreateUnorderedAccessView(
            m_effectTileLists[effectIndex].Get(), nullptr, &listUav,
            DescriptorCpu(UavTileList(effectIndex)));

        Check(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &argsDesc,
            D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, nullptr,
            IID_PPV_ARGS(&m_indirectArgs[effectIndex])), "Create indirect arguments");
        m_device->CreateUnorderedAccessView(
            m_indirectArgs[effectIndex].Get(), nullptr, &argsUav,
            DescriptorCpu(UavArguments(effectIndex)));
    }
}

void Renderer::DestroySizeResources()
{
    for (auto& buffer : m_backBuffers)
        buffer.Reset();
    m_depth.Reset();
    m_baseAlbedo.Reset();
    m_normalMaterial.Reset();
    m_effectsVBuffer.Reset();
    m_signedDelta.Reset();
    for (uint32_t effectIndex = 0; effectIndex < EffectCount; ++effectIndex)
    {
        m_effectTileLists[effectIndex].Reset();
        m_indirectArgs[effectIndex].Reset();
    }
}

void Renderer::CreateImGui(HWND window)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    if (!ImGui_ImplWin32_Init(window))
        throw std::runtime_error("ImGui Win32 initialization failed");

    ImGui_ImplDX12_InitInfo initInfo{};
    initInfo.Device = m_device.Get();
    initInfo.CommandQueue = m_queue.Get();
    initInfo.NumFramesInFlight = FrameCount;
    initInfo.RTVFormat = ColorFormat;
    initInfo.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    initInfo.SrvDescriptorHeap = m_srvHeap.Get();
    initInfo.LegacySingleSrvCpuDescriptor = DescriptorCpu(SrvFont);
    initInfo.LegacySingleSrvGpuDescriptor = DescriptorGpu(SrvFont);
    if (!ImGui_ImplDX12_Init(&initInfo))
        throw std::runtime_error("ImGui DirectX 12 initialization failed");
}

void Renderer::Update()
{
    const auto now = std::chrono::steady_clock::now();
    m_deltaTime = std::min(0.1f, std::chrono::duration<float>(now - m_lastTick).count());
    m_lastTick = now;
    if (!m_paused)
        m_time += m_deltaTime;
    const XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(10.5f, 1.2f, 15.5f, 1.0f),
        XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f), XMVectorSet(0, 1, 0, 0));
    const XMMATRIX projection = XMMatrixPerspectiveFovLH(XMConvertToRadians(50.0f),
        static_cast<float>(m_width) / std::max(1u, m_height), 0.1f, 100.0f);
    const XMMATRIX viewProjection = view * projection;
    FrameConstants constants{};
    XMStoreFloat4x4(&constants.view, view);
    XMStoreFloat4x4(&constants.projection, projection);
    XMStoreFloat4x4(&constants.viewProjection, viewProjection);
    XMStoreFloat4x4(&constants.inverseViewProjection, XMMatrixInverse(nullptr, viewProjection));
    constants.cameraTime = {10.5f, 1.2f, 15.5f, m_time};
    constants.viewportDelta = {static_cast<float>(m_width), static_cast<float>(m_height), m_deltaTime, 0.0f};
    constants.wind = {std::cos(m_sailParameters.windAngle), std::sin(m_sailParameters.windAngle),
        m_sailParameters.windSpeed / 12.0f, m_sailParameters.gustAmount};
    constants.deformation = {m_sailParameters.billowAmplitude, m_sailParameters.wavelength,
        m_sailParameters.travelSpeed, m_sailParameters.tensionResponse};
    constants.dynamics = {m_sailParameters.relaxation, m_sailParameters.sag,
        m_sailParameters.gustFrequency, 0.0f};
    constants.effectDirectionStrength = {
        m_effectParameters.direction.x, m_effectParameters.direction.y,
        m_effectParameters.direction.z, m_effectParameters.signedStrength};
    constants.sailEffectIds = {
        static_cast<uint32_t>(m_effectParameters.sailIds[0]),
        static_cast<uint32_t>(m_effectParameters.sailIds[1]),
        0u,
        0u};
    constants.debugMode = static_cast<uint32_t>(m_debugMode);
    constants.tileCountX = (m_width + TileSize - 1) / TileSize;
    constants.tileCountY = (m_height + TileSize - 1) / TileSize;
    std::memcpy(m_frames[m_frameIndex].constantsMapped, &constants, sizeof(constants));
}

void Renderer::DrawUi()
{
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImGui::Begin("Animated Sail");
    ImGui::Text("%.2f ms (%.1f FPS)", m_deltaTime * 1000.0f, m_deltaTime > 0 ? 1.0f / m_deltaTime : 0.0f);
    ImGui::Text("Deferred buffers: %u x %u", m_width, m_height);
    ImGui::SeparatorText("Wind and cloth");
    ImGui::SliderAngle("Wind direction", &m_sailParameters.windAngle, -180.0f, 180.0f);
    ImGui::SliderFloat("Wind speed", &m_sailParameters.windSpeed, 0.0f, 12.0f);
    ImGui::SliderFloat("Gust amount", &m_sailParameters.gustAmount, 0.0f, 1.0f);
    ImGui::SliderFloat("Gust frequency", &m_sailParameters.gustFrequency, 0.05f, 3.0f);
    ImGui::SliderFloat("Billow amplitude", &m_sailParameters.billowAmplitude, 0.0f, 1.0f);
    ImGui::SliderFloat("Wavelength", &m_sailParameters.wavelength, 0.25f, 3.0f);
    ImGui::SliderFloat("Travel speed", &m_sailParameters.travelSpeed, 0.0f, 4.0f);
    ImGui::SliderFloat("Tension response", &m_sailParameters.tensionResponse, 0.0f, 1.0f);
    ImGui::SliderFloat("Relaxation", &m_sailParameters.relaxation, 0.0f, 1.0f);
    ImGui::SliderFloat("Low-wind sag", &m_sailParameters.sag, 0.0f, 1.0f);
    ImGui::SeparatorText("Per-effect compute");
    const char* effectIds[] = {
        "0 - None",
        "1 - Iridescent wind sheen",
        "2 - Frost crystal",
        "3 - Storm lightning",
        "4 - Smiley warp distortion"};
    ImGui::Combo("Left sail effect", &m_effectParameters.sailIds[0], effectIds, IM_ARRAYSIZE(effectIds));
    ImGui::Combo("Right sail effect", &m_effectParameters.sailIds[1], effectIds, IM_ARRAYSIZE(effectIds));
    ImGui::SliderFloat3("Effect direction (view)", &m_effectParameters.direction.x, -1.0f, 1.0f);
    ImGui::SliderFloat("Signed strength", &m_effectParameters.signedStrength, -3.0f, 3.0f);
    ImGui::TextUnformatted("Warp reaches full replacement at |strength| = 1");
    ImGui::TextUnformatted("8x8 tiles, GPU indirect dispatch; no CPU readback");
    const char* modes[] = {
        "Final composite",
        "Base albedo",
        "View normal / material",
        "V-buffer overview",
        "V-buffer: effect ID",
        "V-buffer: primitive ID",
        "V-buffer: logical UV",
        "V-buffer: barycentrics",
        "Signed delta",
        "Debug overview",
        "Dispatch tiles by effect ID"};
    ImGui::Combo("Debug view", &m_debugMode, modes, IM_ARRAYSIZE(modes));
    if (ImGui::Button(m_paused ? "Resume" : "Pause"))
        m_paused = !m_paused;
    ImGui::SameLine();
    if (ImGui::Button("Reset time"))
        m_time = 0.0f;
    ImGui::SameLine();
    if (ImGui::Button("Reset parameters"))
    {
        m_sailParameters = {};
        m_effectParameters = {};
    }
    ImGui::End();
    ImGui::Render();
}

void Renderer::PopulateCommands()
{
    FrameContext& frame = m_frames[m_frameIndex];
    Check(frame.allocator->Reset(), "Reset frame allocator");
    Check(m_commandList->Reset(frame.allocator.Get(), nullptr), "Reset frame command list");
    ID3D12DescriptorHeap* heaps[] = {m_srvHeap.Get()};
    m_commandList->SetDescriptorHeaps(1, heaps);
    m_commandList->SetGraphicsRootSignature(m_graphicsRootSignature.Get());
    m_commandList->SetGraphicsRootConstantBufferView(0, frame.constants->GetGPUVirtualAddress());
    m_commandList->SetGraphicsRootDescriptorTable(1, DescriptorGpu(SrvCloth));
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height)};
    m_commandList->RSSetViewports(1, &viewport);
    m_commandList->RSSetScissorRects(1, &scissor);

    Transition(m_baseAlbedo.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Transition(m_normalMaterial.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Transition(m_effectsVBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Transition(m_depth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const float zero[4]{};
    const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 3> gbufferRtvs{
        Rtv(FrameCount), Rtv(FrameCount + 1), Rtv(FrameCount + 2)};
    const D3D12_CPU_DESCRIPTOR_HANDLE depthDsv = Dsv();
    m_commandList->ClearRenderTargetView(gbufferRtvs[0], zero, 0, nullptr);
    m_commandList->ClearRenderTargetView(gbufferRtvs[1], zero, 0, nullptr);
    m_commandList->ClearRenderTargetView(gbufferRtvs[2], zero, 0, nullptr);
    m_commandList->ClearDepthStencilView(Dsv(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    m_commandList->OMSetRenderTargets(3, gbufferRtvs.data(), FALSE, &depthDsv);
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->SetPipelineState(m_mastGBufferPso.Get());
    m_commandList->IASetVertexBuffers(0, 1, &m_mastVbv);
    m_commandList->IASetIndexBuffer(&m_mastIbv);
    m_commandList->DrawIndexedInstanced(m_mastIndexCount, 2, 0, 0, 0);
    m_commandList->SetPipelineState(m_sailGBufferPso.Get());
    m_commandList->IASetVertexBuffers(0, 1, &m_sailVbv);
    m_commandList->IASetIndexBuffer(&m_sailIbv);
    m_commandList->DrawIndexedInstanced(m_sailIndexCount, 2, 0, 0, 0);
    Transition(m_baseAlbedo.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(m_normalMaterial.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(m_effectsVBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(m_depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    Transition(m_signedDelta.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_commandList->ClearUnorderedAccessViewFloat(
        DescriptorGpu(UavDelta(0)), DescriptorCpu(UavDelta(0)), m_signedDelta.Get(), zero, 0, nullptr);
    const UINT clearArgs[4]{};
    for (uint32_t effectIndex = 0; effectIndex < EffectCount; ++effectIndex)
    {
        Transition(m_indirectArgs[effectIndex].Get(),
            D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_commandList->ClearUnorderedAccessViewUint(
            DescriptorGpu(UavArguments(effectIndex)), DescriptorCpu(UavArguments(effectIndex)),
            m_indirectArgs[effectIndex].Get(), clearArgs, 0, nullptr);
    }

    m_commandList->SetComputeRootSignature(m_computeRootSignature.Get());
    m_commandList->SetComputeRootConstantBufferView(0, frame.constants->GetGPUVirtualAddress());
    m_commandList->SetComputeRootDescriptorTable(1, DescriptorGpu(SrvAlbedo));
    for (uint32_t effectIndex = 0; effectIndex < EffectCount; ++effectIndex)
    {
        m_commandList->SetComputeRootDescriptorTable(2, DescriptorGpu(UavDelta(effectIndex)));
        m_commandList->SetPipelineState(m_classifyEffectPsos[effectIndex].Get());
        m_commandList->Dispatch(
            (m_width + TileSize - 1) / TileSize,
            (m_height + TileSize - 1) / TileSize, 1);
        UavBarrier(m_indirectArgs[effectIndex].Get());
        m_commandList->SetPipelineState(m_finalizeIndirectPso.Get());
        m_commandList->Dispatch(1, 1, 1);
        UavBarrier(m_effectTileLists[effectIndex].Get());
        Transition(m_indirectArgs[effectIndex].Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        m_commandList->SetPipelineState(m_effectPsos[effectIndex].Get());
        m_commandList->ExecuteIndirect(
            m_dispatchCommandSignature.Get(), 1, m_indirectArgs[effectIndex].Get(), 0, nullptr, 0);
        UavBarrier(m_signedDelta.Get());
    }

    Transition(m_baseAlbedo.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(m_normalMaterial.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(m_effectsVBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(m_depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(m_signedDelta.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    Transition(m_backBuffers[m_frameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE backRtv = Rtv(m_frameIndex);
    const float background[4]{0.035f, 0.075f, 0.12f, 1.0f};
    m_commandList->ClearRenderTargetView(backRtv, background, 0, nullptr);
    m_commandList->OMSetRenderTargets(1, &backRtv, FALSE, nullptr);
    m_commandList->SetGraphicsRootSignature(m_graphicsRootSignature.Get());
    m_commandList->SetGraphicsRootConstantBufferView(0, frame.constants->GetGPUVirtualAddress());
    m_commandList->SetGraphicsRootDescriptorTable(1, DescriptorGpu(SrvCloth));
    m_commandList->SetPipelineState(m_compositePso.Get());
    m_commandList->IASetVertexBuffers(0, 0, nullptr);
    m_commandList->IASetIndexBuffer(nullptr);
    m_commandList->DrawInstanced(3, 1, 0, 0);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_commandList.Get());
    Transition(m_backBuffers[m_frameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    Check(m_commandList->Close(), "Close frame command list");
}

void Renderer::Render()
{
    if (m_minimized || m_width == 0 || m_height == 0)
        return;
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    WaitForFrame(m_frames[m_frameIndex]);
    Update();
    DrawUi();
    PopulateCommands();
    ID3D12CommandList* lists[] = {m_commandList.Get()};
    m_queue->ExecuteCommandLists(1, lists);
    Check(m_swapChain->Present(1, 0), "Present");
    FrameContext& frame = m_frames[m_frameIndex];
    frame.fenceValue = m_nextFenceValue++;
    Check(m_queue->Signal(m_fence.Get(), frame.fenceValue), "Signal frame fence");
}

void Renderer::Resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
    {
        m_minimized = true;
        return;
    }
    m_minimized = false;
    if (width == m_width && height == m_height)
        return;
    WaitForGpu();
    DestroySizeResources();
    m_width = width;
    m_height = height;
    Check(m_swapChain->ResizeBuffers(FrameCount, width, height, ColorFormat, 0), "ResizeBuffers");
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    CreateSizeResources();
}

LRESULT Renderer::HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    return ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam) ? 1 : 0;
}

void Renderer::WaitForFrame(FrameContext& frame)
{
    if (frame.fenceValue != 0 && m_fence->GetCompletedValue() < frame.fenceValue)
    {
        Check(m_fence->SetEventOnCompletion(frame.fenceValue, m_fenceEvent), "SetEventOnCompletion");
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void Renderer::WaitForGpu()
{
    if (!m_queue || !m_fence)
        return;
    const uint64_t value = m_nextFenceValue++;
    Check(m_queue->Signal(m_fence.Get(), value), "Signal GPU fence");
    Check(m_fence->SetEventOnCompletion(value, m_fenceEvent), "Set GPU completion event");
    WaitForSingleObject(m_fenceEvent, INFINITE);
    for (FrameContext& frame : m_frames)
        frame.fenceValue = 0;
}

void Renderer::Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);
}

void Renderer::UavBarrier(ID3D12Resource* resource)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    m_commandList->ResourceBarrier(1, &barrier);
}

D3D12_CPU_DESCRIPTOR_HANDLE Renderer::Rtv(uint32_t index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * m_rtvStride;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE Renderer::Dsv() const
{
    return m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
}

D3D12_CPU_DESCRIPTOR_HANDLE Renderer::DescriptorCpu(uint32_t index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * m_srvStride;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE Renderer::DescriptorGpu(uint32_t index) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(index) * m_srvStride;
    return handle;
}
