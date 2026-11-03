#pragma once
// Mesh, OBJ loader, primitif, BVH, dan baker (normal map + AO) ke ruang UV.
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <numeric>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include "util.h"

struct Vertex { glm::vec3 p, n; glm::vec2 uv; glm::vec3 t; };
struct Mesh {
  std::vector<Vertex> v;
  std::vector<uint32_t> idx;
  bool hasUV = true;
  size_t tris() const { return idx.size() / 3; }
};

inline void computeNormals(Mesh& m) {
  for (auto& v : m.v) v.n = glm::vec3(0);
  for (size_t i = 0; i + 2 < m.idx.size(); i += 3) {
    Vertex &a = m.v[m.idx[i]], &b = m.v[m.idx[i + 1]], &c = m.v[m.idx[i + 2]];
    glm::vec3 fn = glm::cross(b.p - a.p, c.p - a.p);
    a.n += fn; b.n += fn; c.n += fn;
  }
  for (auto& v : m.v) v.n = glm::length(v.n) > 1e-12f ? glm::normalize(v.n) : glm::vec3(0, 1, 0);
}

inline void computeTangents(Mesh& m) {
  std::vector<glm::vec3> acc(m.v.size(), glm::vec3(0));
  for (size_t i = 0; i + 2 < m.idx.size(); i += 3) {
    Vertex &a = m.v[m.idx[i]], &b = m.v[m.idx[i + 1]], &c = m.v[m.idx[i + 2]];
    glm::vec3 e1 = b.p - a.p, e2 = c.p - a.p;
    glm::vec2 d1 = b.uv - a.uv, d2 = c.uv - a.uv;
    float det = d1.x * d2.y - d2.x * d1.y;
    if (std::fabs(det) < 1e-12f) continue;
    glm::vec3 t = (e1 * d2.y - e2 * d1.y) / det;
    acc[m.idx[i]] += t; acc[m.idx[i + 1]] += t; acc[m.idx[i + 2]] += t;
  }
  for (size_t i = 0; i < m.v.size(); i++) {
    glm::vec3 n = m.v[i].n, t = acc[i] - n * glm::dot(n, acc[i]);
    if (glm::dot(t, t) < 1e-12f) {
      glm::vec3 ax = std::fabs(n.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
      t = glm::cross(ax, n);
    }
    m.v[i].t = glm::normalize(t);
  }
}

inline int parseIdx(const std::string& s, size_t n) {
  if (s.empty()) return -1;
  int i = std::atoi(s.c_str());
  if (i == 0) return -1;
  return i > 0 ? i - 1 : (int)n + i;
}

inline bool loadOBJ(const std::string& path, Mesh& m, std::string& err) {
  std::ifstream f(path);
  if (!f) { err = "File tidak ditemukan: " + path; return false; }
  std::vector<glm::vec3> P, N; std::vector<glm::vec2> T;
  std::unordered_map<std::string, uint32_t> cache;
  Mesh r; bool hasN = false, hasT = false;
  std::string line;
  while (std::getline(f, line)) {
    std::istringstream s(line); std::string tag; s >> tag;
    if (tag == "v") { glm::vec3 p; s >> p.x >> p.y >> p.z; P.push_back(p); }
    else if (tag == "vt") { glm::vec2 t; s >> t.x >> t.y; T.push_back(t); }
    else if (tag == "vn") { glm::vec3 n; s >> n.x >> n.y >> n.z; N.push_back(n); }
    else if (tag == "f") {
      std::vector<uint32_t> face; std::string tok;
      while (s >> tok) {
        std::string parts[3]; int k = 0;
        for (char c : tok) { if (c == '/') { if (++k > 2) break; } else parts[k] += c; }
        int vi = parseIdx(parts[0], P.size()), ti = parseIdx(parts[1], T.size()), ni = parseIdx(parts[2], N.size());
        if (vi < 0 || vi >= (int)P.size()) continue;
        if (ti >= (int)T.size()) ti = -1;
        if (ni >= (int)N.size()) ni = -1;
        std::string key = std::to_string(vi) + "/" + std::to_string(ti) + "/" + std::to_string(ni);
        auto it = cache.find(key);
        if (it == cache.end()) {
          Vertex v{}; v.p = P[vi];
          if (ti >= 0) { v.uv = T[ti]; hasT = true; }
          if (ni >= 0) { v.n = N[ni]; hasN = true; }
          it = cache.emplace(key, (uint32_t)r.v.size()).first;
          r.v.push_back(v);
        }
        face.push_back(it->second);
      }
      for (size_t i = 1; i + 1 < face.size(); i++) { r.idx.push_back(face[0]); r.idx.push_back(face[i]); r.idx.push_back(face[i + 1]); }
    }
  }
  if (r.idx.empty()) { err = "OBJ kosong / tidak ada face"; return false; }
  r.hasUV = hasT;
  if (!hasN) computeNormals(r);
  else for (auto& v : r.v) v.n = glm::length(v.n) > 1e-12f ? glm::normalize(v.n) : glm::vec3(0, 1, 0);
  computeTangents(r);
  m = std::move(r);
  return true;
}

// ---------- primitif ----------
inline void addFace(Mesh& m, glm::vec3 n, glm::vec3 u, glm::vec3 v, float half, float off) {
  uint32_t b = (uint32_t)m.v.size();
  const float s[4] = {0, 1, 1, 0}, t[4] = {0, 0, 1, 1};
  for (int i = 0; i < 4; i++) {
    Vertex vt{}; vt.p = n * off + u * ((2 * s[i] - 1) * half) + v * ((2 * t[i] - 1) * half);
    vt.n = n; vt.uv = glm::vec2(s[i], t[i]); vt.t = u; m.v.push_back(vt);
  }
  uint32_t q[6] = {0, 1, 2, 0, 2, 3};
  for (int i = 0; i < 6; i++) m.idx.push_back(b + q[i]);
}
inline Mesh makeCube() {
  Mesh m; using V = glm::vec3;
  addFace(m, V(1, 0, 0), V(0, 0, -1), V(0, 1, 0), 0.5f, 0.5f);
  addFace(m, V(-1, 0, 0), V(0, 0, 1), V(0, 1, 0), 0.5f, 0.5f);
  addFace(m, V(0, 1, 0), V(1, 0, 0), V(0, 0, -1), 0.5f, 0.5f);
  addFace(m, V(0, -1, 0), V(1, 0, 0), V(0, 0, 1), 0.5f, 0.5f);
  addFace(m, V(0, 0, 1), V(1, 0, 0), V(0, 1, 0), 0.5f, 0.5f);
  addFace(m, V(0, 0, -1), V(-1, 0, 0), V(0, 1, 0), 0.5f, 0.5f);
  return m;
}
inline Mesh makePlane() {
  Mesh m; addFace(m, glm::vec3(0, 0, 1), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), 1.0f, 0.f); return m;
}
inline Mesh makeSphere() {
  Mesh m; const int St = 32, Sl = 64; const float PI = 3.14159265f;
  for (int i = 0; i <= St; i++) for (int j = 0; j <= Sl; j++) {
    float lat = -PI / 2 + PI * i / St, lon = 2 * PI * j / Sl;
    Vertex v{}; v.p = glm::vec3(std::cos(lat) * std::cos(lon), std::sin(lat), -std::cos(lat) * std::sin(lon));
    v.n = v.p; v.uv = glm::vec2((float)j / Sl, (float)i / St); m.v.push_back(v);
  }
  for (int i = 0; i < St; i++) for (int j = 0; j < Sl; j++) {
    uint32_t a = i * (Sl + 1) + j, b = (i + 1) * (Sl + 1) + j, d = a + 1, c = b + 1;
    uint32_t q[6] = {a, d, b, b, d, c};
    for (auto x : q) m.idx.push_back(x);
  }
  computeTangents(m);
  return m;
}

// ---------- raycast ----------
struct Hit { float t = 0; int tri = -1; float b1 = 0, b2 = 0; };

inline bool rayTri(const glm::vec3& o, const glm::vec3& d, const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2,
                   float& t, float& bu, float& bv) {
  glm::vec3 e1 = p1 - p0, e2 = p2 - p0, pv = glm::cross(d, e2);
  float det = glm::dot(e1, pv);
  if (std::fabs(det) < 1e-12f) return false;
  float inv = 1.f / det; glm::vec3 tv = o - p0;
  float u = glm::dot(tv, pv) * inv; if (u < 0 || u > 1) return false;
  glm::vec3 qv = glm::cross(tv, e1);
  float v = glm::dot(d, qv) * inv; if (v < 0 || u + v > 1) return false;
  t = glm::dot(e2, qv) * inv; bu = u; bv = v;
  return true;
}

inline bool raycastBrute(const Mesh& m, const glm::vec3& o, const glm::vec3& d, Hit& h) {
  float best = 1e30f; bool found = false;
  for (size_t i = 0; i < m.tris(); i++) {
    float t, u, v;
    if (rayTri(o, d, m.v[m.idx[i * 3]].p, m.v[m.idx[i * 3 + 1]].p, m.v[m.idx[i * 3 + 2]].p, t, u, v) && t > 1e-6f && t < best) {
      best = t; h.t = t; h.tri = (int)i; h.b1 = u; h.b2 = v; found = true;
    }
  }
  return found;
}

struct Bvh {
  struct BNode { glm::vec3 mn, mx; int l = -1, r = -1, start = 0, cnt = 0; };
  std::vector<BNode> nodes; std::vector<int> tris; std::vector<glm::vec3> cent; const Mesh* m = nullptr;

  void build(const Mesh& mesh) {
    m = &mesh; size_t nt = mesh.tris();
    tris.resize(nt); std::iota(tris.begin(), tris.end(), 0);
    cent.resize(nt);
    for (size_t i = 0; i < nt; i++)
      cent[i] = (mesh.v[mesh.idx[i * 3]].p + mesh.v[mesh.idx[i * 3 + 1]].p + mesh.v[mesh.idx[i * 3 + 2]].p) / 3.f;
    nodes.clear(); nodes.reserve(nt + 1);
    buildRec(0, (int)nt);
  }
  int buildRec(int s, int e) {
    int id = (int)nodes.size(); nodes.push_back(BNode());
    glm::vec3 mn(1e30f), mx(-1e30f), cmn(1e30f), cmx(-1e30f);
    for (int i = s; i < e; i++) {
      int t = tris[i];
      for (int k = 0; k < 3; k++) { const glm::vec3& p = m->v[m->idx[t * 3 + k]].p; mn = glm::min(mn, p); mx = glm::max(mx, p); }
      cmn = glm::min(cmn, cent[t]); cmx = glm::max(cmx, cent[t]);
    }
    nodes[id].mn = mn; nodes[id].mx = mx;
    if (e - s <= 4) { nodes[id].start = s; nodes[id].cnt = e - s; return id; }
    glm::vec3 ext = cmx - cmn; int ax = ext.x > ext.y ? (ext.x > ext.z ? 0 : 2) : (ext.y > ext.z ? 1 : 2);
    int mid = (s + e) / 2;
    std::nth_element(tris.begin() + s, tris.begin() + mid, tris.begin() + e, [&](int a, int b) { return cent[a][ax] < cent[b][ax]; });
    int l = buildRec(s, mid), r = buildRec(mid, e);
    nodes[id].l = l; nodes[id].r = r; nodes[id].cnt = 0;
    return id;
  }
  bool intersect(glm::vec3 o, glm::vec3 d, float tmax, Hit& h) const {
    for (int k = 0; k < 3; k++) if (std::fabs(d[k]) < 1e-9f) d[k] = 1e-9f;
    glm::vec3 inv = 1.f / d; int stack[64], sp = 0; stack[sp++] = 0; bool found = false;
    while (sp) {
      const BNode& n = nodes[stack[--sp]];
      float t0 = 0, t1 = tmax; bool ok = true;
      for (int k = 0; k < 3; k++) {
        float a = (n.mn[k] - o[k]) * inv[k], b = (n.mx[k] - o[k]) * inv[k];
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a); t1 = std::min(t1, b);
        if (t0 > t1) { ok = false; break; }
      }
      if (!ok) continue;
      if (n.cnt > 0) {
        for (int i = n.start; i < n.start + n.cnt; i++) {
          int t = tris[i]; float tt, u, v;
          if (rayTri(o, d, m->v[m->idx[t * 3]].p, m->v[m->idx[t * 3 + 1]].p, m->v[m->idx[t * 3 + 2]].p, tt, u, v) && tt > 1e-6f && tt < tmax) {
            tmax = tt; h.t = tt; h.tri = t; h.b1 = u; h.b2 = v; found = true;
          }
        }
      } else if (sp < 62) { stack[sp++] = n.l; stack[sp++] = n.r; }
    }
    return found;
  }
};

inline glm::vec3 smoothNormalAt(const Mesh& m, const Hit& h) {
  const Vertex &a = m.v[m.idx[h.tri * 3]], &b = m.v[m.idx[h.tri * 3 + 1]], &c = m.v[m.idx[h.tri * 3 + 2]];
  glm::vec3 n = a.n * (1 - h.b1 - h.b2) + b.n * h.b1 + c.n * h.b2;
  return glm::length(n) > 1e-8f ? glm::normalize(n) : glm::normalize(glm::cross(b.p - a.p, c.p - a.p));
}

inline float meshDiag(const Mesh& m) {
  glm::vec3 mn(1e30f), mx(-1e30f);
  for (auto& v : m.v) { mn = glm::min(mn, v.p); mx = glm::max(mx, v.p); }
  return glm::length(mx - mn);
}

// Rasterisasi UV low-poly; fn(posisi, normal, tangent, bitangent, rgba_out) dipanggil per texel.
template <class F>
void bakeUV(const Mesh& low, int res, uint8_t fr, uint8_t fg, uint8_t fb, F fn, std::vector<uint8_t>& out) {
  out.assign((size_t)res * res * 4, 255);
  std::vector<uint8_t> mask((size_t)res * res, 0);
  for (size_t i = 0; i < (size_t)res * res; i++) { out[i * 4] = fr; out[i * 4 + 1] = fg; out[i * 4 + 2] = fb; }
  parallelFor((int)low.tris(), [&](int ti) {
    const Vertex &A = low.v[low.idx[ti * 3]], &B = low.v[low.idx[ti * 3 + 1]], &C = low.v[low.idx[ti * 3 + 2]];
    glm::vec2 mn = glm::min(A.uv, glm::min(B.uv, C.uv)), mx = glm::max(A.uv, glm::max(B.uv, C.uv));
    int x0 = std::max(0, (int)std::floor(mn.x * res)), x1 = std::min(res - 1, (int)std::ceil(mx.x * res));
    int y0 = std::max(0, (int)std::floor(mn.y * res)), y1 = std::min(res - 1, (int)std::ceil(mx.y * res));
    float den = (B.uv.y - C.uv.y) * (A.uv.x - C.uv.x) + (C.uv.x - B.uv.x) * (A.uv.y - C.uv.y);
    if (std::fabs(den) < 1e-14f) return;
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
      float u = (x + .5f) / res, v = (y + .5f) / res;
      float w0 = ((B.uv.y - C.uv.y) * (u - C.uv.x) + (C.uv.x - B.uv.x) * (v - C.uv.y)) / den;
      float w1 = ((C.uv.y - A.uv.y) * (u - C.uv.x) + (A.uv.x - C.uv.x) * (v - C.uv.y)) / den;
      float w2 = 1.f - w0 - w1;
      const float e = -1e-4f;
      if (w0 < e || w1 < e || w2 < e) continue;
      glm::vec3 p = A.p * w0 + B.p * w1 + C.p * w2;
      glm::vec3 n = glm::normalize(A.n * w0 + B.n * w1 + C.n * w2);
      glm::vec3 t = A.t * w0 + B.t * w1 + C.t * w2;
      t = t - n * glm::dot(n, t);
      t = glm::length(t) > 1e-8f ? glm::normalize(t) : glm::normalize(glm::cross(glm::vec3(0, 1, 0.01f), n));
      glm::vec3 b = glm::cross(n, t);
      uint8_t px[4] = {fr, fg, fb, 255};
      fn(p, n, t, b, px);
      size_t o = ((size_t)y * res + x) * 4;
      out[o] = px[0]; out[o + 1] = px[1]; out[o + 2] = px[2];
      mask[(size_t)y * res + x] = 1;
    }
  }, 8);
  // padding (dilate) supaya tidak ada garis di seam UV
  for (int it = 0; it < 6; it++) {
    std::vector<uint8_t> o2 = out, m2 = mask;
    for (int y = 0; y < res; y++) for (int x = 0; x < res; x++) {
      if (mask[(size_t)y * res + x]) continue;
      for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
        int nx = x + dx, ny = y + dy;
        if (nx < 0 || ny < 0 || nx >= res || ny >= res || !mask[(size_t)ny * res + nx] || m2[(size_t)y * res + x]) continue;
        size_t s = ((size_t)ny * res + nx) * 4, d = ((size_t)y * res + x) * 4;
        o2[d] = out[s]; o2[d + 1] = out[s + 1]; o2[d + 2] = out[s + 2];
        m2[(size_t)y * res + x] = 1;
      }
    }
    out.swap(o2); mask.swap(m2);
  }
}
