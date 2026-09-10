#include "sail_mesh.h"

SailMesh::SailMesh(uint32_t columns, uint32_t rows)
    : m_vertices((columns + 1) * (rows + 1))
{
    for (uint32_t y = 0; y <= rows; ++y)
    {
        for (uint32_t x = 0; x <= columns; ++x)
        {
            const float u = static_cast<float>(x) / columns;
            const float v = static_cast<float>(y) / rows;
            Vertex& vertex = m_vertices[y * (columns + 1) + x];
            vertex.position = {u * 4.5f, 2.25f - v * 4.5f, 0.0f};
            vertex.normal = {0.0f, 0.0f, -1.0f};
            vertex.uv = {u * 3.0f, v * 3.0f};
        }
    }

    m_indices.reserve(columns * rows * 6);
    for (uint32_t y = 0; y < rows; ++y)
    {
        for (uint32_t x = 0; x < columns; ++x)
        {
            const uint32_t a = y * (columns + 1) + x;
            const uint32_t b = a + 1;
            const uint32_t c = a + columns + 1;
            const uint32_t d = c + 1;
            m_indices.insert(m_indices.end(), {a, c, b, b, c, d});
        }
    }
}
