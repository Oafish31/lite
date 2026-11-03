// LiteTex - texturing tool ringan: node texture (gaya Blender) + paint & bake (gaya Substance Painter)
// Target: OpenGL 3.3 core (Intel HD 4400 / i3-4170 OK)
#define SDL_MAIN_HANDLED
#include <GL/glew.h>
#include <SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include "imnodes.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <filesystem>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cctype>
#include "fileutil.h"
#include "nodes.h"
#include "mesh.h"

// ===================== state =====================
static Graph graph;
static Mesh lowMesh, highMesh;
static Bvh highBvh;
static bool hasHigh = false;

static GLuint prog, vao, vbo, ebo;
static GLsizei indexCount = 0;
static GLuint texAlbedo, texNormal, texRough, texPaint, texAO, texDecal;

static const int resOpts[] = {256, 512, 1024, 2048};
static int texResIdx = 1, bakeResIdx = 1;
static float normStrength = 6.f;
static std::vector<float> heightMap;
static std::vector<uint8_t> albedoPx, roughPx, normalPx;
static int curRes = 0;
static std::vector<uint8_t> bakedNormal, bakedAO;
static int bakedNRes = 0, bakedAORes = 0;
static bool useAO = false;
struct ImgTex { int w = 0, h = 0; std::vector<uint8_t> px; bool ok() const { return !px.empty(); } };
static ImgTex impNormal, impAlbedo;     // texture hasil import file
static int normalSrc = 0;               // 0 = node, 1 = hasil bake, 2 = file import
static int albedoSrc = 0;               // 0 = node, 1 = file import
static bool flipY = false;

static int PRES = 2048;   // resolusi layer paint & decal (bisa diganti di tab Texture)
static int layerResIdx = 1;
static std::vector<uint8_t> paintPx((size_t)PRES * PRES * 4, 0);
static int pdy0 = PRES, pdy1 = -1;

static bool dirty = true, needFull = false;
static std::string status = "Siap.";

// kamera
static float yaw = 0.6f, pitch = 0.4f, dist = 3.f;
static glm::vec3 target(0);
static glm::mat4 gView(1), gProj(1);

// ===================== GL util =====================
static const char* VS = R"(#version 330 core
layout(location=0) in vec3 aPos; layout(location=1) in vec3 aN; layout(location=2) in vec2 aUV; layout(location=3) in vec3 aT;
uniform mat4 uMVP; out vec3 vP; out vec3 vN; out vec3 vT; out vec2 vUV;
void main(){ vP=aPos; vN=aN; vT=aT; vUV=aUV; gl_Position=uMVP*vec4(aPos,1.0); })";

static const char* FS = R"(#version 330 core
in vec3 vP; in vec3 vN; in vec3 vT; in vec2 vUV; out vec4 o;
uniform sampler2D uAlbedo; uniform sampler2D uNormal; uniform sampler2D uRough; uniform sampler2D uPaint; uniform sampler2D uAO; uniform sampler2D uDecal;
uniform vec3 uEye; uniform int uMode; uniform float uNStr; uniform int uUseAO;
void main(){
  vec3 N=normalize(vN); vec3 T=normalize(vT-N*dot(N,vT)); vec3 B=cross(N,T);
  vec3 nm=texture(uNormal,vUV).xyz*2.0-1.0; nm.xy*=uNStr; nm.xy+=texture(uDecal,vUV).rg;
  vec3 Nn=normalize(mat3(T,B,N)*nm);
  vec4 pa=texture(uPaint,vUV);
  vec3 alb=mix(texture(uAlbedo,vUV).rgb,pa.rgb,pa.a);
  float ao=(uUseAO==1)?texture(uAO,vUV).r:1.0;
  float rough=texture(uRough,vUV).r;
  if(uMode==1){o=vec4(alb,1);return;}
  if(uMode==2){o=vec4(Nn*0.5+0.5,1);return;}
  if(uMode==3){o=vec4(vec3(ao),1);return;}
  vec3 V=normalize(uEye-vP);
  vec3 L0=normalize(vec3(0.5,0.8,0.6)); vec3 L1=V;
  vec3 a=pow(alb,vec3(2.2));
  float d0=max(dot(Nn,L0),0.0), d1=max(dot(Nn,L1),0.0);
  float shin=mix(96.0,4.0,rough);
  float s0=pow(max(dot(Nn,normalize(L0+V)),0.0),shin)*(1.0-rough)*0.4;
  float s1=pow(max(dot(Nn,normalize(L1+V)),0.0),shin)*(1.0-rough)*0.2;
  vec3 col=a*(0.12+d0*0.8+d1*0.3)*ao+vec3(s0+s1)*ao;
  o=vec4(pow(col,vec3(1.0/2.2)),1.0);
})";

static GLuint compile(GLenum t, const char* src) {
  GLuint s = glCreateShader(t); glShaderSource(s, 1, &src, nullptr); glCompileShader(s);
  GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { char b[1024]; glGetShaderInfoLog(s, 1024, nullptr, b); fprintf(stderr, "Shader error: %s\n", b); }
  return s;
}
static GLuint makeTex() {
  GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameterf(GL_TEXTURE_2D, 0x84FE, 8.0f);   // anisotropic filtering 8x (tajam di sudut miring)
  return t;
}
static void uploadTex(GLuint t, int w, int h, const void* data, bool mip) {
  glBindTexture(GL_TEXTURE_2D, t); glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
  if (mip) { glGenerateMipmap(GL_TEXTURE_2D); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); }
  else glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
}
static void uploadMesh(const Mesh& m) {
  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferData(GL_ARRAY_BUFFER, m.v.size() * sizeof(Vertex), m.v.data(), GL_STATIC_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo); glBufferData(GL_ELEMENT_ARRAY_BUFFER, m.idx.size() * 4, m.idx.data(), GL_STATIC_DRAW);
  glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, p));
  glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, n));
  glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));
  glEnableVertexAttribArray(3); glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, t));
  indexCount = (GLsizei)m.idx.size();
}
static float lowDiag = 1.f;
static void setLowMesh(Mesh&& m) {
  lowMesh = std::move(m); uploadMesh(lowMesh); lowDiag = meshDiag(lowMesh);
  glm::vec3 mn(1e30f), mx(-1e30f);
  for (auto& v : lowMesh.v) { mn = glm::min(mn, v.p); mx = glm::max(mx, v.p); }
  target = (mn + mx) * 0.5f; dist = glm::length(mx - mn) * 1.3f;
}

// ===================== tekstur dari node =====================
static void applyNormalTex() {
  if (normalSrc == 2 && impNormal.ok()) uploadTex(texNormal, impNormal.w, impNormal.h, impNormal.px.data(), true);
  else if (normalSrc == 1 && !bakedNormal.empty()) uploadTex(texNormal, bakedNRes, bakedNRes, bakedNormal.data(), true);
  else if (!normalPx.empty()) uploadTex(texNormal, curRes, curRes, normalPx.data(), true);
}
static void applyAlbedoTex() {
  if (albedoSrc == 1 && impAlbedo.ok()) uploadTex(texAlbedo, impAlbedo.w, impAlbedo.h, impAlbedo.px.data(), true);
  else if (!albedoPx.empty()) uploadTex(texAlbedo, curRes, curRes, albedoPx.data(), true);
}
static void regenMaps(int res) {
  Evaluator ev; ev.prepare(graph); curRes = res;
  albedoPx.assign((size_t)res * res * 4, 255); roughPx.assign((size_t)res * res * 4, 255); heightMap.assign((size_t)res * res, 0.f);
  parallelFor(res, [&](int y) {
    auto B = [](float f) { return (uint8_t)(glm::clamp(f, 0.f, 1.f) * 255.f + .5f); };
    for (int x = 0; x < res; x++) {
      glm::vec3 a; float h, r;
      ev.outputs((x + .5f) / res, (y + .5f) / res, a, h, r);
      size_t i = (size_t)y * res + x;
      albedoPx[i * 4] = B(a.x); albedoPx[i * 4 + 1] = B(a.y); albedoPx[i * 4 + 2] = B(a.z);
      roughPx[i * 4] = roughPx[i * 4 + 1] = roughPx[i * 4 + 2] = B(r);
      heightMap[i] = h;
    }
  });
  heightToNormal(heightMap, res, normStrength, normalPx);
  applyAlbedoTex();
  uploadTex(texRough, res, res, roughPx.data(), false);
  applyNormalTex();
}

// ===================== paint =====================
static glm::vec3 brushCol(0.9f, 0.2f, 0.2f);
static float brushSize = 24.f, brushHard = 0.6f, brushOpac = 0.8f;
static int tool = 1; static bool eraser = false;
static bool lastValid = false; static glm::vec2 lastUV;

static void stamp(float u, float v) {
  float cx = u * PRES, cy = v * PRES, r = brushSize * PRES / 1024.f;
  int x0 = std::max(0, (int)(cx - r)), x1 = std::min(PRES - 1, (int)(cx + r));
  int y0 = std::max(0, (int)(cy - r)), y1 = std::min(PRES - 1, (int)(cy + r));
  for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
    float d = std::sqrt((x + .5f - cx) * (x + .5f - cx) + (y + .5f - cy) * (y + .5f - cy)) / r;
    if (d > 1.f) continue;
    float f = d <= brushHard ? 1.f : 1.f - (d - brushHard) / (1.f - brushHard + 1e-4f);
    float a = brushOpac * f;
    uint8_t* p = &paintPx[((size_t)y * PRES + x) * 4];
    float da = p[3] / 255.f;
    if (eraser) { p[3] = (uint8_t)(std::max(0.f, da - a) * 255.f + .5f); continue; }
    float oa = a + da * (1 - a);
    for (int k = 0; k < 3; k++) {
      float dc = p[k] / 255.f, c = (brushCol[k] * a + dc * da * (1 - a)) / std::max(oa, 1e-5f);
      p[k] = (uint8_t)(glm::clamp(c, 0.f, 1.f) * 255.f + .5f);
    }
    p[3] = (uint8_t)(oa * 255.f + .5f);
  }
  pdy0 = std::min(pdy0, y0); pdy1 = std::max(pdy1, y1);
}
static void flushPaint() {
  if (pdy1 < pdy0) return;
  glBindTexture(GL_TEXTURE_2D, texPaint); glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, pdy0, PRES, pdy1 - pdy0 + 1, GL_RGBA, GL_UNSIGNED_BYTE, &paintPx[(size_t)pdy0 * PRES * 4]);
  pdy0 = PRES; pdy1 = -1;
}
struct Pick { bool ok = false; glm::vec2 uv{0}, upw{1}; glm::vec3 pos{0}, nrm{0, 1, 0}, tan{1, 0, 0}; };
static Pick pickSurface(const ImVec2& mp, const ImVec2& ds) {
  Pick r;
  float x = 2.f * mp.x / ds.x - 1.f, y = 1.f - 2.f * mp.y / ds.y;
  glm::mat4 inv = glm::inverse(gProj * gView);
  glm::vec4 n = inv * glm::vec4(x, y, -1, 1), f = inv * glm::vec4(x, y, 1, 1);
  glm::vec3 o = glm::vec3(n) / n.w, d = glm::normalize(glm::vec3(f) / f.w - o);
  Hit h;
  if (!raycastBrute(lowMesh, o, d, h)) return r;
  const Vertex &a = lowMesh.v[lowMesh.idx[h.tri * 3]], &b = lowMesh.v[lowMesh.idx[h.tri * 3 + 1]], &c = lowMesh.v[lowMesh.idx[h.tri * 3 + 2]];
  r.uv = a.uv * (1 - h.b1 - h.b2) + b.uv * h.b1 + c.uv * h.b2;
  r.pos = o + d * h.t;
  r.nrm = glm::normalize(a.n * (1 - h.b1 - h.b2) + b.n * h.b1 + c.n * h.b2);
  r.tan = a.t * (1 - h.b1 - h.b2) + b.t * h.b1 + c.t * h.b2;
  glm::vec3 e1 = b.p - a.p, e2 = c.p - a.p; glm::vec2 d1 = b.uv - a.uv, d2 = c.uv - a.uv;
  float det = d1.x * d2.y - d2.x * d1.y;
  if (std::fabs(det) > 1e-12f) {
    glm::vec3 pu = (e1 * d2.y - e2 * d1.y) / det, pv = (e2 * d1.x - e1 * d2.x) / det;
    float lu = glm::length(pu), lv = glm::length(pv);
    if (lu > 1e-8f && lv > 1e-8f) r.upw = glm::vec2(1.f / lu, 1.f / lv);
  }
  r.ok = true; return r;
}
static void paintAt(glm::vec2 uv) {
  if (lastValid && glm::length(uv - lastUV) < 0.25f) {
    float step = std::max(1.f / PRES, brushSize * 0.25f / 1024.f), d = glm::length(uv - lastUV);
    int n = (int)std::ceil(d / step);
    for (int i = 1; i <= n; i++) { glm::vec2 p = glm::mix(lastUV, uv, (float)i / n); stamp(p.x, p.y); }
  } else stamp(uv.x, uv.y);
  lastUV = uv; lastValid = true;
}


// ===================== normal decal =====================
// Decal disimpan sebagai slope xy + mask, lengkap dengan mip-chain (supaya tidak aliasing / "bertitik" saat diperkecil)
struct DecalLevel { int w = 0, h = 0; std::vector<glm::vec2> xy; std::vector<float> m; };
struct Decal { std::string name; int w = 0, h = 0; std::vector<DecalLevel> lv; GLuint thumb = 0; };
static std::vector<Decal> decals; static int decalSel = 0;
static std::vector<float> decalPx((size_t)PRES * PRES * 2, 0.f);   // layer: rg = offset slope (float, presisi tinggi)
static int ddy0 = PRES, ddy1 = -1; static bool decalUsed = false;
static std::vector<std::vector<float>> decalUndo;
static float decalSize = 0.08f, decalRot = 0.f, decalStrength = 1.f, decalOpac = 1.f; static bool decalInvert = false, decalRandom = false;
static char decalPath[512] = ""; static int decalIsNormal = 1; static bool decalFlipY = false, decalFixBias = true;

static float ssf(float a, float b, float x) { float t = glm::clamp((x - a) / (b - a), 0.f, 1.f); return t * t * (3 - 2 * t); }

static DecalLevel halfLevel(const DecalLevel& s) {
  DecalLevel r; r.w = std::max(1, s.w / 2); r.h = std::max(1, s.h / 2);
  r.xy.resize((size_t)r.w * r.h); r.m.resize(r.xy.size());
  for (int y = 0; y < r.h; y++) for (int x = 0; x < r.w; x++) {
    float ms = 0; glm::vec2 a(0);
    for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) {
      size_t i = (size_t)std::min(s.h - 1, y * 2 + dy) * s.w + std::min(s.w - 1, x * 2 + dx);
      ms += s.m[i]; a += s.xy[i] * s.m[i];
    }
    size_t o = (size_t)y * r.w + x; r.m[o] = ms * 0.25f; r.xy[o] = ms > 1e-6f ? a / ms : glm::vec2(0);
  }
  return r;
}
static void heightToXY(const std::vector<float>& H, int w, int h, float depth, std::vector<glm::vec2>& xy) {
  xy.resize((size_t)w * h);
  auto G = [&](int x, int y) { x = glm::clamp(x, 0, w - 1); y = glm::clamp(y, 0, h - 1); return H[(size_t)y * w + x]; };
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
    float dx = (G(x + 1, y) - G(x - 1, y)) * w / 4.f * depth, dy = (G(x, y + 1) - G(x, y - 1)) * h / 4.f * depth;
    glm::vec3 n = glm::normalize(glm::vec3(-dx, -dy, 1.f)); xy[(size_t)y * w + x] = glm::vec2(n.x, n.y);
  }
}
static void finishDecalXY(Decal& d, DecalLevel&& base) {
  while (base.w > 2048 || base.h > 2048) base = halfLevel(base);   // batasi memori
  d.w = base.w; d.h = base.h; d.lv.clear(); d.lv.push_back(std::move(base));
  while (d.lv.back().w > 1 || d.lv.back().h > 1) d.lv.push_back(halfLevel(d.lv.back()));
  int ti = 0; while (ti + 1 < (int)d.lv.size() && d.lv[ti].w > 128) ti++;
  const DecalLevel& T = d.lv[ti]; std::vector<uint8_t> px((size_t)T.w * T.h * 4, 255);
  for (size_t i = 0; i < T.xy.size(); i++) {
    glm::vec2 v = T.xy[i]; float z = std::sqrt(std::max(0.f, 1.f - glm::dot(v, v)));
    glm::vec3 c = glm::mix(glm::vec3(.5f, .5f, 1.f), glm::vec3(v.x, v.y, z) * 0.5f + 0.5f, glm::clamp(T.m[i], 0.f, 1.f));
    for (int k = 0; k < 3; k++) px[i * 4 + k] = (uint8_t)(glm::clamp(c[k], 0.f, 1.f) * 255.f + .5f);
  }
  d.thumb = makeTex(); uploadTex(d.thumb, T.w, T.h, px.data(), false);
  decals.push_back(std::move(d));
}
template <class F> static void procDecal(const char* name, float depth, F fn) {
  Decal d; d.name = name; const int N = 256;
  std::vector<float> H((size_t)N * N); DecalLevel L; L.w = L.h = N; L.m.resize(H.size());
  for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
    float px = (x + .5f) / (N / 2.f) - 1.f, py = (y + .5f) / (N / 2.f) - 1.f, h, m;
    fn(px, py, h, m); H[(size_t)y * N + x] = h; L.m[(size_t)y * N + x] = m;
  }
  heightToXY(H, N, N, depth, L.xy); finishDecalXY(d, std::move(L));
}
static bool loadDecalPNG(const char* path, bool isNormal, bool flipGreenY, std::string& err) {
  int w, h, c; stbi_set_flip_vertically_on_load(1);
  unsigned char* px = stbi_load(path, &w, &h, &c, 4); stbi_set_flip_vertically_on_load(0);
  if (!px) { err = std::string("Gagal load PNG: ") + path; return false; }
  Decal d; d.name = std::filesystem::path(path).filename().string();
  size_t N = (size_t)w * h; DecalLevel L; L.w = w; L.h = h; L.m.assign(N, 1.f); L.xy.assign(N, glm::vec2(0));
  bool anyAlpha = false; for (size_t i = 0; i < N; i++) if (px[i * 4 + 3] < 255) { anyAlpha = true; break; }
  if (anyAlpha) for (size_t i = 0; i < N; i++) L.m[i] = px[i * 4 + 3] / 255.f;
  if (isNormal) {
    // hanya pakai kanal R,G (slope). Kanal B diabaikan -> aman untuk normal map hasil render/tone-mapping yang birunya tidak 255
    for (size_t i = 0; i < N; i++) { float x = px[i * 4] / 127.5f - 1.f, y = px[i * 4 + 1] / 127.5f - 1.f; L.xy[i] = glm::vec2(x, flipGreenY ? -y : y); }
    if (decalFixBias) {   // buang kemiringan dasar (area datar tidak 128,128) pakai median
      std::vector<float> xs, ys; size_t step = std::max<size_t>(1, N / 200000);
      for (size_t i = 0; i < N; i += step) if (L.m[i] > 0.9f) { xs.push_back(L.xy[i].x); ys.push_back(L.xy[i].y); }
      if (xs.size() > 16) {
        std::nth_element(xs.begin(), xs.begin() + xs.size() / 2, xs.end()); std::nth_element(ys.begin(), ys.begin() + ys.size() / 2, ys.end());
        glm::vec2 bias(xs[xs.size() / 2], ys[ys.size() / 2]);
        for (size_t i = 0; i < N; i++) L.xy[i] -= bias;
      }
    }
  } else {
    std::vector<float> H(N);
    for (size_t i = 0; i < N; i++) H[i] = (0.299f * px[i * 4] + 0.587f * px[i * 4 + 1] + 0.114f * px[i * 4 + 2]) / 255.f;
    heightToXY(H, w, h, 0.3f, L.xy);
  }
  for (size_t i = 0; i < N; i++) {
    if (L.m[i] <= 0.f) L.xy[i] = glm::vec2(0);
    float len = glm::length(L.xy[i]); if (len > 0.98f) L.xy[i] *= 0.98f / len;
  }
  stbi_image_free(px); finishDecalXY(d, std::move(L)); return true;
}
static void uploadDecalFull() {
  glBindTexture(GL_TEXTURE_2D, texDecal); glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, PRES, PRES, 0, GL_RG, GL_FLOAT, decalPx.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
}
static void initDecals() {
  texDecal = makeTex(); uploadDecalFull();
  procDecal("Rivet", 0.35f, [](float x, float y, float& h, float& m) {
    float r = std::sqrt(x * x + y * y);
    h = r < 0.78f ? 0.8f * std::sqrt(1.f - (r / 0.78f) * (r / 0.78f)) : (r < 0.9f ? -0.25f : 0.f);
    m = 1.f - ssf(0.88f, 0.97f, r); });
  procDecal("Screw", 0.35f, [](float x, float y, float& h, float& m) {
    float r = std::sqrt(x * x + y * y), ax = std::fabs(x), ay = std::fabs(y);
    float base = 0.6f * (1.f - ssf(0.78f, 0.9f, r));
    float s1 = (1.f - ssf(0.07f, 0.12f, ay)) * (1.f - ssf(0.65f, 0.72f, ax)), s2 = (1.f - ssf(0.07f, 0.12f, ax)) * (1.f - ssf(0.65f, 0.72f, ay));
    h = base - 0.55f * std::max(s1, s2) * (r < 0.85f ? 1.f : 0.f); m = 1.f - ssf(0.88f, 0.97f, r); });
  procDecal("Hex Bolt", 0.35f, [](float x, float y, float& h, float& m) {
    float ax = std::fabs(x), ay = std::fabs(y), r = std::sqrt(x * x + y * y), hx = std::max(ax * 0.866f + ay * 0.5f, ay);
    h = 0.7f * (1.f - ssf(0.62f, 0.72f, hx)) - 0.4f * (1.f - ssf(0.25f, 0.3f, r)); m = 1.f - ssf(0.78f, 0.86f, hx); });
  procDecal("Scratch", 0.5f, [](float x, float y, float& h, float& m) {
    float wob = 0.12f * std::sin(x * 3.f) + 0.05f * std::sin(x * 9.f), d = std::fabs(y - wob), ax = std::fabs(x);
    float w = 0.07f * (1.f - ssf(0.6f, 1.f, ax)) + 0.005f;
    h = -(1.f - ssf(0.f, w * 1.5f, d)); m = (1.f - ssf(0.85f, 1.f, ax)) * (1.f - ssf(w * 2.f, w * 3.5f, d)); });
  procDecal("Panel Line", 0.5f, [](float x, float y, float& h, float& m) {
    float bd = std::min(0.92f - std::fabs(x), 0.55f - std::fabs(y));
    float g = 1.f - ssf(0.f, 0.03f, std::fabs(bd - 0.04f));
    h = -0.9f * g + 0.12f * ssf(0.06f, 0.1f, bd); m = ssf(-0.04f, 0.f, bd); });
  procDecal("Dent", 0.4f, [](float x, float y, float& h, float& m) {
    float r2 = x * x + y * y; h = -std::exp(-r2 * 5.f); m = 1.f - ssf(0.7f, 1.f, std::sqrt(r2)); });
  procDecal("Bump", 0.4f, [](float x, float y, float& h, float& m) {
    float r2 = x * x + y * y; h = std::exp(-r2 * 5.f); m = 1.f - ssf(0.7f, 1.f, std::sqrt(r2)); });
}
static void sampleLevel(const DecalLevel& L, float u, float v, glm::vec2& xy, float& m) {
  float fx = glm::clamp(u * L.w - 0.5f, 0.f, (float)L.w - 1.f), fy = glm::clamp(v * L.h - 0.5f, 0.f, (float)L.h - 1.f);
  int x = std::min((int)fx, L.w - 1), y = std::min((int)fy, L.h - 1), x1 = std::min(x + 1, L.w - 1), y1 = std::min(y + 1, L.h - 1);
  float tx = fx - x, ty = fy - y;
  size_t i[4] = {(size_t)y * L.w + x, (size_t)y * L.w + x1, (size_t)y1 * L.w + x, (size_t)y1 * L.w + x1};
  float w[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
  glm::vec2 a(0); float ms = 0;
  for (int k = 0; k < 4; k++) { ms += w[k] * L.m[i[k]]; a += w[k] * L.m[i[k]] * L.xy[i[k]]; }
  m = ms; xy = ms > 1e-6f ? a / ms : glm::vec2(0);
}
// trilinear: pilih level mip sesuai ukuran texel tujuan -> tepi tipis tidak hilang / bertitik
static void sampleDecal(const Decal& d, float u, float v, float lod, glm::vec2& xy, float& m) {
  lod = glm::clamp(lod, 0.f, (float)d.lv.size() - 1.f);
  int l0 = (int)lod, l1 = std::min(l0 + 1, (int)d.lv.size() - 1); float f = lod - l0;
  glm::vec2 a, b; float ma, mb; sampleLevel(d.lv[l0], u, v, a, ma);
  if (l1 == l0 || f < 1e-3f) { xy = a; m = ma; return; }
  sampleLevel(d.lv[l1], u, v, b, mb);
  float wa = ma * (1 - f), wb = mb * f; m = glm::mix(ma, mb, f); xy = (a * wa + b * wb) / std::max(wa + wb, 1e-6f);
}
static uint8_t encSlope(float v) { return (uint8_t)glm::clamp(v * 127.f + 128.5f, 0.f, 255.f); }
// Proyeksi 3D: tiap texel UV dihitung posisi 3D-nya, lalu diproyeksikan ke bidang decal -> tembus seam UV dengan mulus
static void stampDecal(const Decal& d, const Pick& pk) {
  float halfW = lowDiag * decalSize * 0.5f, ang = glm::radians(decalRot);
  if (decalRandom) ang += (std::rand() / (float)RAND_MAX) * 6.2831853f;
  float c = std::cos(ang), s = std::sin(ang), sign = decalInvert ? -1.f : 1.f, R = halfW * 1.4143f;
  float ws = 0.5f * (1.f / (PRES * pk.upw.x) + 1.f / (PRES * pk.upw.y));   // ukuran 1 texel layer di dunia
  float lod = std::log2(std::max(1.f, (d.w / (2.f * halfW)) * ws));
  glm::vec3 N0 = pk.nrm, T0 = pk.tan - N0 * glm::dot(N0, pk.tan);
  T0 = glm::length(T0) > 1e-6f ? glm::normalize(T0) : glm::normalize(glm::cross(glm::vec3(0, 1, 0.01f), N0));
  glm::vec3 B0 = glm::cross(N0, T0), X = T0 * c + B0 * s, Y = B0 * c - T0 * s;
  std::vector<uint8_t> done((size_t)PRES * PRES, 0);
  int miny = PRES, maxy = -1;
  for (size_t ti = 0; ti < lowMesh.tris(); ti++) {
    const Vertex &A = lowMesh.v[lowMesh.idx[ti * 3]], &B = lowMesh.v[lowMesh.idx[ti * 3 + 1]], &C = lowMesh.v[lowMesh.idx[ti * 3 + 2]];
    glm::vec3 mn = glm::min(A.p, glm::min(B.p, C.p)) - glm::vec3(R), mx = glm::max(A.p, glm::max(B.p, C.p)) + glm::vec3(R);
    if (pk.pos.x < mn.x || pk.pos.y < mn.y || pk.pos.z < mn.z || pk.pos.x > mx.x || pk.pos.y > mx.y || pk.pos.z > mx.z) continue;
    glm::vec2 umn = glm::min(A.uv, glm::min(B.uv, C.uv)), umx = glm::max(A.uv, glm::max(B.uv, C.uv));
    int x0 = std::max(0, (int)std::floor(umn.x * PRES)), x1 = std::min(PRES - 1, (int)std::ceil(umx.x * PRES));
    int y0 = std::max(0, (int)std::floor(umn.y * PRES)), y1 = std::min(PRES - 1, (int)std::ceil(umx.y * PRES));
    float den = (B.uv.y - C.uv.y) * (A.uv.x - C.uv.x) + (C.uv.x - B.uv.x) * (A.uv.y - C.uv.y);
    if (std::fabs(den) < 1e-14f) continue;
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
      if (done[(size_t)y * PRES + x]) continue;
      float u = (x + .5f) / PRES, v = (y + .5f) / PRES;
      float w0 = ((B.uv.y - C.uv.y) * (u - C.uv.x) + (C.uv.x - B.uv.x) * (v - C.uv.y)) / den;
      float w1 = ((C.uv.y - A.uv.y) * (u - C.uv.x) + (A.uv.x - C.uv.x) * (v - C.uv.y)) / den;
      float w2 = 1.f - w0 - w1;
      if (w0 < -1e-5f || w1 < -1e-5f || w2 < -1e-5f) continue;
      glm::vec3 q = A.p * w0 + B.p * w1 + C.p * w2 - pk.pos;
      float depth = glm::dot(q, N0), rx = glm::dot(q, X) / halfW, ry = glm::dot(q, Y) / halfW;
      if (std::fabs(rx) > 1.f || std::fabs(ry) > 1.f || std::fabs(depth) > halfW) continue;
      glm::vec3 ns = glm::normalize(A.n * w0 + B.n * w1 + C.n * w2);
      float face = ssf(0.15f, 0.5f, glm::dot(ns, N0));
      if (face <= 0.f) continue;
      glm::vec3 ts = A.t * w0 + B.t * w1 + C.t * w2; ts = ts - ns * glm::dot(ns, ts);
      ts = glm::length(ts) > 1e-8f ? glm::normalize(ts) : glm::normalize(glm::cross(glm::vec3(0, 1, 0.01f), ns));
      glm::vec3 bs = glm::cross(ns, ts);
      glm::vec2 n; float m; sampleDecal(d, rx * .5f + .5f, ry * .5f + .5f, lod, n, m);
      float k = m * face * (1.f - ssf(0.9f, 1.f, std::max(std::fabs(rx), std::fabs(ry)))) * (1.f - ssf(0.6f, 1.f, std::fabs(depth) / halfW))
                * decalOpac * decalStrength * sign;
      glm::vec3 dn = (X * n.x + Y * n.y) * k;
      float* p = &decalPx[((size_t)y * PRES + x) * 2];
      p[0] = glm::clamp(p[0] + glm::dot(dn, ts), -4.f, 4.f); p[1] = glm::clamp(p[1] + glm::dot(dn, bs), -4.f, 4.f);
      done[(size_t)y * PRES + x] = 1; miny = std::min(miny, y); maxy = std::max(maxy, y);
    }
  }
  if (maxy >= miny) { ddy0 = std::min(ddy0, miny); ddy1 = std::max(ddy1, maxy); decalUsed = true; }
}
static void placeDecal(const Pick& pk) {
  if (decals.empty()) return;
  size_t maxU = std::max<size_t>(1, std::min<size_t>(10, ((size_t)256 << 20) / (decalPx.size() * 4)));
  while (decalUndo.size() >= maxU) decalUndo.erase(decalUndo.begin());
  decalUndo.push_back(decalPx);
  stampDecal(decals[decalSel], pk);
}
static void undoDecal() {
  if (decalUndo.empty()) { status = "Tidak ada decal untuk di-undo"; return; }
  decalPx = decalUndo.back(); decalUndo.pop_back(); ddy0 = 0; ddy1 = PRES - 1;
}
static void clearDecals() {
  decalUndo.clear(); std::fill(decalPx.begin(), decalPx.end(), 0.f);
  ddy0 = 0; ddy1 = PRES - 1; decalUsed = false;
}
static void flushDecal() {
  if (ddy1 < ddy0) return;
  glBindTexture(GL_TEXTURE_2D, texDecal); glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, ddy0, PRES, ddy1 - ddy0 + 1, GL_RG, GL_FLOAT, &decalPx[(size_t)ddy0 * PRES * 2]);
  ddy0 = PRES; ddy1 = -1;
}

// ===================== bake =====================
static float cageRel = 0.05f, aoDistRel = 0.15f; static int aoSamples = 24;

static void bakeNormal() {
  if (!lowMesh.hasUV) { status = "Low-poly tidak punya UV!"; return; }
  if (!hasHigh) { status = "Load high-poly OBJ dulu."; return; }
  auto t0 = std::chrono::steady_clock::now();
  int res = resOpts[bakeResIdx]; float cage = meshDiag(lowMesh) * cageRel;
  bakeUV(lowMesh, res, 128, 128, 255, [&](glm::vec3 p, glm::vec3 n, glm::vec3 t, glm::vec3 b, uint8_t* o) {
    Hit h; glm::vec3 hn = n;
    if (highBvh.intersect(p + n * cage, -n, cage * 2.f, h)) hn = smoothNormalAt(highMesh, h);
    glm::vec3 q = glm::normalize(glm::vec3(glm::dot(hn, t), glm::dot(hn, b), glm::dot(hn, n))) * 0.5f + 0.5f;
    for (int k = 0; k < 3; k++) o[k] = (uint8_t)(glm::clamp(q[k], 0.f, 1.f) * 255.f + .5f);
  }, bakedNormal);
  bakedNRes = res; normalSrc = 1; applyNormalTex();
  double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  status = "Bake normal selesai (" + std::to_string(res) + "px, " + std::to_string(s).substr(0, 4) + "s)";
}
static void bakeAO() {
  if (!lowMesh.hasUV) { status = "Low-poly tidak punya UV!"; return; }
  Bvh lowBvh; const Mesh* src = hasHigh ? &highMesh : &lowMesh;
  const Bvh* bvh = &highBvh;
  if (!hasHigh) { lowBvh.build(lowMesh); bvh = &lowBvh; }
  auto t0 = std::chrono::steady_clock::now();
  int res = resOpts[bakeResIdx]; float diag = meshDiag(*src), dmax = diag * aoDistRel, eps = diag * 0.002f; int ns = aoSamples;
  bakeUV(lowMesh, res, 255, 255, 255, [&](glm::vec3 p, glm::vec3 n, glm::vec3 t, glm::vec3 b, uint8_t* o) {
    uint32_t st = ihash((int)(p.x * 7919), (int)(p.y * 7919), (int)(p.z * 7919)) | 1u;
    auto rnd = [&]() { st ^= st << 13; st ^= st >> 17; st ^= st << 5; return (st & 0xFFFFFF) / 16777216.f; };
    int hits = 0;
    for (int i = 0; i < ns; i++) {
      float r1 = rnd() * 6.2831853f, r2 = rnd(), sr = std::sqrt(r2);
      glm::vec3 d = t * (sr * std::cos(r1)) + b * (sr * std::sin(r1)) + n * std::sqrt(1.f - r2);
      Hit h; if (bvh->intersect(p + n * eps, d, dmax, h)) hits++;
    }
    uint8_t v = (uint8_t)((1.f - (float)hits / ns) * 255.f + .5f);
    o[0] = o[1] = o[2] = v;
  }, bakedAO);
  bakedAORes = res; useAO = true; uploadTex(texAO, res, res, bakedAO.data(), true);
  double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  status = "Bake AO selesai (" + std::to_string(res) + "px, " + std::to_string(s).substr(0, 4) + "s)";
}

// ===================== export =====================
static bool writePNG(const std::string& path, int w, int h, const uint8_t* d) {
  stbi_flip_vertically_on_write(1);
  return stbi_write_png(path.c_str(), w, h, 4, d, w * 4) != 0;
}
static bool exportNormal(const std::string& dir) {
  const uint8_t* base = normalPx.data(); int bw = curRes, bh = curRes;
  if (normalSrc == 2 && impNormal.ok()) { base = impNormal.px.data(); bw = impNormal.w; bh = impNormal.h; }
  else if (normalSrc == 1 && !bakedNormal.empty()) { base = bakedNormal.data(); bw = bh = bakedNRes; }
  if (!decalUsed) return writePNG(dir + "/normal.png", bw, bh, base);
  int W = std::max(bw, PRES), H = std::max(bh, PRES); std::vector<uint8_t> out((size_t)W * H * 4, 255);
  parallelFor(H, [&](int y) {
    for (int x = 0; x < W; x++) {
      size_t bi = ((size_t)(y * bh / H) * bw + (x * bw / W)) * 4, di = ((size_t)(y * PRES / H) * PRES + (x * PRES / W)) * 2, o = ((size_t)y * W + x) * 4;
      glm::vec3 n(base[bi] / 127.5f - 1.f + decalPx[di], base[bi + 1] / 127.5f - 1.f + decalPx[di + 1], base[bi + 2] / 127.5f - 1.f);
      n = glm::normalize(n) * 0.5f + 0.5f;
      for (int k = 0; k < 3; k++) out[o + k] = (uint8_t)(glm::clamp(n[k], 0.f, 1.f) * 255.f + .5f);
    }
  });
  return writePNG(dir + "/normal.png", W, H, out.data());
}
static void exportAll(const std::string& dir) {
  namespace fs = std::filesystem;
  std::error_code ec; fs::create_directories(dir, ec);
  regenMaps(resOpts[texResIdx]); dirty = false;
  int r = curRes;
  bool ia = albedoSrc == 1 && impAlbedo.ok();
  const uint8_t* ab = ia ? impAlbedo.px.data() : albedoPx.data(); int aw = ia ? impAlbedo.w : r, ah = ia ? impAlbedo.h : r;
  int OW = std::max(aw, PRES), OH = std::max(ah, PRES);
  std::vector<uint8_t> alb((size_t)OW * OH * 4);
  parallelFor(OH, [&](int y) {
    for (int x = 0; x < OW; x++) {
      size_t bi = ((size_t)(y * ah / OH) * aw + (x * aw / OW)) * 4, pi = ((size_t)(y * PRES / OH) * PRES + (x * PRES / OW)) * 4, o = ((size_t)y * OW + x) * 4;
      float a = paintPx[pi + 3] / 255.f;
      for (int k = 0; k < 3; k++) alb[o + k] = (uint8_t)(ab[bi + k] * (1 - a) + paintPx[pi + k] * a + .5f);
      alb[o + 3] = 255;
    }
  });
  std::vector<uint8_t> hp((size_t)r * r * 4, 255);
  for (size_t i = 0; i < (size_t)r * r; i++) hp[i * 4] = hp[i * 4 + 1] = hp[i * 4 + 2] = (uint8_t)(glm::clamp(heightMap[i], 0.f, 1.f) * 255.f + .5f);
  bool ok = writePNG(dir + "/albedo.png", OW, OH, alb.data());
  ok &= writePNG(dir + "/roughness.png", r, r, roughPx.data());
  ok &= writePNG(dir + "/height.png", r, r, hp.data());
  ok &= exportNormal(dir);
  if (!bakedAO.empty()) ok &= writePNG(dir + "/ao.png", bakedAORes, bakedAORes, bakedAO.data());
  status = ok ? "Export selesai -> folder '" + dir + "'" : "Export gagal (cek folder / izin tulis)";
}

// ===================== UI =====================
static void nodeParams(Node& n) {
  bool ch = false;
  ImGui::PushItemWidth(90);
  auto col = [&](const char* id, float* c) { ch |= ImGui::ColorEdit3(id, c, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel); };
  switch (n.type) {
    case N_NOISE:
      ch |= ImGui::DragInt("Scale", &n.i0, 0.2f, 1, 64); ch |= ImGui::DragInt("Detail", &n.i1, 0.1f, 1, 8);
      ch |= ImGui::DragFloat("Rough", &n.f0, 0.005f, 0.1f, 0.9f); ch |= ImGui::DragInt("Seed", &n.i2, 0.2f, 0, 999); break;
    case N_VORONOI: ch |= ImGui::DragInt("Scale", &n.i0, 0.2f, 1, 64); ch |= ImGui::DragInt("Seed", &n.i2, 0.2f, 0, 999); break;
    case N_CHECKER: ch |= ImGui::DragInt("Tiles", &n.i0, 0.2f, 1, 64); col("##a", n.c0); ImGui::SameLine(); col("##b", n.c1); break;
    case N_COLOR: col("##a", n.c0); break;
    case N_RAMP:
      ch |= ImGui::DragFloat("Start", &n.f0, 0.005f, 0.f, 1.f); ch |= ImGui::DragFloat("End", &n.f1, 0.005f, 0.f, 1.f);
      col("##a", n.c0); ImGui::SameLine(); col("##b", n.c1); break;
    case N_MIX:
      ch |= ImGui::Combo("##m", &n.i0, "Mix\0Multiply\0Add\0Screen\0"); ch |= ImGui::DragFloat("Fac", &n.f0, 0.005f, 0.f, 1.f);
      col("##a", n.c0); ImGui::SameLine(); col("##b", n.c1); break;
    case N_INVERT: col("##a", n.c0); break;
    case N_OUTPUT:
      col("##a", n.c0); ImGui::SameLine(); ImGui::TextUnformatted("Albedo");
      ch |= ImGui::DragFloat("Height", &n.f0, 0.005f, 0.f, 1.f); ch |= ImGui::DragFloat("Rough", &n.f1, 0.005f, 0.f, 1.f); break;
  }
  ImGui::PopItemWidth();
  if (ch) dirty = true;
}

static ImVec2 popupPos;
static void nodeEditorUI(bool& open) {
  ImGui::SetNextWindowBgAlpha(0.92f);
  ImGui::SetNextWindowPos(ImVec2(10, 380), ImGuiCond_FirstUseEver); ImGui::SetNextWindowSize(ImVec2(820, 330), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Node Editor  (klik kanan = tambah node, Delete = hapus)", &open)) { ImGui::End(); return; }
  ImNodes::BeginNodeEditor();
  for (auto& n : graph.nodes) {
    if (!n.placed) {
      if (n.screenSpace) ImNodes::SetNodeScreenSpacePos(n.id, ImVec2(n.pos[0], n.pos[1]));
      else ImNodes::SetNodeGridSpacePos(n.id, ImVec2(n.pos[0], n.pos[1]));
      n.placed = true;
    }
    ImNodes::BeginNode(n.id);
    ImGui::PushID(n.id);
    ImNodes::BeginNodeTitleBar(); ImGui::TextUnformatted(nodeName(n.type)); ImNodes::EndNodeTitleBar();
    for (int k = 0; k < nodeInputs(n.type); k++) {
      ImNodes::BeginInputAttribute(pinIn(n.id, k)); ImGui::TextUnformatted(inputName(n.type, k)); ImNodes::EndInputAttribute();
    }
    nodeParams(n);
    if (n.type != N_OUTPUT) { ImNodes::BeginOutputAttribute(pinOut(n.id)); ImGui::TextUnformatted("Out"); ImNodes::EndOutputAttribute(); }
    ImGui::PopID();
    ImNodes::EndNode();
  }
  for (auto& l : graph.links) ImNodes::Link(l.id, l.from, l.to);
  ImNodes::EndNodeEditor();

  if (ImNodes::IsEditorHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { ImGui::OpenPopup("addnode"); popupPos = ImGui::GetMousePos(); }
  if (ImGui::BeginPopup("addnode")) {
    for (int t = 0; t < N_OUTPUT; t++)
      if (ImGui::MenuItem(nodeName(t))) { graph.add(t, popupPos.x, popupPos.y, true); }
    ImGui::EndPopup();
  }
  int a, b;
  if (ImNodes::IsLinkCreated(&a, &b)) {
    int o = a, i = b; if (o % 10 != 9) std::swap(o, i);
    if (graph.connect(o, i)) dirty = true;
  }
  int lid;
  if (ImNodes::IsLinkDestroyed(&lid)) { graph.removeLink(lid); dirty = true; }
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
    int nl = ImNodes::NumSelectedLinks();
    if (nl > 0) { std::vector<int> ids(nl); ImNodes::GetSelectedLinks(ids.data()); for (int id : ids) graph.removeLink(id); ImNodes::ClearLinkSelection(); dirty = true; }
    int nn = ImNodes::NumSelectedNodes();
    if (nn > 0) { std::vector<int> ids(nn); ImNodes::GetSelectedNodes(ids.data()); for (int id : ids) graph.removeNode(id); ImNodes::ClearNodeSelection(); dirty = true; }
  }
  ImGui::End();
}

static void setLayerRes(int r) {
  PRES = r;
  paintPx.assign((size_t)r * r * 4, 0); decalPx.assign((size_t)r * r * 2, 0.f);
  decalUndo.clear(); decalUsed = false; lastValid = false;
  pdy0 = PRES; pdy1 = -1; ddy0 = PRES; ddy1 = -1;
  uploadTex(texPaint, PRES, PRES, paintPx.data(), false); uploadDecalFull();
  status = "Resolusi layer paint/decal = " + std::to_string(r) + " (paint & decal di-reset)";
}
// ===================== import & UI =====================
static int viewMode = 0; static bool wire = false, showNodes = true; static char exportDir[512] = "export";
static std::string lowName = "Sphere", highName, impNormalName, impAlbedoName;
static bool dropOpen = false; static std::string dropPath;
static const char* FILT_OBJ = "Model OBJ\0*.obj\0Semua file\0*.*\0";
static const char* FILT_IMG = "Gambar\0*.png;*.jpg;*.jpeg;*.tga;*.bmp\0Semua file\0*.*\0";

static std::string fileNameOf(const std::string& p) { return std::filesystem::path(p).filename().string(); }
static std::string extOf(const std::string& p) {
  std::string e = std::filesystem::path(p).extension().string();
  for (auto& c : e) c = (char)std::tolower((unsigned char)c);
  return e;
}
static bool browse(const char* filter, std::string& out) {
  if (!fileDialogSupported()) { status = "Dialog file hanya ada di Windows. Pakai drag & drop."; return false; }
  return pickOpenFile(filter, out);
}
static void resetForNewModel() {
  std::fill(paintPx.begin(), paintPx.end(), 0); pdy0 = 0; pdy1 = PRES - 1; lastValid = false;
  clearDecals();
  bakedNormal.clear(); bakedAO.clear(); useAO = false;
  uint8_t w[4] = {255, 255, 255, 255}; uploadTex(texAO, 1, 1, w, false);
  if (normalSrc == 1) normalSrc = 0;
  applyNormalTex();
}
static void useModel(Mesh&& m, const std::string& name) { setLowMesh(std::move(m)); lowName = name; resetForNewModel(); }
static void loadLow(const std::string& p) {
  Mesh m; std::string err;
  if (!loadOBJ(p, m, err)) { status = err; return; }
  bool uv = m.hasUV; size_t t = m.tris(); useModel(std::move(m), fileNameOf(p));
  status = "Model dimuat: " + std::to_string(t) + " tris" + (uv ? "" : " (TANPA UV, tidak bisa paint/bake)");
}
static void loadHigh(const std::string& p) {
  Mesh m; std::string err;
  if (!loadOBJ(p, m, err)) { status = err; return; }
  highMesh = std::move(m); highBvh.build(highMesh); hasHigh = true; highName = fileNameOf(p);
  status = "High-poly dimuat: " + std::to_string(highMesh.tris()) + " tris";
}
static bool loadImg(const std::string& path, ImgTex& t, std::string& err) {
  int w, h, c; stbi_set_flip_vertically_on_load(1);
  unsigned char* p = stbi_load(path.c_str(), &w, &h, &c, 4); stbi_set_flip_vertically_on_load(0);
  if (!p) { err = "Gagal membuka gambar: " + path; return false; }
  t.w = w; t.h = h; t.px.assign(p, p + (size_t)w * h * 4); stbi_image_free(p);
  for (size_t i = 0; i < (size_t)w * h; i++) t.px[i * 4 + 3] = 255;
  return true;
}
static void flipGreen(ImgTex& t) { for (size_t i = 0; i < (size_t)t.w * t.h; i++) t.px[i * 4 + 1] = 255 - t.px[i * 4 + 1]; }
static void importNormal(const std::string& p) {
  std::string err; ImgTex t;
  if (!loadImg(p, t, err)) { status = err; return; }
  if (flipY) flipGreen(t);
  impNormal = std::move(t); impNormalName = fileNameOf(p); normalSrc = 2; applyNormalTex();
  status = "Normal map dipakai: " + impNormalName + (flipY ? " (flip Y)" : "");
}
static void importAlbedo(const std::string& p) {
  std::string err; ImgTex t;
  if (!loadImg(p, t, err)) { status = err; return; }
  impAlbedo = std::move(t); impAlbedoName = fileNameOf(p); albedoSrc = 1; applyAlbedoTex();
  status = "Albedo dipakai: " + impAlbedoName;
}
static void importDecal(const std::string& p, bool isNormal) {
  std::string err;
  if (loadDecalPNG(p.c_str(), isNormal, decalFlipY, err)) { decalSel = (int)decals.size() - 1; status = "Decal dimuat: " + decals.back().name; }
  else status = err;
}

// popup setelah drag & drop: tanya file ini mau dipakai sebagai apa
static void dropPopupUI() {
  if (dropOpen) { ImGui::OpenPopup("Import file"); dropOpen = false; }
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal("Import file", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
  ImGui::Text("%s", fileNameOf(dropPath).c_str()); ImGui::Separator(); ImGui::TextUnformatted("Pakai sebagai:");
  std::string ext = extOf(dropPath); const ImVec2 bs(280, 0); bool close = false;
  if (ext == ".obj") {
    if (ImGui::Button("Model utama (low-poly)", bs)) { loadLow(dropPath); close = true; }
    if (ImGui::Button("High-poly (untuk bake)", bs)) { loadHigh(dropPath); close = true; }
  } else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp") {
    if (ImGui::Button("Normal map utama", bs)) { importNormal(dropPath); close = true; }
    if (ImGui::Button("Albedo / Base color", bs)) { importAlbedo(dropPath); close = true; }
    if (ImGui::Button("Decal (normal map)", bs)) { importDecal(dropPath, true); close = true; }
    if (ImGui::Button("Decal (height / grayscale)", bs)) { importDecal(dropPath, false); close = true; }
  } else ImGui::TextUnformatted("Format tidak didukung (OBJ, PNG, JPG, TGA, BMP)");
  if (ImGui::Button("Batal", bs)) close = true;
  if (close) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

static void toolsUI() {
  ImGui::SetNextWindowPos(ImVec2(940, 10), ImGuiCond_FirstUseEver); ImGui::SetNextWindowSize(ImVec2(330, 700), ImGuiCond_FirstUseEver);
  ImGui::Begin("LiteTex");
  int newTool = 0; std::string p;
  ImGui::SetNextItemWidth(110); ImGui::Combo("##view", &viewMode, "Lit\0Albedo\0Normal\0AO\0");
  ImGui::SameLine(); ImGui::Checkbox("Wire", &wire); ImGui::SameLine(); ImGui::Checkbox("Node Editor", &showNodes);
  ImGui::TextDisabled("Kamera: klik kanan putar | tengah geser | scroll zoom");
  if (ImGui::BeginTabBar("tabs")) {
    // ---------------- Import ----------------
    if (ImGui::BeginTabItem("Import")) {
      ImGui::SeparatorText("Model");
      if (ImGui::Button("Sphere")) useModel(makeSphere(), "Sphere"); ImGui::SameLine();
      if (ImGui::Button("Cube")) useModel(makeCube(), "Cube"); ImGui::SameLine();
      if (ImGui::Button("Plane")) useModel(makePlane(), "Plane");
      if (ImGui::Button("Buka model OBJ...", ImVec2(-1, 0)) && browse(FILT_OBJ, p)) loadLow(p);
      ImGui::Text("Aktif: %s (%d tris)%s", lowName.c_str(), (int)lowMesh.tris(), lowMesh.hasUV ? "" : "  [tanpa UV]");
      ImGui::SeparatorText("Texture");
      if (ImGui::Checkbox("Normal map DirectX (flip Y)", &flipY) && impNormal.ok()) { flipGreen(impNormal); applyNormalTex(); }
      if (ImGui::Button("Buka Normal Map...", ImVec2(-1, 0)) && browse(FILT_IMG, p)) importNormal(p);
      if (impNormal.ok()) {
        ImGui::Text("Normal: %s", impNormalName.c_str()); ImGui::SameLine();
        if (ImGui::SmallButton("Hapus##n")) { impNormal = ImgTex(); if (normalSrc == 2) normalSrc = 0; applyNormalTex(); }
      }
      if (ImGui::Button("Buka Albedo / Base Color...", ImVec2(-1, 0)) && browse(FILT_IMG, p)) importAlbedo(p);
      if (impAlbedo.ok()) {
        ImGui::Text("Albedo: %s", impAlbedoName.c_str()); ImGui::SameLine();
        if (ImGui::SmallButton("Hapus##a")) { impAlbedo = ImgTex(); if (albedoSrc == 1) albedoSrc = 0; applyAlbedoTex(); }
      }
      ImGui::Spacing(); ImGui::TextWrapped("Tips: drag & drop file OBJ / PNG / JPG langsung ke jendela, nanti ditanya mau dipakai sebagai apa.");
      ImGui::TextDisabled("Ganti model = paint, decal & bake di-reset.");
      ImGui::EndTabItem();
    }
    // ---------------- Texture ----------------
    if (ImGui::BeginTabItem("Texture")) {
      ImGui::TextWrapped("Atur texture lewat Node Editor (klik kanan = tambah node).");
      if (ImGui::Combo("Resolusi", &texResIdx, "256\0" "512\0" "1024\0" "2048\0")) dirty = true;
      if (ImGui::SliderFloat("Normal strength", &normStrength, 0.f, 30.f)) dirty = true;
      ImGui::SeparatorText("Kualitas layer Paint & Decal");
      if (ImGui::Combo("Resolusi layer", &layerResIdx, "1024\0" "2048\0" "4096\0")) setLayerRes(1024 << layerResIdx);
      ImGui::TextDisabled("Makin tinggi makin tajam saat zoom dekat (reset paint/decal).");
      ImGui::SeparatorText("Sumber map di model");
      if (ImGui::Combo("Normal", &normalSrc, "Dari Node\0Hasil Bake\0File Import\0")) applyNormalTex();
      if (ImGui::Combo("Albedo", &albedoSrc, "Dari Node\0File Import\0")) applyAlbedoTex();
      ImGui::TextDisabled("Kalau sumber belum ada, otomatis pakai Node.");
      ImGui::EndTabItem();
    }
    // ---------------- Paint ----------------
    if (ImGui::BeginTabItem("Paint")) {
      newTool = 1;
      ImGui::TextWrapped("Klik kiri di model untuk melukis.");
      ImGui::Checkbox("Eraser", &eraser);
      ImGui::ColorEdit3("Warna", glm::value_ptr(brushCol), ImGuiColorEditFlags_NoInputs);
      ImGui::SliderFloat("Ukuran", &brushSize, 2.f, 120.f); ImGui::SliderFloat("Hardness", &brushHard, 0.f, 0.99f); ImGui::SliderFloat("Opacity", &brushOpac, 0.05f, 1.f);
      if (ImGui::Button("Hapus semua paint")) { std::fill(paintPx.begin(), paintPx.end(), 0); pdy0 = 0; pdy1 = PRES - 1; }
      ImGui::EndTabItem();
    }
    // ---------------- Decal ----------------
    if (ImGui::BeginTabItem("Decal")) {
      newTool = 2;
      ImGui::TextWrapped("Pilih decal, lalu klik di model. Q/E putar, Ctrl+Z undo.");
      for (int i = 0; i < (int)decals.size(); i++) {
        if (i % 4) ImGui::SameLine();
        ImGui::PushID(i); bool sel = i == decalSel;
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.9f, 0.6f, 0.1f, 1));
        if (ImGui::ImageButton("##d", (ImTextureID)(intptr_t)decals[i].thumb, ImVec2(56, 56))) decalSel = i;
        if (sel) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", decals[i].name.c_str());
        ImGui::PopID();
      }
      ImGui::SliderFloat("Ukuran", &decalSize, 0.005f, 0.4f, "%.3f"); ImGui::SliderFloat("Rotasi", &decalRot, -180.f, 180.f);
      ImGui::SliderFloat("Kekuatan", &decalStrength, 0.f, 3.f); ImGui::SliderFloat("Opacity##d", &decalOpac, 0.05f, 1.f);
      ImGui::Checkbox("Inverse", &decalInvert); ImGui::SameLine(); ImGui::Checkbox("Rotasi acak", &decalRandom);
      if (ImGui::Button("Undo")) undoDecal(); ImGui::SameLine(); if (ImGui::Button("Hapus semua decal")) clearDecals();
      ImGui::SeparatorText("Decal sendiri");
      ImGui::Checkbox("DirectX (flip Y)", &decalFlipY); ImGui::SameLine(); ImGui::Checkbox("Auto-koreksi tilt", &decalFixBias);
      ImGui::RadioButton("Normal map", &decalIsNormal, 1); ImGui::SameLine(); ImGui::RadioButton("Height/grayscale", &decalIsNormal, 0);
      if (ImGui::Button("Buka PNG decal...", ImVec2(-1, 0)) && browse(FILT_IMG, p)) importDecal(p, decalIsNormal != 0);
      ImGui::EndTabItem();
    }
    // ---------------- Bake ----------------
    if (ImGui::BeginTabItem("Bake")) {
      ImGui::TextWrapped("1) Model aktif = low-poly (harus punya UV)  2) Buka high-poly  3) Bake.");
      if (ImGui::Button("Buka High-poly OBJ...", ImVec2(-1, 0)) && browse(FILT_OBJ, p)) loadHigh(p);
      if (hasHigh) {
        ImGui::Text("High: %s (%d tris)", highName.c_str(), (int)highMesh.tris()); ImGui::SameLine();
        if (ImGui::SmallButton("Hapus##h")) { hasHigh = false; highMesh = Mesh(); highName.clear(); }
      } else ImGui::TextDisabled("High-poly belum dimuat");
      ImGui::Combo("Resolusi bake", &bakeResIdx, "256\0" "512\0" "1024\0" "2048\0");
      ImGui::SeparatorText("Normal map");
      ImGui::SliderFloat("Cage", &cageRel, 0.005f, 0.3f);
      if (ImGui::Button("Bake Normal Map", ImVec2(-1, 0))) bakeNormal();
      ImGui::SeparatorText("Ambient Occlusion");
      ImGui::SliderFloat("Jarak", &aoDistRel, 0.01f, 0.6f); ImGui::SliderInt("Sample", &aoSamples, 4, 64);
      if (ImGui::Button("Bake AO", ImVec2(-1, 0))) bakeAO();
      ImGui::Checkbox("Pakai AO di model", &useAO);
      ImGui::EndTabItem();
    }
    // ---------------- Export ----------------
    if (ImGui::BeginTabItem("Export")) {
      ImGui::TextWrapped("Simpan albedo (+paint), normal (+bake/decal), roughness, height, ao sebagai PNG.");
      ImGui::InputText("Folder", exportDir, sizeof(exportDir));
      if (ImGui::Button("Pilih folder...", ImVec2(-1, 0))) {
        std::string d; if (!fileDialogSupported()) status = "Dialog folder hanya ada di Windows. Ketik nama folder."; else if (pickFolder(d)) std::snprintf(exportDir, sizeof(exportDir), "%s", d.c_str());
      }
      if (ImGui::Button("EXPORT SEMUA", ImVec2(-1, 36))) exportAll(exportDir);
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  tool = newTool;
  ImGui::Separator(); ImGui::TextWrapped("%s", status.c_str());
  ImGui::TextDisabled("%.0f FPS", ImGui::GetIO().Framerate);
  ImGui::End();
}

// ===================== main =====================
int main(int, char**) {
  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24); SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_Window* win = SDL_CreateWindow("LiteTex", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
  if (!win) { fprintf(stderr, "Window: %s\n", SDL_GetError()); return 1; }
  SDL_GLContext ctx = SDL_GL_CreateContext(win);
  if (!ctx) { fprintf(stderr, "GL context: %s\n(Butuh OpenGL 3.3, update driver Intel)\n", SDL_GetError()); return 1; }
  SDL_GL_SetSwapInterval(1);
  glewExperimental = GL_TRUE;
  if (glewInit() != GLEW_OK) { fprintf(stderr, "GLEW gagal\n"); return 1; }

  IMGUI_CHECKVERSION(); ImGui::CreateContext(); ImNodes::CreateContext();
  ImGui::StyleColorsDark(); ImNodes::StyleColorsDark();
  ImGui_ImplSDL2_InitForOpenGL(win, ctx); ImGui_ImplOpenGL3_Init("#version 330");

  GLuint vs = compile(GL_VERTEX_SHADER, VS), fs = compile(GL_FRAGMENT_SHADER, FS);
  prog = glCreateProgram(); glAttachShader(prog, vs); glAttachShader(prog, fs); glLinkProgram(prog);
  glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo); glGenBuffers(1, &ebo);
  texAlbedo = makeTex(); texNormal = makeTex(); texRough = makeTex(); texPaint = makeTex(); texAO = makeTex();
  uploadTex(texPaint, PRES, PRES, paintPx.data(), false);
  uint8_t white[4] = {255, 255, 255, 255}; uploadTex(texAO, 1, 1, white, false);
  initDecals();
  setLowMesh(makeSphere());

  // graph default: Noise -> Ramp -> Albedo, Noise -> Height
  graph.add(N_NOISE, 20, 40).i0 = 6;
  graph.add(N_RAMP, 230, 40);
  graph.add(N_OUTPUT, 450, 40);
  graph.connect(pinOut(1), pinIn(2, 0)); graph.connect(pinOut(2), pinIn(3, 0)); graph.connect(pinOut(1), pinIn(3, 1));

  bool running = true;
  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) { ImGui_ImplSDL2_ProcessEvent(&e); if (e.type == SDL_QUIT) running = false;
      if (e.type == SDL_DROPFILE && e.drop.file) { dropPath = e.drop.file; SDL_free(e.drop.file); dropOpen = true; } }
    ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplSDL2_NewFrame(); ImGui::NewFrame();
    ImGuiIO& io = ImGui::GetIO();

    toolsUI(); dropPopupUI();
    if (showNodes) nodeEditorUI(showNodes);

    if (!io.WantCaptureMouse) {
      if (io.MouseDown[1]) { yaw -= io.MouseDelta.x * 0.01f; pitch = glm::clamp(pitch + io.MouseDelta.y * 0.01f, -1.5f, 1.5f); }
      if (io.MouseDown[2]) {
        glm::vec3 right(gView[0][0], gView[1][0], gView[2][0]), up(gView[0][1], gView[1][1], gView[2][1]);
        target += (-right * io.MouseDelta.x + up * io.MouseDelta.y) * dist * 0.002f;
      }
      dist = std::max(0.05f, dist * std::pow(0.9f, io.MouseWheel));
    }
    {
      bool vh = !io.WantCaptureMouse; Pick pk;
      if (vh && ((tool == 1 && io.MouseDown[0]) || tool == 2)) pk = pickSurface(io.MousePos, io.DisplaySize);
      if (tool == 1 && vh && io.MouseDown[0] && pk.ok) paintAt(pk.uv); else lastValid = false;
      if (tool == 2 && vh) {
        if (pk.ok) {
          glm::vec3 eyeN = target + dist * glm::vec3(std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw));
          float rad = (lowDiag * decalSize * 0.5f) / (glm::length(pk.pos - eyeN) * std::tan(glm::radians(22.5f))) * io.DisplaySize.y * 0.5f;
          ImGui::GetForegroundDrawList()->AddCircle(io.MousePos, std::max(3.f, rad), IM_COL32(255, 190, 40, 255), 48, 1.5f);
          if (ImGui::IsMouseClicked(0)) placeDecal(pk);
        }
        if (!io.WantTextInput) {
          if (ImGui::IsKeyPressed(ImGuiKey_Q)) { decalRot -= 15.f; if (decalRot < -180.f) decalRot += 360.f; }
          if (ImGui::IsKeyPressed(ImGuiKey_E)) { decalRot += 15.f; if (decalRot > 180.f) decalRot -= 360.f; }
        }
      }
      if (io.KeyCtrl && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Z, false)) undoDecal();
    }

    bool dragging = io.MouseDown[0];
    int full = resOpts[texResIdx];
    if (dirty) { regenMaps(dragging ? std::min(full, 256) : full); dirty = false; needFull = dragging && full > 256; }
    else if (needFull && !dragging) { regenMaps(full); needFull = false; }

    ImGui::Render();
    int w, h; SDL_GL_GetDrawableSize(win, &w, &h);
    glViewport(0, 0, w, h); glClearColor(0.12f, 0.12f, 0.14f, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    glm::vec3 eye = target + dist * glm::vec3(std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw));
    gView = glm::lookAt(eye, target, glm::vec3(0, 1, 0));
    gProj = glm::perspective(glm::radians(45.f), (float)w / std::max(1, h), dist * 0.01f, dist * 100.f);
    flushPaint(); flushDecal();
    glUseProgram(prog);
    glm::mat4 mvp = gProj * gView;
    glUniformMatrix4fv(glGetUniformLocation(prog, "uMVP"), 1, GL_FALSE, glm::value_ptr(mvp));
    glUniform3fv(glGetUniformLocation(prog, "uEye"), 1, glm::value_ptr(eye));
    glUniform1i(glGetUniformLocation(prog, "uMode"), viewMode);
    glUniform1f(glGetUniformLocation(prog, "uNStr"), 1.f);
    glUniform1i(glGetUniformLocation(prog, "uUseAO"), useAO && !bakedAO.empty() ? 1 : 0);
    GLuint texs[6] = {texAlbedo, texNormal, texRough, texPaint, texAO, texDecal};
    const char* names[6] = {"uAlbedo", "uNormal", "uRough", "uPaint", "uAO", "uDecal"};
    for (int i = 0; i < 6; i++) { glActiveTexture(GL_TEXTURE0 + i); glBindTexture(GL_TEXTURE_2D, texs[i]); glUniform1i(glGetUniformLocation(prog, names[i]), i); }
    glPolygonMode(GL_FRONT_AND_BACK, wire ? GL_LINE : GL_FILL);
    glBindVertexArray(vao); glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, nullptr);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glActiveTexture(GL_TEXTURE0);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(win);
  }
  ImNodes::DestroyContext(); ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplSDL2_Shutdown(); ImGui::DestroyContext();
  SDL_GL_DeleteContext(ctx); SDL_DestroyWindow(win); SDL_Quit();
  return 0;
}
