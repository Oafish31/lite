#pragma once
// Sistem node tekstur procedural (gaya Blender), dievaluasi di CPU, tileable.
#include <glm/glm.hpp>
#include <vector>
#include <array>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include "util.h"

enum NodeType { N_NOISE, N_VORONOI, N_CHECKER, N_COLOR, N_RAMP, N_MIX, N_INVERT, N_OUTPUT, N_COUNT };

inline const char* nodeName(int t) {
  static const char* n[] = {"Noise", "Voronoi", "Checker", "Color", "Color Ramp", "Mix", "Invert", "Material Output"};
  return n[t];
}
inline int nodeInputs(int t) {
  switch (t) { case N_RAMP: return 1; case N_MIX: return 3; case N_INVERT: return 1; case N_OUTPUT: return 3; default: return 0; }
}
inline const char* inputName(int t, int i) {
  static const char* mix[] = {"A", "B", "Fac"};
  static const char* out[] = {"Albedo", "Height", "Roughness"};
  if (t == N_RAMP) return "Fac";
  if (t == N_MIX) return mix[i];
  if (t == N_INVERT) return "Color";
  return out[i];
}

struct Node {
  int id = 0, type = 0;
  int i0 = 8, i1 = 4, i2 = 0;
  float f0 = 0.5f, f1 = 0.5f;
  float c0[3] = {0, 0, 0}, c1[3] = {1, 1, 1};
  float pos[2] = {0, 0};
  bool screenSpace = false, placed = false;
};
struct Link { int id, from, to; };

inline int pinIn(int nid, int i) { return nid * 10 + i; }
inline int pinOut(int nid) { return nid * 10 + 9; }

struct Graph {
  std::vector<Node> nodes;
  std::vector<Link> links;
  int nextNode = 1, nextLink = 1;

  int indexOf(int id) const {
    for (size_t i = 0; i < nodes.size(); i++) if (nodes[i].id == id) return (int)i;
    return -1;
  }
  Node& add(int type, float x, float y, bool screen = false) {
    Node n; n.id = nextNode++; n.type = type; n.pos[0] = x; n.pos[1] = y; n.screenSpace = screen;
    switch (type) {
      case N_NOISE: n.i0 = 8; n.i1 = 4; n.f0 = 0.5f; break;
      case N_VORONOI: n.i0 = 6; break;
      case N_CHECKER: n.i0 = 8; n.c0[0] = n.c0[1] = n.c0[2] = 0.2f; n.c1[0] = n.c1[1] = n.c1[2] = 0.8f; break;
      case N_COLOR: n.c0[0] = n.c0[1] = n.c0[2] = 0.5f; break;
      case N_RAMP: n.f0 = 0.3f; n.f1 = 0.7f; n.c0[0] = 0.12f; n.c0[1] = 0.08f; n.c0[2] = 0.05f;
                   n.c1[0] = 0.8f; n.c1[1] = 0.6f; n.c1[2] = 0.4f; break;
      case N_MIX: n.f0 = 0.5f; n.c0[0] = n.c0[1] = n.c0[2] = 0.5f; break;
      case N_INVERT: n.c0[0] = n.c0[1] = n.c0[2] = 0.5f; break;
      case N_OUTPUT: n.c0[0] = n.c0[1] = n.c0[2] = 0.7f; n.f0 = 0.5f; n.f1 = 0.6f; break;
    }
    nodes.push_back(n);
    return nodes.back();
  }
  bool dependsOn(int a, int b, int depth = 0) const {
    if (a == b || depth > 64) return true;
    for (auto& l : links) if (l.to / 10 == a && dependsOn(l.from / 10, b, depth + 1)) return true;
    return false;
  }
  bool connect(int outPin, int inPin) {
    int on = outPin / 10, in = inPin / 10;
    if (outPin % 10 != 9 || inPin % 10 > 2) return false;
    int ii = indexOf(in);
    if (ii < 0 || indexOf(on) < 0 || inPin % 10 >= nodeInputs(nodes[ii].type)) return false;
    if (on == in || dependsOn(on, in)) return false;
    removeLinkTo(inPin);
    links.push_back({nextLink++, outPin, inPin});
    return true;
  }
  void removeLinkTo(int inPin) {
    links.erase(std::remove_if(links.begin(), links.end(), [&](const Link& l) { return l.to == inPin; }), links.end());
  }
  void removeLink(int id) {
    links.erase(std::remove_if(links.begin(), links.end(), [&](const Link& l) { return l.id == id; }), links.end());
  }
  void removeNode(int id) {
    int i = indexOf(id);
    if (i < 0 || nodes[i].type == N_OUTPUT) return;
    links.erase(std::remove_if(links.begin(), links.end(), [&](const Link& l) { return l.from / 10 == id || l.to / 10 == id; }), links.end());
    nodes.erase(nodes.begin() + i);
  }
};

// ---------- noise tileable ----------
inline uint32_t ihash(int x, int y, int s) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)s * 2147483647u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}
inline float h01(int x, int y, int s) { return (ihash(x, y, s) & 0xFFFFFF) / 16777215.0f; }
inline int wrapi(int a, int p) { int r = a % p; return r < 0 ? r + p : r; }

inline float vnoise(float u, float v, int per, int seed) {
  float x = u * per, y = v * per;
  int xi = (int)std::floor(x), yi = (int)std::floor(y);
  float fx = x - xi, fy = y - yi;
  fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
  int x0 = wrapi(xi, per), x1 = wrapi(xi + 1, per), y0 = wrapi(yi, per), y1 = wrapi(yi + 1, per);
  float a = h01(x0, y0, seed), b = h01(x1, y0, seed), c = h01(x0, y1, seed), d = h01(x1, y1, seed);
  return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
}
inline float fbm(float u, float v, int scale, int oct, float pers, int seed) {
  float s = 0, amp = 1, tot = 0; int per = std::max(1, scale);
  for (int o = 0; o < oct; o++) { s += amp * vnoise(u, v, per, seed + o * 17); tot += amp; amp *= pers; per *= 2; }
  return s / tot;
}
inline float voronoi(float u, float v, int per, int seed) {
  per = std::max(1, per);
  float x = u * per, y = v * per; int xi = (int)std::floor(x), yi = (int)std::floor(y);
  float best = 10.f;
  for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) {
    int cx = xi + i, cy = yi + j, wx = wrapi(cx, per), wy = wrapi(cy, per);
    float px = cx + h01(wx, wy, seed), py = cy + h01(wx, wy, seed + 1);
    float d = std::sqrt((px - x) * (px - x) + (py - y) * (py - y));
    best = std::min(best, d);
  }
  return std::min(1.f, best * 1.3f);
}

struct Evaluator {
  const Graph* g = nullptr;
  std::vector<std::array<int, 3>> src;
  int out = -1;

  void prepare(const Graph& gr) {
    g = &gr; out = -1;
    src.assign(gr.nodes.size(), {-1, -1, -1});
    for (size_t i = 0; i < gr.nodes.size(); i++) {
      if (gr.nodes[i].type == N_OUTPUT) out = (int)i;
      for (int k = 0; k < 3; k++)
        for (auto& l : gr.links) if (l.to == pinIn(gr.nodes[i].id, k)) src[i][k] = gr.indexOf(l.from / 10);
    }
  }
  glm::vec3 in(int ni, int k, float u, float v, glm::vec3 def) const {
    int s = src[ni][k];
    return s >= 0 ? eval(s, u, v) : def;
  }
  glm::vec3 eval(int ni, float u, float v) const {
    const Node& n = g->nodes[ni];
    glm::vec3 c0(n.c0[0], n.c0[1], n.c0[2]), c1(n.c1[0], n.c1[1], n.c1[2]);
    switch (n.type) {
      case N_NOISE: return glm::vec3(fbm(u, v, n.i0, n.i1, n.f0, n.i2));
      case N_VORONOI: return glm::vec3(voronoi(u, v, n.i0, n.i2));
      case N_CHECKER: { int a = (int)std::floor(u * n.i0) + (int)std::floor(v * n.i0); return (a & 1) ? c1 : c0; }
      case N_COLOR: return c0;
      case N_RAMP: {
        float f = in(ni, 0, u, v, glm::vec3(0.5f)).x;
        float t = glm::clamp((f - n.f0) / std::max(n.f1 - n.f0, 1e-4f), 0.f, 1.f);
        return glm::mix(c0, c1, t);
      }
      case N_MIX: {
        glm::vec3 a = in(ni, 0, u, v, c0), b = in(ni, 1, u, v, c1);
        float f = glm::clamp(in(ni, 2, u, v, glm::vec3(n.f0)).x, 0.f, 1.f);
        switch (n.i0) {
          case 1: return glm::mix(a, a * b, f);
          case 2: return a + b * f;
          case 3: return glm::mix(a, 1.f - (1.f - a) * (1.f - b), f);
          default: return glm::mix(a, b, f);
        }
      }
      case N_INVERT: return 1.f - in(ni, 0, u, v, c0);
    }
    return glm::vec3(0);
  }
  void outputs(float u, float v, glm::vec3& alb, float& h, float& r) const {
    if (out < 0) { alb = glm::vec3(0.5f); h = 0.5f; r = 0.6f; return; }
    const Node& n = g->nodes[out];
    alb = in(out, 0, u, v, glm::vec3(n.c0[0], n.c0[1], n.c0[2]));
    h = src[out][1] >= 0 ? eval(src[out][1], u, v).x : n.f0;
    r = src[out][2] >= 0 ? eval(src[out][2], u, v).x : n.f1;
  }
};

// Height -> normal map (tangent space, OpenGL Y+), tileable
inline void heightToNormal(const std::vector<float>& h, int res, float strength, std::vector<uint8_t>& rgba) {
  rgba.assign((size_t)res * res * 4, 255);
  float k = strength * res / 512.f;
  parallelFor(res, [&](int y) {
    for (int x = 0; x < res; x++) {
      auto H = [&](int xx, int yy) { return h[(size_t)wrapi(yy, res) * res + wrapi(xx, res)]; };
      float dx = (H(x + 1, y) - H(x - 1, y)) * 0.5f * k;
      float dy = (H(x, y + 1) - H(x, y - 1)) * 0.5f * k;
      glm::vec3 n = glm::normalize(glm::vec3(-dx, -dy, 1.f)) * 0.5f + 0.5f;
      size_t i = ((size_t)y * res + x) * 4;
      rgba[i] = (uint8_t)(n.x * 255.f + .5f); rgba[i + 1] = (uint8_t)(n.y * 255.f + .5f); rgba[i + 2] = (uint8_t)(n.z * 255.f + .5f);
    }
  });
}
