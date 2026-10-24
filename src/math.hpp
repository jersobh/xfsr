#pragma once

#include <cmath>
#include <cstring>

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

struct Mat4 {
    float m[16]{}; // column-major

    static Mat4 identity() {
        Mat4 r{};
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }

    static Mat4 perspective(float fovyRad, float aspect, float znear, float zfar) {
        Mat4 r{};
        float f = 1.0f / std::tan(fovyRad * 0.5f);
        r.m[0] = f / aspect;
        r.m[5] = f;
        r.m[10] = zfar / (znear - zfar);
        r.m[11] = -1.0f;
        r.m[14] = (zfar * znear) / (znear - zfar);
        return r;
    }

    static Mat4 lookAt(Vec3 eye, Vec3 center, Vec3 up) {
        auto sub = [](Vec3 a, Vec3 b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; };
        auto cross = [](Vec3 a, Vec3 b) {
            return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
        };
        auto norm = [](Vec3 v) {
            float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            return Vec3{v.x / l, v.y / l, v.z / l};
        };
        Vec3 f = norm(sub(center, eye));
        Vec3 s = norm(cross(f, up));
        Vec3 u = cross(s, f);
        Mat4 r = identity();
        r.m[0] = s.x;  r.m[4] = s.y;  r.m[8]  = s.z;
        r.m[1] = u.x;  r.m[5] = u.y;  r.m[9]  = u.z;
        r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
        r.m[12] = -(s.x * eye.x + s.y * eye.y + s.z * eye.z);
        r.m[13] = -(u.x * eye.x + u.y * eye.y + u.z * eye.z);
        r.m[14] = f.x * eye.x + f.y * eye.y + f.z * eye.z;
        return r;
    }

    static Mat4 translate(Vec3 t) {
        Mat4 r = identity();
        r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
        return r;
    }

    static Mat4 scale(Vec3 s) {
        Mat4 r = identity();
        r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z;
        return r;
    }

    static Mat4 rotateY(float a) {
        Mat4 r = identity();
        float c = std::cos(a), s = std::sin(a);
        r.m[0] = c;  r.m[8] = s;
        r.m[2] = -s; r.m[10] = c;
        return r;
    }

    static Mat4 rotateX(float a) {
        Mat4 r = identity();
        float c = std::cos(a), s = std::sin(a);
        r.m[5] = c;  r.m[9] = s;
        r.m[6] = -s; r.m[10] = c;
        return r;
    }

    Mat4 operator*(const Mat4& o) const {
        Mat4 r{};
        for (int c = 0; c < 4; ++c) {
            for (int row = 0; row < 4; ++row) {
                r.m[c * 4 + row] =
                    m[0 * 4 + row] * o.m[c * 4 + 0] +
                    m[1 * 4 + row] * o.m[c * 4 + 1] +
                    m[2 * 4 + row] * o.m[c * 4 + 2] +
                    m[3 * 4 + row] * o.m[c * 4 + 3];
            }
        }
        return r;
    }
};
