#pragma once

#include <algorithm>
#include <cstdint>
#include <glm/glm.hpp>
#include <limits>
#include <tuple>

#include "tinyrend/core/macros.h" // for GSPLAT_HOST_DEVICE
#include "tinyrend/core/math.h"
#include "tinyrend/core/solver.h"

namespace tinyrend::camera::fisheye {

/// \brief Compute the radial distortion: theta -> theta_d
/// \param theta Angle in radians
/// \param radial_coeffs Radial distortion coefficients (k1, k2, k3, k4)
/// \return Distorted angle theta_d
GSPLAT_HOST_DEVICE inline auto
distortion(float const &theta, std::array<float, 4> const &radial_coeffs) -> float {
    auto const theta2 = theta * theta;
    auto const &[k1, k2, k3, k4] = radial_coeffs;
    return theta * tinyrend::math::eval_poly_horner<5>({1.f, k1, k2, k3, k4}, theta2);
}

/// \brief Compute the Jacobian of the distortion: J = d(theta_d) / d(theta)
/// \param theta Angle in radians
/// \param radial_coeffs Radial distortion coefficients (k1, k2, k3, k4)
/// \return Jacobian of the distortion function
GSPLAT_HOST_DEVICE inline auto
distortion_jac(float const &theta, std::array<float, 4> const &radial_coeffs) -> float {
    auto const theta2 = theta * theta;
    auto const &[k1, k2, k3, k4] = radial_coeffs;
    return tinyrend::math::eval_poly_horner<5>(
        {1.f, 3.f * k1, 5.f * k2, 7.f * k3, 9.f * k4}, theta2
    );
}

/// \brief Compute the inverse radial distortion: theta_d -> theta
/// \tparam N_ITER Number of iterations for Newton's method
/// \param theta_d Distorted angle in radians
/// \param radial_coeffs Radial distortion coefficients (k1, k2, k3, k4)
/// \param max_theta Maximum valid theta angle
/// \return Pair of undistorted angle and convergence flag
template <size_t N_ITER = 20>
GSPLAT_HOST_DEVICE inline auto undistortion(
    float const &theta_d,
    std::array<float, 4> const &radial_coeffs,
    float const &max_theta = std::numeric_limits<float>::max()
) -> std::pair<float, bool> {
    // define the residual and Jacobian of the equation
    auto const func = [&theta_d, &radial_coeffs, &max_theta](const float &theta
                      ) -> std::pair<float, float> {
        auto const valid_flag = theta <= max_theta;
        if (!valid_flag)
            return {float{}, float{}};
        auto const J = distortion_jac(theta, radial_coeffs);
        auto const residual = distortion(theta, radial_coeffs) - theta_d;
        return {residual, J};
    };
    // solve the equation
    auto const &[theta, converged] =
        tinyrend::solver::newton<1, N_ITER>(func, theta_d, 1e-6f);
    return {theta, converged};
}

/// \brief Compute the maximum theta such that [0, max_theta] is monotonicly increasing
/// \tparam N_ITER Number of iterations for root finding
/// \param radial_coeffs Radial distortion coefficients (k1, k2, k3, k4)
/// \param guess Initial guess for the root
/// \return Maximum theta angle for monotonic distortion
template <size_t N_ITER = 20>
GSPLAT_HOST_DEVICE inline auto monotonic_max_theta(
    std::array<float, 4> const &radial_coeffs, float guess = 1.57f
) -> float {
    // The distortion function is
    //   f(theta) = theta * (1 + k1*theta^2 + k2*theta^4 + k3*theta^6 +
    //   k4*theta^8)
    // The derivative is
    //   f'(theta) = 1 + 3*k1*theta^2 + 5*k2*theta^4 + 7*k3*theta^6 +
    //   9*k4*theta^8
    // The maximum theta such that [0, max_theta] is monotonicly increasing is
    // the minimal positive root of f'(theta) = 0.

    // Setting x = theta^2, so we just need to solve this:
    //   0 = 1 + 3*k1*x + 5*k2*x^2 + 7*k3*x^3 + 9*k4*x^4
    auto const &[k1, k2, k3, k4] = radial_coeffs;
    constexpr float INF = std::numeric_limits<float>::max();
    auto const x2 = tinyrend::solver::poly_minimal_positive<N_ITER>(
        std::array<float, 5>{1.f, 3.f * k1, 5.f * k2, 7.f * k3, 9.f * k4},
        0.f,
        guess,
        INF
    );
    return x2 == INF ? INF : std::sqrt(x2);
}

/// \brief Project a 3D point in camera space to 2D image space using fisheye projection
/// \param camera_point 3D point in camera space (x, y, z)
/// \param focal_length Focal length in pixels (fx, fy)
/// \param principal_point Principal point in pixels (cx, cy)
/// \param min_2d_norm Minimum 2D norm threshold for numerical stability
/// \return Projected 2D point in image space
GSPLAT_HOST_DEVICE inline auto project(
    glm::fvec3 const &camera_point,
    glm::fvec2 const &focal_length,
    glm::fvec2 const &principal_point,
    float const &min_2d_norm = 1e-6f
) -> glm::fvec2 {
    auto const xy = glm::fvec2(camera_point) / camera_point.z;
    auto const r = tinyrend::math::numerically_stable_norm2(xy[0], xy[1]);
    glm::fvec2 uv;
    if (r < min_2d_norm) {
        // For points at the image center, there is no distortion
        uv = xy;
    } else {
        auto const theta = std::atan(r);
        uv = theta / r * xy;
    }
    auto const image_point = focal_length * uv + principal_point;
    return image_point;
}

/// \brief Project a 3D point in camera space to 2D image space using fisheye projection
/// with radial distortion
/// \param camera_point 3D point in camera space (x, y, z)
/// \param focal_length Focal length in pixels (fx, fy)
/// \param principal_point Principal point in pixels (cx, cy)
/// \param radial_coeffs Radial distortion coefficients (k1, k2, k3, k4)
/// \param min_2d_norm Minimum 2D norm threshold for numerical stability
/// \param max_theta Maximum theta angle for valid projection
/// \return Pair of projected 2D point and validity flag
GSPLAT_HOST_DEVICE inline auto project(
    glm::fvec3 const &camera_point,
    glm::fvec2 const &focal_length,
    glm::fvec2 const &principal_point,
    std::array<float, 4> const &radial_coeffs,
    float const &min_2d_norm = 1e-6f,
    float const &max_theta = std::numeric_limits<float>::max()
) -> std::pair<glm::fvec2, bool> {
    auto const xy = glm::fvec2(camera_point) / camera_point.z;
    auto const r = tinyrend::math::numerically_stable_norm2(xy[0], xy[1]);
    glm::fvec2 uv;
    if (r < min_2d_norm) {
        // For points at the image center, there is no distortion
        uv = xy;
    } else {
        auto const theta = std::atan(r);
        if (theta > max_theta) {
            // Theta is too large, might be in the invalid region
            return {glm::fvec2{}, false};
        }
        auto const theta_d = distortion(theta, radial_coeffs);
        uv = theta_d / r * xy;
    }
    auto const image_point = focal_length * uv + principal_point;
    return {image_point, true};
}

/// \brief Compute the Jacobian of the projection: J = d(image_point) / d(camera_point)
/// \param camera_point 3D point in camera space (x, y, z)
/// \param focal_length Focal length in pixels (fx, fy)
/// \param min_2d_norm Minimum 2D norm threshold for numerical stability
/// \return 3x2 Jacobian matrix
GSPLAT_HOST_DEVICE inline auto project_jac(
    glm::fvec3 const &camera_point,
    glm::fvec2 const &focal_length,
    float const &min_2d_norm = 1e-6f
) -> glm::fmat3x2 {
    // forward:
    auto const invz = 1.0f / camera_point.z;
    auto const xy = glm::fvec2(camera_point) * invz;
    auto const r = tinyrend::math::numerically_stable_norm2(xy[0], xy[1]);

    glm::fmat2 J_uv_xy;
    if (r < min_2d_norm) {
        // For points at the image center, J_uv_xy = I
        J_uv_xy = glm::fmat2(1.0f);
    } else {
        auto const invr = 1.0f / r;
        auto const theta = std::atan(r);
        auto const s = theta * invr;
        // auto const uv = s * xy;
        // auto const image_point = focal_length * uv + principal_point;

        // backward:
        // Note: this can be slightly optimized by fusing the matrix multiplications.
        auto const J_theta_r = 1.0f / (1.0f + r * r);
        auto const J_s_xy = (J_theta_r - s) * invr * invr * xy;
        J_uv_xy = s * glm::fmat2(1.0f) + glm::outerProduct(J_s_xy, xy);
    }

    auto const J_im_xy = glm::fmat2x2(
        focal_length[0] * J_uv_xy[0][0],
        focal_length[1] * J_uv_xy[0][1],
        focal_length[0] * J_uv_xy[1][0],
        focal_length[1] * J_uv_xy[1][1]
    );
    auto const J_xy_cam =
        glm::fmat3x2(invz, 0.0f, 0.0f, invz, -xy[0] * invz, -xy[1] * invz);
    auto const J = J_im_xy * J_xy_cam;
    return J;
}

// This version is slower than the one below.
GSPLAT_HOST_DEVICE inline auto _project_hess(
    glm::fvec3 const &camera_point,
    glm::fvec2 const &focal_length,
    float const &min_2d_norm = 1e-6f
) -> std::array<glm::fmat3x3, 2> {
    // forward:
    auto const invz = 1.0f / camera_point.z;
    auto const invz2 = invz * invz;
    auto const xy = glm::fvec2(camera_point) * invz;
    auto const r = tinyrend::math::numerically_stable_norm2(xy[0], xy[1]);

    glm::fmat2 J_uv_xy;
    glm::fmat2 d_J_uv_xy_d_x, d_J_uv_xy_d_y;
    if (r < min_2d_norm) {
        // For points at the image center, J_uv_xy = I
        d_J_uv_xy_d_x = glm::fmat2(0.0f);
        d_J_uv_xy_d_y = glm::fmat2(0.0f);
    } else {
        auto const invr = 1.0f / r;
        auto const invr2 = invr * invr;
        auto const theta = std::atan(r);
        auto const s = theta * invr;
        // auto const uv = s * xy;
        // auto const image_point = focal_length * uv + principal_point;

        // backward:
        // Note: this can be slightly optimized by fusing the matrix multiplications.
        auto const J_theta_r = 1.0f / (1.0f + r * r);
        auto const tmp = (J_theta_r - s) * invr * invr;
        auto const xy_outer = glm::outerProduct(xy, xy);
        J_uv_xy = s * glm::fmat2(1.0f) + tmp * xy_outer;

        auto const d_r_d_xy = xy * invr;
        auto const d_s_d_r = J_theta_r * invr - theta * invr2;
        auto const d_tmp_d_r =
            invr2 * (-2.0f * J_theta_r * J_theta_r * r - 3.0f * d_s_d_r);

        auto const d_s_d_xy = d_s_d_r * d_r_d_xy;
        auto const d_tmp_d_xy = d_tmp_d_r * d_r_d_xy;

        d_J_uv_xy_d_x = d_s_d_xy[0] * glm::fmat2(1.0f) + d_tmp_d_xy[0] * xy_outer +
                        tmp * glm::fmat2(2.0f * xy[0], xy[1], xy[1], 0.0f);

        d_J_uv_xy_d_y = d_s_d_xy[1] * glm::fmat2(1.0f) + d_tmp_d_xy[1] * xy_outer +
                        tmp * glm::fmat2(0.0f, xy[0], xy[0], 2.0f * xy[1]);
    }

    auto const J_im_xy = glm::fmat2x2(
        focal_length[0] * J_uv_xy[0][0],
        focal_length[1] * J_uv_xy[0][1],
        focal_length[0] * J_uv_xy[1][0],
        focal_length[1] * J_uv_xy[1][1]
    );
    // auto const J_xy_cam =
    //     glm::fmat3x2(invz, 0.0f, 0.0f, invz, -xy[0] * invz, -xy[1] * invz);
    // auto const J = J_im_xy * J_xy_cam;

    auto const d_J_im_xy_d_x = glm::fmat2x2(
        focal_length[0] * d_J_uv_xy_d_x[0][0],
        focal_length[1] * d_J_uv_xy_d_x[0][1],
        focal_length[0] * d_J_uv_xy_d_x[1][0],
        focal_length[1] * d_J_uv_xy_d_x[1][1]
    );

    auto const d_J_im_xy_d_y = glm::fmat2x2(
        focal_length[0] * d_J_uv_xy_d_y[0][0],
        focal_length[1] * d_J_uv_xy_d_y[0][1],
        focal_length[0] * d_J_uv_xy_d_y[1][0],
        focal_length[1] * d_J_uv_xy_d_y[1][1]
    );

    auto const d_J_d_cam_x =
        invz2 *
        glm::fmat3x2(
            d_J_im_xy_d_x[0][0],
            d_J_im_xy_d_x[0][1],
            d_J_im_xy_d_x[1][0],
            d_J_im_xy_d_x[1][1],
            -d_J_im_xy_d_x[0][0] * xy[0] - d_J_im_xy_d_x[1][0] * xy[1] - J_im_xy[0][0],
            -d_J_im_xy_d_x[0][1] * xy[0] - d_J_im_xy_d_x[1][1] * xy[1] - J_im_xy[0][1]
        );
    auto const d_J_d_cam_y =
        invz2 *
        glm::fmat3x2(
            d_J_im_xy_d_y[0][0],
            d_J_im_xy_d_y[0][1],
            d_J_im_xy_d_y[1][0],
            d_J_im_xy_d_y[1][1],
            -d_J_im_xy_d_y[0][0] * xy[0] - d_J_im_xy_d_y[1][0] * xy[1] - J_im_xy[1][0],
            -d_J_im_xy_d_y[0][1] * xy[0] - d_J_im_xy_d_y[1][1] * xy[1] - J_im_xy[1][1]
        );

    auto const d_J_xy_cam_d_z_direct =
        glm::fmat3x2(-invz2, 0.0f, 0.0f, -invz2, xy[0] * invz2, xy[1] * invz2);
    auto const d_J_d_cam_z =
        -d_J_d_cam_x * xy[0] - d_J_d_cam_y * xy[1] + J_im_xy * d_J_xy_cam_d_z_direct;

    auto const H1 = glm::fmat3x3(
        d_J_d_cam_x[0][0],
        d_J_d_cam_x[1][0],
        d_J_d_cam_x[2][0],
        d_J_d_cam_y[0][0],
        d_J_d_cam_y[1][0],
        d_J_d_cam_y[2][0],
        d_J_d_cam_z[0][0],
        d_J_d_cam_z[1][0],
        d_J_d_cam_z[2][0]
    );

    auto const H2 = glm::fmat3x3(
        d_J_d_cam_x[0][1],
        d_J_d_cam_x[1][1],
        d_J_d_cam_x[2][1],
        d_J_d_cam_y[0][1],
        d_J_d_cam_y[1][1],
        d_J_d_cam_y[2][1],
        d_J_d_cam_z[0][1],
        d_J_d_cam_z[1][1],
        d_J_d_cam_z[2][1]
    );
    return {H1, H2};
}

/// \brief Compute the Hessian of the projection: H = d²(image_point) / d(camera_point)²
/// \param camera_point 3D point in camera space (x, y, z)
/// \param focal_length Focal length in pixels (fx, fy)
/// \param min_2d_norm Minimum 2D norm threshold for numerical stability
/// \return Array of two 3x3 Hessian matrices (H1 = ∂²u/∂p², H2 = ∂²v/∂p²)
GSPLAT_HOST_DEVICE inline auto project_hess(
    glm::fvec3 const &camera_point,
    glm::fvec2 const &focal_length,
    float const min_2d_norm = 1e-6f
) -> std::array<glm::fmat3x3, 2> {
    // ‑‑‑ stage‑0 : helpers
    const float invz = 1.f / camera_point.z;
    const float x_ = camera_point.x * invz;
    const float y_ = camera_point.y * invz;
    const float r2 = x_ * x_ + y_ * y_;
    const float r = tinyrend::math::numerically_stable_norm2(x_, y_);
    const float invr = (r > 0.f) ? 1.f / r : 0.f;

    // ‑‑‑ stage‑1 : s(r) and radial derivatives
    float s = 1.f, s1 = 0.f, s2 = 0.f;
    if (r > min_2d_norm) {
        const float theta = std::atan(r);
        const float Jtr = 1.f / (1.f + r2); // dθ/dr
        s = theta * invr;
        s1 = (Jtr - s) * invr; // ds/dr
        const float dJtr = -2.f * r / ((1.f + r2) * (1.f + r2));
        /*  exact  ∂²s/∂r²  (no extra ×2 term!) */
        s2 = (dJtr - s1 - (Jtr - s) * invr) * invr;
    }

    // ∂s/∂xy  and  ∂²s/∂xy²
    glm::fvec2 Js =
        (r > min_2d_norm) ? s1 * invr * glm::fvec2(x_, y_) : glm::fvec2(0.f);
    float Hs[2][2]{{0.f, 0.f}, {0.f, 0.f}};
    if (r > min_2d_norm) {
        const float invr2 = invr * invr;
        const float c1 = s2 * invr2;
        const float c2 = s1 * invr;
        Hs[0][0] = c1 * x_ * x_ + c2 * (1.f - x_ * x_ * invr2);
        Hs[0][1] = c1 * x_ * y_ - c2 * x_ * y_ * invr2;
        Hs[1][0] = Hs[0][1];
        Hs[1][1] = c1 * y_ * y_ + c2 * (1.f - y_ * y_ * invr2);
    }

    // J_xy (2×3)  and  H_xy (2×3×3)
    const float invz2 = invz * invz, invz3 = invz2 * invz;
    float Jxy[2][3] = {{invz, 0.f, -x_ * invz}, {0.f, invz, -y_ * invz}};
    float Hxy[2][3][3]{}; // zero‑init
    /*  x'/z  */
    Hxy[0][0][2] = Hxy[0][2][0] = -invz2;
    Hxy[0][2][2] = 2.f * camera_point.x * invz3;
    /*  y'/z  */
    Hxy[1][1][2] = Hxy[1][2][1] = -invz2;
    Hxy[1][2][2] = 2.f * camera_point.y * invz3;

    // H_uv in xy‑space
    float Huv[2][2][2]{};
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
            for (int k = 0; k < 2; ++k)
                Huv[i][j][k] = Js[k] * (i == j) + Js[j] * (i == k) +
                               ((i == 0) ? x_ : y_) * Hs[j][k];

    // J_uv in xy‑space
    float Juv[2][2] = {{s + x_ * Js.x, x_ * Js.y}, {y_ * Js.x, s + y_ * Js.y}};

    // ‑‑‑ stage‑2 : assemble Hessians  (two 3×3 blocks)
    std::array<glm::fmat3x3, 2> H{glm::fmat3x3(0.f), glm::fmat3x3(0.f)};

    for (int i = 0; i < 2; ++i) { // 0 = u , 1 = v
        float Htmp[3][3]{};       // row,col in p‑space

        // (a)  H_uv  ×  (J_xy ⊗ J_xy)
        for (int j = 0; j < 2; ++j)
            for (int k = 0; k < 2; ++k)
                for (int a = 0; a < 3; ++a)
                    for (int b = 0; b < 3; ++b)
                        Htmp[a][b] += Huv[i][j][k] * Jxy[j][a] * Jxy[k][b];

        // (b)  J_uv_j * H_xy_j
        for (int j = 0; j < 2; ++j)
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b)
                    Htmp[a][b] += Juv[i][j] * Hxy[j][a][b];

        /* write into column‑major glm::mat */
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                H[i][col][row] = focal_length[i] * Htmp[row][col];
    }
    return H; // H[0] = ∂²u/∂p² ,  H[1] = ∂²v/∂p²
}

/// \brief Unproject a 2D point from image space to camera space (undistorted fisheye)
/// \param image_point 2D point in image space
/// \param focal_length Focal length in pixels (fx, fy)
/// \param principal_point Principal point in pixels (cx, cy)
/// \param min_2d_norm Minimum 2D norm threshold for numerical stability
/// \return Normalized ray direction in camera space
GSPLAT_HOST_DEVICE inline auto unproject(
    glm::fvec2 const &image_point,
    glm::fvec2 const &focal_length,
    glm::fvec2 const &principal_point,
    float const &min_2d_norm = 1e-6f
) -> glm::fvec3 {
    auto const uv = (image_point - principal_point) / focal_length;
    auto const theta = sqrtf(glm::dot(uv, uv));

    if (theta < min_2d_norm) {
        // For points at the image center, the ray direction is
        // simply pointing forward.
        return glm::fvec3{0.f, 0.f, 1.f};
    }

    auto const xy = std::sin(theta) / theta * uv;
    auto const dir = glm::fvec3{xy[0], xy[1], std::cos(theta)};
    return dir;
}

/// \brief Unproject a 2D point from image space to camera space (distorted fisheye)
/// \param image_point 2D point in image space
/// \param focal_length Focal length in pixels (fx, fy)
/// \param principal_point Principal point in pixels (cx, cy)
/// \param radial_coeffs Radial distortion coefficients (k1, k2, k3, k4)
/// \param min_2d_norm Minimum 2D norm threshold for numerical stability
/// \param max_theta Maximum theta angle for valid unprojection
/// \return Pair of normalized ray direction and validity flag
GSPLAT_HOST_DEVICE inline auto unproject(
    glm::fvec2 const &image_point,
    glm::fvec2 const &focal_length,
    glm::fvec2 const &principal_point,
    std::array<float, 4> const &radial_coeffs,
    float const &min_2d_norm = 1e-6f,
    float const &max_theta = std::numeric_limits<float>::max()
) -> std::pair<glm::fvec3, bool> {
    auto const uv = (image_point - principal_point) / focal_length;
    auto const theta_d = sqrtf(glm::dot(uv, uv));

    if (theta_d < min_2d_norm) {
        // For points at the image center, the ray direction is
        // simply pointing forward.
        return {glm::fvec3{0.f, 0.f, 1.f}, true};
    }

    auto const &[theta, valid_flag] = undistortion(theta_d, radial_coeffs, max_theta);
    if (!valid_flag) {
        return {glm::fvec3{}, false};
    }

    auto const xy = std::sin(theta) / theta_d * uv;
    auto const dir = glm::fvec3{xy[0], xy[1], std::cos(theta)};
    return {dir, true};
}

} // namespace tinyrend::camera::fisheye
