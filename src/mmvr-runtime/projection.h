#pragma once
#include <openxr/openxr.h>
#include <cmath>
namespace mmvr {
// Row-vector matrices, shared by desktop and Android. Backend depth conversion
// happens after this core; the game's display-list convention is GL [-1,1].
struct Matrix {
    float m[4][4]{};
};
inline Matrix Multiply(const Matrix& a, const Matrix& b) {
    Matrix out;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k)
                out.m[i][j] += a.m[i][k] * b.m[k][j];
    return out;
}
inline Matrix PoseMatrix(const XrPosef& pose) {
    const auto q = pose.orientation;
    Matrix out{ { { 1 - 2 * (q.y * q.y + q.z * q.z), 2 * (q.x * q.y + q.z * q.w), 2 * (q.x * q.z - q.y * q.w), 0 },
                  { 2 * (q.x * q.y - q.z * q.w), 1 - 2 * (q.x * q.x + q.z * q.z), 2 * (q.y * q.z + q.x * q.w), 0 },
                  { 2 * (q.x * q.z + q.y * q.w), 2 * (q.y * q.z - q.x * q.w), 1 - 2 * (q.x * q.x + q.y * q.y), 0 },
                  { pose.position.x, pose.position.y, pose.position.z, 1 } } };
    return out;
}
// Only rigid poses with normalized OpenXR quaternions are passed here.
inline Matrix InversePose(const Matrix& pose) {
    Matrix out;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out.m[i][j] = pose.m[j][i];
    for (int j = 0; j < 3; ++j)
        for (int k = 0; k < 3; ++k)
            out.m[3][j] -= pose.m[3][k] * out.m[k][j];
    out.m[3][3] = 1;
    return out;
}
// Affine inverse also handles the native skeleton's 0.01 model scale.
inline bool InverseAffine(const Matrix& a, Matrix& result) {
    const auto& m = a.m;
    float det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (!std::isfinite(det) || std::abs(det) < 1e-12f)
        return false;
    result = {};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) {
            int i = (col + 1) % 3, j = (col + 2) % 3, k = (row + 1) % 3, l = (row + 2) % 3;
            result.m[row][col] = (m[i][k] * m[j][l] - m[i][l] * m[j][k]) / det;
        }
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 3; ++row)
            result.m[3][col] -= m[3][row] * result.m[row][col];
    result.m[3][3] = 1;
    return true;
}
inline Matrix CenterSkybox(Matrix model, const Matrix& worldView, const XrPosef& eye, const XrPosef& origin,
                           float unitsPerMetre = 40.f) {
    auto viewPose = InversePose(worldView);
    auto relative = Multiply(PoseMatrix(eye), InversePose(PoseMatrix(origin)));
    for (int j = 0; j < 3; ++j) {
        model.m[3][j] = viewPose.m[3][j];
        for (int i = 0; i < 3; ++i)
            model.m[3][j] += relative.m[3][i] * unitsPerMetre * viewPose.m[i][j];
    }
    return model;
}
inline float NativeFogDepth(float a, float b, float scale, float eyeW) {
    return -a / scale + b / (scale * eyeW);
}
// Snapshot once per vertex batch. Preserve the native operation order and do
// not reassociate b/(scale*w) into (b/scale)/w.
struct FogProjection {
    float constant = 0, b = 0, scale = 1;
    bool active = false;
    float Depth(float z, float w) const {
        return active ? constant + b / (scale * w) : z / w;
    }
};
inline FogProjection MakeFogProjection(float a, float b, float scale, bool active) {
    return { active ? -a / scale : 0.f, b, scale, active };
}
inline Matrix EyeProjectionMatrix(const XrPosef& eye, const XrFovf& fov, const XrPosef& origin, float nearPlane = 1.f,
                                  float farPlane = 30000.f, float unitsPerMetre = 40.f) {
    auto relative = Multiply(PoseMatrix(eye), InversePose(PoseMatrix(origin)));
    for (int i = 0; i < 3; ++i)
        relative.m[3][i] *= unitsPerMetre;
    const float l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight), b = std::tan(fov.angleDown),
                t = std::tan(fov.angleUp);
    Matrix projection{ { { 2 / (r - l), 0, 0, 0 },
                         { 0, 2 / (t - b), 0, 0 },
                         { (r + l) / (r - l), (t + b) / (t - b), -(farPlane + nearPlane) / (farPlane - nearPlane), -1 },
                         { 0, 0, -2 * farPlane * nearPlane / (farPlane - nearPlane), 0 } } };
    return Multiply(InversePose(relative), projection);
}
} // namespace mmvr
