#pragma once

#include <DirectXMath.h>

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace walnut
{
    // An axis-aligned bounding box.
    struct Aabb
    {
        DirectX::XMFLOAT3 min{}, max{};

        // Corner i (0..7): bit 0 picks x, bit 1 y, bit 2 z (0 = min, 1 = max).
        DirectX::XMFLOAT3 Corner(int i) const
        {
            return { (i & 1) ? max.x : min.x, (i & 2) ? max.y : min.y, (i & 4) ? max.z : min.z };
        }

        DirectX::XMFLOAT3 Size() const { return { max.x - min.x, max.y - min.y, max.z - min.z }; }

        // Length of the diagonal: the diameter of the box's bounding sphere.
        float Diagonal() const
        {
            using namespace DirectX;
            return XMVectorGetX(XMVector3Length(XMVectorSubtract(XMLoadFloat3(&max), XMLoadFloat3(&min))));
        }

        // Distance from a point to the box's farthest corner.
        float FarthestCornerDistance(const DirectX::XMFLOAT3& point) const
        {
            float farthest = 0.0f;
            for (int i = 0; i < 8; ++i)
            {
                const DirectX::XMFLOAT3 p = Corner(i);
                const float dx = p.x - point.x, dy = p.y - point.y, dz = p.z - point.z;
                farthest = std::max(farthest, std::sqrt(dx * dx + dy * dy + dz * dz));
            }
            return farthest;
        }

        // Does a sphere touch the box? The box's closest point to the center, within the radius?
        bool TouchesSphere(const DirectX::XMFLOAT3& center, float radius) const
        {
            const float dx = std::clamp(center.x, min.x, max.x) - center.x;
            const float dy = std::clamp(center.y, min.y, max.y) - center.y;
            const float dz = std::clamp(center.z, min.z, max.z) - center.z;
            return dx * dx + dy * dy + dz * dz <= radius * radius;
        }

        // The box around the 8 corners after a transform: axis-aligned again, so it can be larger than the box.
        Aabb Transformed(DirectX::FXMMATRIX transform) const
        {
            using namespace DirectX;
            XMVECTOR lo = XMVectorReplicate(FLT_MAX), hi = XMVectorReplicate(-FLT_MAX);
            for (int i = 0; i < 8; ++i)
            {
                const XMFLOAT3 corner = Corner(i);
                const XMVECTOR p = XMVector3TransformCoord(XMLoadFloat3(&corner), transform);
                lo = XMVectorMin(lo, p);
                hi = XMVectorMax(hi, p);
            }
            Aabb result;
            XMStoreFloat3(&result.min, lo);
            XMStoreFloat3(&result.max, hi);
            return result;
        }
    };
}
