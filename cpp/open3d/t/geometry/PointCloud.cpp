// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2018 www.open3d.org
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
// ----------------------------------------------------------------------------

#include "open3d/t/geometry/PointCloud.h"

#include <Eigen/Core>
#include <string>
#include <unordered_map>

#include "open3d/core/EigenConverter.h"
#include "open3d/core/ShapeUtil.h"
#include "open3d/core/Tensor.h"
#include "open3d/core/hashmap/Hashmap.h"
#include "open3d/core/kernel/Kernel.h"
#include "open3d/core/linalg/Matmul.h"
#include "open3d/t/geometry/TensorMap.h"

namespace open3d {
namespace t {
namespace geometry {

PointCloud::PointCloud(const core::Device &device)
    : Geometry(Geometry::GeometryType::PointCloud, 3),
      device_(device),
      point_attr_(TensorMap("points")) {
    ;
}

PointCloud::PointCloud(const core::Tensor &points)
    : PointCloud(points.GetDevice()) {
    points.AssertShapeCompatible({utility::nullopt, 3});
    SetPoints(points);
}

PointCloud::PointCloud(const std::unordered_map<std::string, core::Tensor>
                               &map_keys_to_tensors)
    : PointCloud(map_keys_to_tensors.at("points").GetDevice()) {
    if (map_keys_to_tensors.count("points") == 0) {
        utility::LogError("\"points\" attribute must be specified.");
    }
    map_keys_to_tensors.at("points").AssertShapeCompatible(
            {utility::nullopt, 3});
    point_attr_ = TensorMap("points", map_keys_to_tensors.begin(),
                            map_keys_to_tensors.end());
}

core::Tensor PointCloud::GetMinBound() const { return GetPoints().Min({0}); }

core::Tensor PointCloud::GetMaxBound() const { return GetPoints().Max({0}); }

core::Tensor PointCloud::GetCenter() const { return GetPoints().Mean({0}); }

PointCloud &PointCloud::Transform(const core::Tensor &transformation) {
    transformation.AssertShape({4, 4});
    transformation.AssertDevice(device_);

    core::Tensor R = transformation.Slice(0, 0, 3).Slice(1, 0, 3);
    core::Tensor t = transformation.Slice(0, 0, 3).Slice(1, 3, 4);
    // TODO: Make it more generalised [4x4][4xN] Transformation

    // TODO: consider adding a new op extending MatMul to support `AB + C`
    // GEMM operation. Also, a parallel joint optimimsed kernel for
    // independent MatMul operation with common matrix like AB and AC
    // with fusion based cache optimisation
    core::Tensor &points = GetPoints();
    points = (R.Matmul(points.T())).Add_(t).T();

    if (HasPointNormals()) {
        core::Tensor &normals = GetPointNormals();
        normals = (R.Matmul(normals.T())).T();
    }
    return *this;
}

PointCloud &PointCloud::Translate(const core::Tensor &translation,
                                  bool relative) {
    translation.AssertShape({3});
    translation.AssertDevice(device_);

    core::Tensor transform = translation;
    if (!relative) {
        transform -= GetCenter();
    }
    GetPoints() += transform;
    return *this;
}

PointCloud &PointCloud::Scale(double scale, const core::Tensor &center) {
    center.AssertShape({3});
    center.AssertDevice(device_);

    core::Tensor points = GetPoints();
    points.Sub_(center).Mul_(scale).Add_(center);
    return *this;
}

PointCloud &PointCloud::Rotate(const core::Tensor &R,
                               const core::Tensor &center) {
    R.AssertShape({3, 3});
    R.AssertDevice(device_);
    center.AssertShape({3});
    center.AssertDevice(device_);

    core::Tensor Rot = R;
    core::Tensor &points = GetPoints();
    points = ((Rot.Matmul((points.Sub_(center)).T())).T()).Add_(center);

    if (HasPointNormals()) {
        core::Tensor &normals = GetPointNormals();
        normals = (Rot.Matmul(normals.T())).T();
    }
    return *this;
}

PointCloud PointCloud::VoxelDownSample(double voxel_size) const {
    core::Tensor points_voxeld = GetPoints() / voxel_size;
    core::Tensor points_voxeli = points_voxeld.To(core::Dtype::Int64);

    core::Hashmap points_voxeli_hashmap(points_voxeli.GetShape()[0],
                                        core::Dtype::Int64, core::Dtype::Int32,
                                        {3}, {1}, device_);

    core::Tensor addrs, masks;
    points_voxeli_hashmap.Activate(points_voxeli, addrs, masks);

    std::unordered_map<std::string, core::Tensor> pcd_down_map;
    core::Tensor points = points_voxeli.IndexGet({masks}).To(
                                  point_attr_.at("points").GetDtype()) *
                          voxel_size;
    pcd_down_map.emplace(std::make_pair("points", points));

    for (auto &kv : point_attr_) {
        if (kv.first != "points") {
            core::Tensor point_attr = kv.second.IndexGet({masks});
            pcd_down_map.emplace(std::make_pair(kv.first, point_attr));
        }
    }

    return PointCloud(pcd_down_map);
}

/// Create a PointCloud from a depth image
PointCloud PointCloud::CreateFromDepthImage(const Image &depth,
                                            const core::Tensor &intrinsics,
                                            double depth_scale,
                                            double depth_max,
                                            int stride) {
    core::Device device = depth.GetDevice();
    std::unordered_map<std::string, core::Tensor> srcs = {
            {"depth", depth.AsTensor()},
            {"intrinsics", intrinsics.Copy(device)},
            {"depth_scale",
             core::Tensor(std::vector<float>{static_cast<float>(depth_scale)},
                          {}, core::Dtype::Float32, device)},
            {"depth_max",
             core::Tensor(std::vector<float>{static_cast<float>(depth_max)}, {},
                          core::Dtype::Float32, device)},
            {"stride", core::Tensor(std::vector<int64_t>{stride}, {},
                                    core::Dtype::Int64, device)}};
    std::unordered_map<std::string, core::Tensor> dsts;

    core::kernel::GeneralEW(srcs, dsts,
                            core::kernel::GeneralEWOpCode::Unproject);
    if (dsts.count("points") == 0) {
        utility::LogError(
                "[PointCloud] unprojection launch failed, vertex map expected "
                "to return.");
    }
    return PointCloud(dsts.at("points"));
}

PointCloud PointCloud::FromLegacyPointCloud(
        const open3d::geometry::PointCloud &pcd_legacy,
        core::Dtype dtype,
        const core::Device &device) {
    geometry::PointCloud pcd(device);
    if (pcd_legacy.HasPoints()) {
        pcd.SetPoints(core::eigen_converter::EigenVector3dVectorToTensor(
                pcd_legacy.points_, dtype, device));
    } else {
        utility::LogWarning("Creating from an empty legacy PointCloud.");
    }
    if (pcd_legacy.HasColors()) {
        pcd.SetPointColors(core::eigen_converter::EigenVector3dVectorToTensor(
                pcd_legacy.colors_, dtype, device));
    }
    if (pcd_legacy.HasNormals()) {
        pcd.SetPointNormals(core::eigen_converter::EigenVector3dVectorToTensor(
                pcd_legacy.normals_, dtype, device));
    }
    return pcd;
}

open3d::geometry::PointCloud PointCloud::ToLegacyPointCloud() const {
    open3d::geometry::PointCloud pcd_legacy;
    if (HasPoints()) {
        pcd_legacy.points_ =
                core::eigen_converter::TensorToEigenVector3dVector(GetPoints());
    }
    if (HasPointColors()) {
        pcd_legacy.colors_ = core::eigen_converter::TensorToEigenVector3dVector(
                GetPointColors());
    }
    if (HasPointNormals()) {
        pcd_legacy.normals_ =
                core::eigen_converter::TensorToEigenVector3dVector(
                        GetPointNormals());
    }
    return pcd_legacy;
}

}  // namespace geometry
}  // namespace t
}  // namespace open3d