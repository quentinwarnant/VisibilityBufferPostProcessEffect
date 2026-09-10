#pragma once

#include <DirectXMath.h>
#include <cstdint>
#include <vector>

struct Vertex
{
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT2 uv;
};

struct SailParameters
{
    float windAngle = 0.8552113f;
    float windSpeed = 5.928f;
    float gustAmount = 0.278f;
    float gustFrequency = 0.786f;
    float billowAmplitude = 1.0f;
    float wavelength = 1.234f;
    float travelSpeed = 3.442f;
    float tensionResponse = 0.756f;
    float relaxation = 0.551f;
    float sag = 1.0f;
};

class SailMesh
{
public:
    SailMesh(uint32_t columns = 48, uint32_t rows = 36);
    const std::vector<Vertex>& Vertices() const { return m_vertices; }
    const std::vector<uint32_t>& Indices() const { return m_indices; }

private:
    std::vector<Vertex> m_vertices;
    std::vector<uint32_t> m_indices;
};
