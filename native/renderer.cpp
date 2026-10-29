#include "renderer.hpp"

#include <d3d11.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace flock {
namespace {

template<class T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& other) noexcept : value_(other.detach()) {}
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) reset(other.detach());
        return *this;
    }

    T* get() const noexcept { return value_; }
    T* operator->() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }
    T** put() noexcept {
        reset();
        return &value_;
    }
    T* detach() noexcept {
        T* value = value_;
        value_ = nullptr;
        return value;
    }
    void reset(T* value = nullptr) noexcept {
        if (value_) value_->Release();
        value_ = value;
    }

private:
    T* value_ = nullptr;
};

struct MappedResource {
    ID3D11DeviceContext* context;
    ID3D11Resource* resource;
    ~MappedResource() { if (context && resource) context->Unmap(resource, 0); }
};

constexpr std::size_t MaxTrailSegments =
    (static_cast<std::size_t>(MaxBirds) + static_cast<std::size_t>(MaxGhosts)) *
    HistoryCapacity;
constexpr UINT SwapBufferCount = 2;
constexpr float Pi = 3.14159265358979323846f;

float bounded(float value, float low, float high, float fallback) noexcept {
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

std::string narrow(const wchar_t* text);

std::string hresultMessage(const char* operation, HRESULT result) {
    wchar_t systemText[512]{};
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(result), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        systemText, static_cast<DWORD>(std::size(systemText)), nullptr);
    std::wstring detail;
    if (length != 0) {
        detail.assign(systemText, length);
        while (!detail.empty() && (detail.back() == L'\r' || detail.back() == L'\n' || detail.back() == L' '))
            detail.pop_back();
    }
    std::ostringstream message;
    message << operation << " failed (HRESULT 0x" << std::hex << std::uppercase
            << static_cast<unsigned long>(result) << ')';
    if (!detail.empty()) message << ": " << narrow(detail.c_str());
    return message.str();
}

std::string narrow(const wchar_t* text) {
    if (!text || !*text) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1) return {};
    std::string result(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), count, nullptr, nullptr) != count)
        return {};
    result.pop_back();
    return result;
}

bool finiteColor(Color color) noexcept {
    return std::isfinite(color.r) && std::isfinite(color.g) && std::isfinite(color.b);
}

Color normalizedColor(Color color, Color fallback = {}) noexcept {
    if (!finiteColor(color)) color = fallback;
    return {
        std::clamp(color.r, 0.0f, 1.0f),
        std::clamp(color.g, 0.0f, 1.0f),
        std::clamp(color.b, 0.0f, 1.0f)
    };
}

struct BirdVertex {
    float x, y, luminance, wing;
};

// A small, faceted bird silhouette: forked tail, swept wings and a pointed body.
constexpr std::array<BirdVertex, 30> BirdMesh{{
    {-0.34f, -0.035f, 0.40f, 0.0f}, {-0.88f, -0.22f, 0.28f, 0.0f}, {-0.58f, -0.01f, 0.36f, 0.0f},
    {-0.34f,  0.035f, 0.40f, 0.0f}, {-0.58f,  0.01f, 0.36f, 0.0f}, {-0.88f,  0.22f, 0.28f, 0.0f},
    { 0.12f, -0.045f, 0.66f, 1.0f}, {-0.38f, -0.96f, 0.59f, 1.0f}, {-0.62f, -0.52f, 0.56f, 1.0f},
    { 0.12f, -0.045f, 0.66f, 1.0f}, {-0.62f, -0.52f, 0.56f, 1.0f}, {-0.32f, -0.16f, 0.63f, 1.0f},
    { 0.12f,  0.045f, 0.66f, 1.0f}, {-0.62f,  0.52f, 0.56f, 1.0f}, {-0.38f,  0.96f, 0.59f, 1.0f},
    { 0.12f,  0.045f, 0.66f, 1.0f}, {-0.32f,  0.16f, 0.63f, 1.0f}, {-0.62f,  0.52f, 0.56f, 1.0f},
    {-0.42f, -0.06f, 0.86f, 0.0f}, { 0.38f, -0.025f, 0.98f, 0.0f}, {-0.15f, 0.0f, 0.91f, 0.0f},
    {-0.15f,  0.0f, 0.91f, 0.0f}, { 0.38f, -0.025f, 0.98f, 0.0f}, { 0.64f, 0.0f, 1.0f, 0.0f},
    {-0.42f,  0.06f, 0.86f, 0.0f}, {-0.15f,  0.0f, 0.91f, 0.0f}, { 0.38f, 0.025f, 0.98f, 0.0f},
    {-0.15f,  0.0f, 0.91f, 0.0f}, { 0.38f,  0.025f, 0.98f, 0.0f}, { 0.64f, 0.0f, 1.0f, 0.0f}
}};

struct BirdInstance {
    float centerX, centerY;
    float velocityX, velocityY;
    float phase;
    float deathProgress;
    float alpha;
    float colorR, colorG, colorB;
};
static_assert(sizeof(BirdInstance) == sizeof(float) * 10);

struct TrailInstance {
    float startX, startY, startTime;
    float startR, startG, startB;
    float endX, endY, endTime;
    float endR, endG, endB;
};
static_assert(sizeof(TrailInstance) == sizeof(float) * 12);
constexpr std::size_t BirdInstanceBufferBytes =
    sizeof(BirdInstance) * static_cast<std::size_t>(MaxBirds);
static_assert(BirdInstanceBufferBytes <=
              static_cast<std::size_t>(std::numeric_limits<UINT>::max()));
static_assert(MaxTrailSegments <=
              std::numeric_limits<std::size_t>::max() / sizeof(TrailInstance));
constexpr std::size_t TrailInstanceBufferBytes =
    MaxTrailSegments * sizeof(TrailInstance);
static_assert(TrailInstanceBufferBytes <=
              static_cast<std::size_t>(std::numeric_limits<UINT>::max()));

struct BirdVertexConstants {
    float viewportWidth, viewportHeight;
    float size, time;
};
struct BirdPixelConstants {
    float colorR, colorG, colorB, brightness;
    float contrast;
    std::uint32_t randomColor;
    float padding[2];
};
struct TrailVertexConstants {
    float viewportWidth, viewportHeight;
    float now, duration;
    float width, padding[3];
};
struct TrailPixelConstants {
    float colorR, colorG, colorB, brightness;
    float contrast;
    std::uint32_t randomColor;
    float padding[2];
};
static_assert(sizeof(BirdVertexConstants) == 16);
static_assert(sizeof(BirdPixelConstants) == 32);
static_assert(sizeof(TrailVertexConstants) == 32);
static_assert(sizeof(TrailPixelConstants) == 32);

constexpr char BirdVertexShader[] = R"hlsl(
cbuffer BirdVertexConstants : register(b0) {
    float2 uViewport;
    float uSize;
    float uTime;
};
struct VSInput {
    float2 local : POSITION;
    float luminance : TEXCOORD0;
    float wing : TEXCOORD1;
    float2 center : INSTANCECENTER;
    float2 velocity : INSTANCEVELOCITY;
    float phase : INSTANCEPHASE;
    float deathProgress : INSTANCEDEATH;
    float alpha : INSTANCEALPHA;
    float3 birdColor : BIRDCOLOR;
};
struct VSOutput {
    float4 position : SV_POSITION;
    float luminance : TEXCOORD0;
    float depth : TEXCOORD1;
    float deathProgress : TEXCOORD2;
    float alpha : TEXCOORD3;
    float3 birdColor : COLOR0;
};
float smoothValue(float edge0, float edge1, float value) {
    float t = saturate((value - edge0) / max(edge1 - edge0, 0.00001));
    return t * t * (3.0 - 2.0 * t);
}
VSOutput main(VSInput input) {
    float speed = length(input.velocity);
    float2 direction = speed > 0.0001 ? input.velocity / speed : float2(1.0, 0.0);
    float beat = uTime * 7.0 + input.phase;
    float flap = sin(beat);
    float dying = input.deathProgress >= 0.0 ? 1.0 : 0.0;
    float progress = saturate(input.deathProgress);
    float unfold = 0.64 + 0.52 * smoothValue(0.0, 0.18, progress);
    float shrink = 1.0 - smoothValue(0.32, 1.0, progress);
    float2 local = input.local;
    local.y *= lerp(1.0, 0.84 + 0.12 * flap, input.wing);
    local.x += input.wing * abs(input.local.y) * 0.055 * cos(beat);
    local.y *= lerp(1.0, 1.28, dying * smoothValue(0.0, 0.18, progress) * input.wing);
    float depth = 0.91 + 0.12 * (0.5 + 0.5 * sin(input.phase * 1.73 + 0.8));
    local *= uSize * depth * lerp(1.0, unfold * shrink, dying);
    float2 rotated = float2(
        local.x * direction.x - local.y * direction.y,
        local.x * direction.y + local.y * direction.x);
    float2 birdPosition = input.center + rotated;
    float2 clip = float2(birdPosition.x / max(uViewport.x, 1.0) * 2.0 - 1.0,
                         1.0 - birdPosition.y / max(uViewport.y, 1.0) * 2.0);
    VSOutput output;
    output.position = float4(clip, 0.0, 1.0);
    output.luminance = input.luminance;
    output.depth = depth;
    output.deathProgress = input.deathProgress;
    output.alpha = input.alpha;
    output.birdColor = input.birdColor;
    return output;
}
)hlsl";

constexpr char BirdPixelShader[] = R"hlsl(
cbuffer BirdPixelConstants : register(b1) {
    float3 uColor;
    float uBrightness;
    float uContrast;
    uint uRandomColor;
    float2 uPadding;
};
struct PSInput {
    float4 position : SV_POSITION;
    float luminance : TEXCOORD0;
    float depth : TEXCOORD1;
    float deathProgress : TEXCOORD2;
    float alpha : TEXCOORD3;
    float3 birdColor : COLOR0;
};
float smoothValue(float edge0, float edge1, float value) {
    float t = saturate((value - edge0) / max(edge1 - edge0, 0.00001));
    return t * t * (3.0 - 2.0 * t);
}
float4 main(PSInput input) : SV_TARGET {
    float3 base = uRandomColor != 0 ? saturate(input.birdColor) : uColor;
    float tone = (input.luminance - 0.64) * 1.5;
    float3 feather = base * clamp(1.0 + tone * uContrast, 0.38, 1.0) * input.depth;
    float dying = input.deathProgress >= 0.0 ? 1.0 : 0.0;
    float progress = saturate(input.deathProgress);
    float flash = 1.0 - smoothValue(0.02, 0.32, progress);
    feather = lerp(feather, float3(0.96, 1.0, 1.0), dying * flash * 0.48);
    float fade = 1.0 - smoothValue(0.35, 1.0, progress);
    float alpha = 0.96 * input.alpha * lerp(1.0, fade, dying);
    return float4(feather * uBrightness, alpha);
}
)hlsl";

constexpr char TrailVertexShader[] = R"hlsl(
cbuffer TrailVertexConstants : register(b0) {
    float2 uViewport;
    float uNow;
    float uDuration;
    float uWidth;
    float3 uPadding;
};
struct VSInput {
    float2 corner : POSITION;
    float3 start : TRAILSTART;
    float3 startColor : TRAILCOLOR0;
    float3 end : TRAILEND;
    float3 endColor : TRAILCOLOR1;
};
struct VSOutput {
    float4 position : SV_POSITION;
    float fade : TEXCOORD0;
    float distance : TEXCOORD1;
    float halfWidth : TEXCOORD2;
    float3 color : COLOR0;
};
float smoothValue(float edge0, float edge1, float value) {
    float t = saturate((value - edge0) / max(edge1 - edge0, 0.00001));
    return t * t * (3.0 - 2.0 * t);
}
VSOutput main(VSInput input) {
    float cutoff = uNow - uDuration;
    float deltaTime = input.end.z - input.start.z;
    bool valid = deltaTime > 0.000001 && input.end.z > cutoff && input.start.z < uNow;
    float visibleFrom = max(input.start.z, cutoff);
    float visibleTo = min(input.end.z, uNow);
    float invDelta = deltaTime > 0.000001 ? rcp(deltaTime) : 0.0;
    float fromT = saturate((visibleFrom - input.start.z) * invDelta);
    float toT = saturate((visibleTo - input.start.z) * invDelta);
    float2 from = lerp(input.start.xy, input.end.xy, fromT);
    float2 to = lerp(input.start.xy, input.end.xy, toT);
    float along = input.corner.x;
    float pointTime = lerp(visibleFrom, visibleTo, along);
    float age = saturate((pointTime - cutoff) / max(uDuration, 0.0001));
    float fade = smoothValue(0.0, 0.38, age);
    float2 tangent = to - from;
    float tangentLength = length(tangent);
    float2 normal = tangentLength > 0.0001 ? float2(-tangent.y, tangent.x) / tangentLength : float2(0.0, 1.0);
    float taper = 0.18 + 0.82 * fade;
    float halfWidth = uWidth * taper * 0.5;
    float2 trailPosition = lerp(from, to, along) + normal * (input.corner.y * halfWidth);
    float2 clip = float2(trailPosition.x / max(uViewport.x, 1.0) * 2.0 - 1.0,
                         1.0 - trailPosition.y / max(uViewport.y, 1.0) * 2.0);
    VSOutput output;
    output.position = valid ? float4(clip, 0.0, 1.0) : float4(2.0, 2.0, 2.0, 1.0);
    output.fade = valid ? fade : 0.0;
    output.distance = input.corner.y * halfWidth;
    output.halfWidth = halfWidth;
    output.color = lerp(input.startColor, input.endColor, lerp(fromT, toT, along));
    return output;
}
)hlsl";

constexpr char TrailPixelShader[] = R"hlsl(
cbuffer TrailPixelConstants : register(b1) {
    float3 uColor;
    float uBrightness;
    float uContrast;
    uint uRandomColor;
    float2 uPadding;
};
struct PSInput {
    float4 position : SV_POSITION;
    float fade : TEXCOORD0;
    float distance : TEXCOORD1;
    float halfWidth : TEXCOORD2;
    float3 color : COLOR0;
};
float4 main(PSInput input) : SV_TARGET {
    float footprint = max(fwidth(input.distance), 0.0001);
    float overlap = min(input.halfWidth, input.distance + footprint * 0.5)
                  - max(-input.halfWidth, input.distance - footprint * 0.5);
    float coverage = max(overlap, 0.0) / footprint;
    float alpha = clamp(0.78 * uBrightness * input.fade * coverage, 0.0, 0.92);
    if (alpha <= 0.001) discard;
    float3 base = uRandomColor != 0 ? saturate(input.color) : uColor;
    float tone = lerp(0.25, 1.0, input.fade);
    float shade = clamp(1.0 + (tone - 1.0) * uContrast, 0.4, 1.0);
    return float4(base * uBrightness * shade, alpha);
}
)hlsl";

constexpr char CompositeVertexShader[] = R"hlsl(
struct VSOutput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
VSOutput main(uint vertexId : SV_VertexID) {
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    VSOutput output;
    output.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    output.uv = uv;
    return output;
}
)hlsl";

constexpr char CompositePixelShader[] = R"hlsl(
Texture2D<float4> uScene : register(t0);
SamplerState uSampler : register(s0);
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    return uScene.Sample(uSampler, uv);
}
)hlsl";

HRESULT compileShader(const char* source, const char* name, const char* target,
                      ComPtr<ID3DBlob>& bytecode, std::string& error) {
    ComPtr<ID3DBlob> diagnostics;
    const HRESULT result = D3DCompile(source, std::strlen(source), name, nullptr, nullptr,
        "main", target, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0, bytecode.put(), diagnostics.put());
    if (FAILED(result)) {
        if (diagnostics && diagnostics->GetBufferPointer()) {
            error.assign(static_cast<const char*>(diagnostics->GetBufferPointer()),
                         diagnostics->GetBufferSize());
        } else {
            error = hresultMessage("D3DCompile", result);
        }
    }
    return result;
}

} // namespace

struct Renderer::Impl {
    HWND window = nullptr;
    int width = 0;
    int height = 0;
    int targetWidth = 0;
    int targetHeight = 0;
    float renderScale = 1.0f;
    bool initialized = false;
    bool suspended = false;
    bool hasPresented = false;
    std::string adapterName;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<ID3D11RenderTargetView> backBufferView;
    ComPtr<ID3D11Texture2D> sceneTexture;
    ComPtr<ID3D11RenderTargetView> sceneView;
    ComPtr<ID3D11ShaderResourceView> sceneResource;
    ComPtr<ID3D11Texture2D> captureTexture;
    ComPtr<ID3D11RenderTargetView> captureView;
    int captureWidth = 0;
    int captureHeight = 0;

    ComPtr<ID3D11VertexShader> birdVertexShader;
    ComPtr<ID3D11PixelShader> birdPixelShader;
    ComPtr<ID3D11InputLayout> birdInputLayout;
    ComPtr<ID3D11Buffer> birdMeshBuffer;
    ComPtr<ID3D11Buffer> birdInstanceBuffer;
    ComPtr<ID3D11Buffer> birdVertexConstants;
    ComPtr<ID3D11Buffer> birdPixelConstants;

    ComPtr<ID3D11VertexShader> trailVertexShader;
    ComPtr<ID3D11PixelShader> trailPixelShader;
    ComPtr<ID3D11InputLayout> trailInputLayout;
    ComPtr<ID3D11Buffer> trailCornerBuffer;
    ComPtr<ID3D11Buffer> trailInstanceBuffer;
    ComPtr<ID3D11Buffer> trailVertexConstants;
    ComPtr<ID3D11Buffer> trailPixelConstants;

    ComPtr<ID3D11VertexShader> compositeVertexShader;
    ComPtr<ID3D11PixelShader> compositePixelShader;
    ComPtr<ID3D11SamplerState> linearSampler;
    ComPtr<ID3D11BlendState> alphaBlend;
    ComPtr<ID3D11RasterizerState> rasterizer;

    std::array<BirdInstance, MaxBirds> birdInstances{};
    std::vector<TrailInstance> trailInstances;
    std::size_t trailCount = 0;

    Impl() : trailInstances(MaxTrailSegments) {}

    void releaseDeviceResources() noexcept {
        initialized = false;
        if (context) {
            context->ClearState();
            context->Flush();
        }
        backBufferView.reset();
        captureView.reset();
        captureTexture.reset();
        captureWidth = 0;
        captureHeight = 0;
        sceneResource.reset();
        sceneView.reset();
        sceneTexture.reset();
        compositePixelShader.reset();
        compositeVertexShader.reset();
        linearSampler.reset();
        alphaBlend.reset();
        rasterizer.reset();
        birdPixelConstants.reset();
        birdVertexConstants.reset();
        birdInstanceBuffer.reset();
        birdMeshBuffer.reset();
        birdInputLayout.reset();
        birdPixelShader.reset();
        birdVertexShader.reset();
        trailPixelConstants.reset();
        trailVertexConstants.reset();
        trailInstanceBuffer.reset();
        trailCornerBuffer.reset();
        trailInputLayout.reset();
        trailPixelShader.reset();
        trailVertexShader.reset();
        swapChain.reset();
        context.reset();
        device.reset();
        adapterName.clear();
        targetWidth = 0;
        targetHeight = 0;
        hasPresented = false;
    }

    bool createBuffer(UINT byteWidth, UINT bindFlags, D3D11_USAGE usage,
                      UINT cpuAccess, const void* initialData,
                      ComPtr<ID3D11Buffer>& output, const char* operation,
                      std::string& error) {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = byteWidth;
        desc.Usage = usage;
        desc.BindFlags = bindFlags;
        desc.CPUAccessFlags = cpuAccess;
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = initialData;
        const HRESULT result = device->CreateBuffer(&desc, initialData ? &data : nullptr, output.put());
        if (FAILED(result)) {
            error = hresultMessage(operation, result);
            return false;
        }
        return true;
    }

    bool createBackBufferViews(std::string& error) {
        if (!swapChain || !device) {
            error = "Cannot create swap-chain views before D3D11 initialization.";
            return false;
        }
        ComPtr<ID3D11Texture2D> buffer;
        HRESULT result = swapChain->GetBuffer(0, IID_PPV_ARGS(buffer.put()));
        if (FAILED(result)) {
            error = hresultMessage("IDXGISwapChain::GetBuffer(0)", result);
            return false;
        }
        result = device->CreateRenderTargetView(buffer.get(), nullptr, backBufferView.put());
        if (FAILED(result)) {
            error = hresultMessage("ID3D11Device::CreateRenderTargetView(back buffer 0)", result);
            return false;
        }
        return true;
    }

    bool createSceneTarget(std::string& error) {
        if (!device) {
            error = "Cannot create the render target without a D3D11 device.";
            return false;
        }
        const float scale = bounded(renderScale, 0.5f, 1.0f, 1.0f);
        const int wantedWidth = std::max(1, static_cast<int>(std::lround(std::max(width, 1) * scale)));
        const int wantedHeight = std::max(1, static_cast<int>(std::lround(std::max(height, 1) * scale)));
        if (sceneTexture && wantedWidth == targetWidth && wantedHeight == targetHeight)
            return true;

        if (context) {
            ID3D11RenderTargetView* noTarget = nullptr;
            ID3D11ShaderResourceView* noResource = nullptr;
            context->OMSetRenderTargets(1, &noTarget, nullptr);
            context->PSSetShaderResources(0, 1, &noResource);
        }
        sceneResource.reset();
        sceneView.reset();
        sceneTexture.reset();

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(wantedWidth);
        desc.Height = static_cast<UINT>(wantedHeight);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT result = device->CreateTexture2D(&desc, nullptr, sceneTexture.put());
        if (FAILED(result)) {
            error = hresultMessage("ID3D11Device::CreateTexture2D(scene)", result);
            sceneTexture.reset();
            return false;
        }
        result = device->CreateRenderTargetView(sceneTexture.get(), nullptr, sceneView.put());
        if (FAILED(result)) {
            error = hresultMessage("ID3D11Device::CreateRenderTargetView(scene)", result);
            sceneView.reset();
            sceneTexture.reset();
            return false;
        }
        result = device->CreateShaderResourceView(sceneTexture.get(), nullptr, sceneResource.put());
        if (FAILED(result)) {
            error = hresultMessage("ID3D11Device::CreateShaderResourceView(scene)", result);
            sceneResource.reset();
            sceneView.reset();
            sceneTexture.reset();
            return false;
        }
        targetWidth = wantedWidth;
        targetHeight = wantedHeight;
        return true;
    }

    bool createCaptureTarget(std::string& error) {
        if (!device || width <= 0 || height <= 0) {
            error = "Cannot create a PNG capture target without a live client size and D3D11 device.";
            return false;
        }
        if (captureTexture && captureView && captureWidth == width && captureHeight == height)
            return true;
        if (context) {
            ID3D11RenderTargetView* noTarget = nullptr;
            ID3D11ShaderResourceView* noResource = nullptr;
            context->OMSetRenderTargets(1, &noTarget, nullptr);
            context->PSSetShaderResources(0, 1, &noResource);
        }
        captureView.reset();
        captureTexture.reset();
        captureWidth = 0;
        captureHeight = 0;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        HRESULT result = device->CreateTexture2D(&desc, nullptr, captureTexture.put());
        if (FAILED(result)) {
            error = hresultMessage("ID3D11Device::CreateTexture2D(capture target)", result);
            captureTexture.reset();
            return false;
        }
        result = device->CreateRenderTargetView(captureTexture.get(), nullptr, captureView.put());
        if (FAILED(result)) {
            error = hresultMessage("ID3D11Device::CreateRenderTargetView(capture target)", result);
            captureView.reset();
            captureTexture.reset();
            return false;
        }
        captureWidth = width;
        captureHeight = height;
        return true;
    }

    void compositeTo(ID3D11RenderTargetView* target) {
        context->OMSetRenderTargets(1, &target, nullptr);
        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(width);
        viewport.Height = static_cast<float>(height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        context->RSSetViewports(1, &viewport);
        context->RSSetState(rasterizer.get());
        context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetInputLayout(nullptr);
        context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
        context->VSSetShader(compositeVertexShader.get(), nullptr, 0);
        context->PSSetShader(compositePixelShader.get(), nullptr, 0);
        ID3D11ShaderResourceView* resource = sceneResource.get();
        ID3D11SamplerState* sampler = linearSampler.get();
        context->PSSetShaderResources(0, 1, &resource);
        context->PSSetSamplers(0, 1, &sampler);
        context->Draw(3, 0);
        ID3D11ShaderResourceView* noResource = nullptr;
        context->PSSetShaderResources(0, 1, &noResource);
        context->OMSetRenderTargets(0, nullptr, nullptr);
    }

    bool createStaticResources(std::string& error) {
        ComPtr<ID3DBlob> birdVs;
        ComPtr<ID3DBlob> birdPs;
        ComPtr<ID3DBlob> trailVs;
        ComPtr<ID3DBlob> trailPs;
        ComPtr<ID3DBlob> compositeVs;
        ComPtr<ID3DBlob> compositePs;
        if (FAILED(compileShader(BirdVertexShader, "bird vertex", "vs_4_0", birdVs, error))) return false;
        if (FAILED(compileShader(BirdPixelShader, "bird pixel", "ps_4_0", birdPs, error))) return false;
        if (FAILED(compileShader(TrailVertexShader, "trail vertex", "vs_4_0", trailVs, error))) return false;
        if (FAILED(compileShader(TrailPixelShader, "trail pixel", "ps_4_0", trailPs, error))) return false;
        if (FAILED(compileShader(CompositeVertexShader, "composite vertex", "vs_4_0", compositeVs, error))) return false;
        if (FAILED(compileShader(CompositePixelShader, "composite pixel", "ps_4_0", compositePs, error))) return false;

        HRESULT result = device->CreateVertexShader(birdVs->GetBufferPointer(), birdVs->GetBufferSize(), nullptr, birdVertexShader.put());
        if (FAILED(result)) { error = hresultMessage("CreateVertexShader(bird)", result); return false; }
        result = device->CreatePixelShader(birdPs->GetBufferPointer(), birdPs->GetBufferSize(), nullptr, birdPixelShader.put());
        if (FAILED(result)) { error = hresultMessage("CreatePixelShader(bird)", result); return false; }
        result = device->CreateVertexShader(trailVs->GetBufferPointer(), trailVs->GetBufferSize(), nullptr, trailVertexShader.put());
        if (FAILED(result)) { error = hresultMessage("CreateVertexShader(trail)", result); return false; }
        result = device->CreatePixelShader(trailPs->GetBufferPointer(), trailPs->GetBufferSize(), nullptr, trailPixelShader.put());
        if (FAILED(result)) { error = hresultMessage("CreatePixelShader(trail)", result); return false; }
        result = device->CreateVertexShader(compositeVs->GetBufferPointer(), compositeVs->GetBufferSize(), nullptr, compositeVertexShader.put());
        if (FAILED(result)) { error = hresultMessage("CreateVertexShader(composite)", result); return false; }
        result = device->CreatePixelShader(compositePs->GetBufferPointer(), compositePs->GetBufferSize(), nullptr, compositePixelShader.put());
        if (FAILED(result)) { error = hresultMessage("CreatePixelShader(composite)", result); return false; }

        const D3D11_INPUT_ELEMENT_DESC birdElements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"INSTANCECENTER", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"INSTANCEVELOCITY", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 8, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"INSTANCEPHASE", 0, DXGI_FORMAT_R32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"INSTANCEDEATH", 0, DXGI_FORMAT_R32_FLOAT, 1, 20, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"INSTANCEALPHA", 0, DXGI_FORMAT_R32_FLOAT, 1, 24, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"BIRDCOLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 28, D3D11_INPUT_PER_INSTANCE_DATA, 1}
        };
        result = device->CreateInputLayout(birdElements, static_cast<UINT>(std::size(birdElements)),
            birdVs->GetBufferPointer(), birdVs->GetBufferSize(), birdInputLayout.put());
        if (FAILED(result)) { error = hresultMessage("CreateInputLayout(bird)", result); return false; }

        const float trailCorners[] = {0.0f, -1.0f, 0.0f, 1.0f, 1.0f, -1.0f,
                                      1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f};
        const D3D11_INPUT_ELEMENT_DESC trailElements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TRAILSTART", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"TRAILCOLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 12, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"TRAILEND", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 24, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"TRAILCOLOR", 1, DXGI_FORMAT_R32G32B32_FLOAT, 1, 36, D3D11_INPUT_PER_INSTANCE_DATA, 1}
        };
        result = device->CreateInputLayout(trailElements, static_cast<UINT>(std::size(trailElements)),
            trailVs->GetBufferPointer(), trailVs->GetBufferSize(), trailInputLayout.put());
        if (FAILED(result)) { error = hresultMessage("CreateInputLayout(trail)", result); return false; }

        if (!createBuffer(static_cast<UINT>(sizeof(BirdMesh)), D3D11_BIND_VERTEX_BUFFER,
                          D3D11_USAGE_IMMUTABLE, 0, BirdMesh.data(), birdMeshBuffer,
                          "CreateBuffer(bird mesh)", error)) return false;
        if (!createBuffer(static_cast<UINT>(BirdInstanceBufferBytes), D3D11_BIND_VERTEX_BUFFER,
                          D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE, nullptr, birdInstanceBuffer,
                          "CreateBuffer(bird instances)", error)) return false;
        if (!createBuffer(static_cast<UINT>(sizeof(trailCorners)), D3D11_BIND_VERTEX_BUFFER,
                          D3D11_USAGE_IMMUTABLE, 0, trailCorners, trailCornerBuffer,
                          "CreateBuffer(trail corners)", error)) return false;
        if (!createBuffer(static_cast<UINT>(TrailInstanceBufferBytes), D3D11_BIND_VERTEX_BUFFER,
                          D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE, nullptr, trailInstanceBuffer,
                          "CreateBuffer(trail instances)", error)) return false;
        if (!createBuffer(sizeof(BirdVertexConstants), D3D11_BIND_CONSTANT_BUFFER,
                          D3D11_USAGE_DEFAULT, 0, nullptr, birdVertexConstants,
                          "CreateBuffer(bird VS constants)", error)) return false;
        if (!createBuffer(sizeof(BirdPixelConstants), D3D11_BIND_CONSTANT_BUFFER,
                          D3D11_USAGE_DEFAULT, 0, nullptr, birdPixelConstants,
                          "CreateBuffer(bird PS constants)", error)) return false;
        if (!createBuffer(sizeof(TrailVertexConstants), D3D11_BIND_CONSTANT_BUFFER,
                          D3D11_USAGE_DEFAULT, 0, nullptr, trailVertexConstants,
                          "CreateBuffer(trail VS constants)", error)) return false;
        if (!createBuffer(sizeof(TrailPixelConstants), D3D11_BIND_CONSTANT_BUFFER,
                          D3D11_USAGE_DEFAULT, 0, nullptr, trailPixelConstants,
                          "CreateBuffer(trail PS constants)", error)) return false;

        D3D11_BLEND_DESC blendDesc{};
        blendDesc.RenderTarget[0].BlendEnable = TRUE;
        blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        result = device->CreateBlendState(&blendDesc, alphaBlend.put());
        if (FAILED(result)) { error = hresultMessage("CreateBlendState", result); return false; }

        D3D11_RASTERIZER_DESC rasterDesc{};
        rasterDesc.FillMode = D3D11_FILL_SOLID;
        rasterDesc.CullMode = D3D11_CULL_NONE;
        rasterDesc.DepthClipEnable = TRUE;
        result = device->CreateRasterizerState(&rasterDesc, rasterizer.put());
        if (FAILED(result)) { error = hresultMessage("CreateRasterizerState", result); return false; }

        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        result = device->CreateSamplerState(&samplerDesc, linearSampler.put());
        if (FAILED(result)) { error = hresultMessage("CreateSamplerState", result); return false; }
        return true;
    }

    bool createDeviceResources(std::string& error) {
        releaseDeviceResources();
        if (!window) {
            error = "Renderer initialization requires a valid HWND.";
            return false;
        }

        constexpr D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
        };
        D3D_FEATURE_LEVEL obtained{};
        HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels, static_cast<UINT>(std::size(featureLevels)),
            D3D11_SDK_VERSION, device.put(), &obtained, context.put());
        if (result == E_INVALIDARG) {
            // Older D3D runtimes reject the 11.1 enum even when 11.0 is supported.
            result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels + 1,
                static_cast<UINT>(std::size(featureLevels) - 1), D3D11_SDK_VERSION,
                device.put(), &obtained, context.put());
        }
        if (FAILED(result)) {
            device.reset();
            context.reset();
            result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels + 1,
                static_cast<UINT>(std::size(featureLevels) - 1), D3D11_SDK_VERSION,
                device.put(), &obtained, context.put());
        }
        if (FAILED(result)) {
            error = hresultMessage("D3D11CreateDevice (hardware and WARP)", result);
            releaseDeviceResources();
            return false;
        }

        ComPtr<IDXGIDevice> dxgiDevice;
        result = device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()));
        if (FAILED(result)) {
            error = hresultMessage("QueryInterface(IDXGIDevice)", result);
            releaseDeviceResources();
            return false;
        }
        ComPtr<IDXGIAdapter> adapter;
        result = dxgiDevice->GetAdapter(adapter.put());
        if (SUCCEEDED(result)) {
            DXGI_ADAPTER_DESC adapterDesc{};
            if (SUCCEEDED(adapter->GetDesc(&adapterDesc))) adapterName = narrow(adapterDesc.Description);
        }
        if (adapterName.empty()) adapterName = "Unknown D3D11 adapter";

        ComPtr<IDXGIFactory2> factory;
        result = adapter ? adapter->GetParent(IID_PPV_ARGS(factory.put())) : E_NOINTERFACE;
        if (FAILED(result)) {
            error = hresultMessage("IDXGIAdapter::GetParent(IDXGIFactory2)", result);
            releaseDeviceResources();
            return false;
        }
        DXGI_SWAP_CHAIN_DESC1 swapDesc{};
        swapDesc.Width = static_cast<UINT>(std::max(width, 1));
        swapDesc.Height = static_cast<UINT>(std::max(height, 1));
        swapDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        swapDesc.Stereo = FALSE;
        swapDesc.SampleDesc.Count = 1;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.BufferCount = SwapBufferCount;
        swapDesc.Scaling = DXGI_SCALING_STRETCH;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        result = factory->CreateSwapChainForHwnd(device.get(), window, &swapDesc, nullptr, nullptr, swapChain.put());
        if (FAILED(result)) {
            error = hresultMessage("IDXGIFactory2::CreateSwapChainForHwnd", result);
            releaseDeviceResources();
            return false;
        }
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        ComPtr<IDXGISwapChain2> swapChain2;
        if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(swapChain2.put()))))
            swapChain2->SetMaximumFrameLatency(1);

        if (!createBackBufferViews(error) || !createStaticResources(error) || !createSceneTarget(error)) {
            releaseDeviceResources();
            return false;
        }
        initialized = true;
        suspended = width <= 0 || height <= 0;
        hasPresented = false;
        return true;
    }

    bool recoverFromFailure(HRESULT cause, const char* operation, std::string& error) {
        const HRESULT removedReason = device ? device->GetDeviceRemovedReason() : S_OK;
        const bool removed = cause == DXGI_ERROR_DEVICE_REMOVED || cause == DXGI_ERROR_DEVICE_RESET ||
            FAILED(removedReason);
        if (!removed) {
            error = hresultMessage(operation, cause);
            return false;
        }
        const std::string original = hresultMessage(operation, cause);
        std::string recoveryError;
        if (createDeviceResources(recoveryError)) {
            error.clear();
            return true;
        }
        error = original + "; device recreation failed: " + recoveryError;
        return false;
    }

    TrailPoint interpolate(const TrailPoint& a, const TrailPoint& b, float time) const noexcept {
        const float amount = b.time > a.time ? std::clamp((time - a.time) / (b.time - a.time), 0.0f, 1.0f) : 0.0f;
        TrailPoint point;
        point.time = time;
        point.position.x = a.position.x + (b.position.x - a.position.x) * amount;
        point.position.y = a.position.y + (b.position.y - a.position.y) * amount;
        point.color = {
            a.color.r + (b.color.r - a.color.r) * amount,
            a.color.g + (b.color.g - a.color.g) * amount,
            a.color.b + (b.color.b - a.color.b) * amount
        };
        return point;
    }

    bool appendSegment(const TrailPoint& start, const TrailPoint& end,
                       float renderNow, float pathEnd, float duration) {
        if (!(end.time > start.time)) return true;
        if (trailCount >= trailInstances.size()) return false;
        const float cutoff = renderNow - duration;
        const float fromTime = std::max(start.time, cutoff);
        const float toTime = std::min(end.time, pathEnd);
        if (!(toTime > fromTime) || end.time <= cutoff || start.time >= pathEnd) return true;
        const TrailPoint clippedStart = fromTime > start.time ? interpolate(start, end, fromTime) : start;
        const TrailPoint clippedEnd = toTime < end.time ? interpolate(start, end, toTime) : end;
        TrailInstance& instance = trailInstances[trailCount++];
        instance.startX = clippedStart.position.x;
        instance.startY = clippedStart.position.y;
        instance.startTime = clippedStart.time;
        instance.startR = clippedStart.color.r;
        instance.startG = clippedStart.color.g;
        instance.startB = clippedStart.color.b;
        instance.endX = clippedEnd.position.x;
        instance.endY = clippedEnd.position.y;
        instance.endTime = clippedEnd.time;
        instance.endR = clippedEnd.color.r;
        instance.endG = clippedEnd.color.g;
        instance.endB = clippedEnd.color.b;
        return true;
    }

    bool appendHistory(const std::array<TrailPoint, HistoryCapacity>& history, int head, int count,
                       float renderNow, float pathEnd, float duration, float sampleInterval,
                       const TrailPoint* liveEndpoint) {
        count = std::clamp(count, 0, HistoryCapacity);
        if (count < 2 && !liveEndpoint) return true;
        TrailPoint previousRaw{};
        bool havePreviousRaw = false;
        TrailPoint previousKept{};
        bool havePreviousKept = false;
        float nextSampleTime = -std::numeric_limits<float>::infinity();
        const float cutoff = renderNow - duration;
        bool overflow = false;

        auto acceptPoint = [&](TrailPoint point, bool forceKeep) {
            if (!std::isfinite(point.time) || !std::isfinite(point.position.x) || !std::isfinite(point.position.y)) {
                havePreviousRaw = false;
                havePreviousKept = false;
                return;
            }
            if (!finiteColor(point.color)) point.color = {};
            if (havePreviousRaw && !(point.time > previousRaw.time)) {
                if (forceKeep && havePreviousKept && point.time > previousKept.time) {
                    if (!appendSegment(previousKept, point, renderNow, pathEnd, duration)) {
                        overflow = true;
                        return;
                    }
                    previousKept = point;
                    nextSampleTime = point.time + sampleInterval;
                }
                previousRaw = point;
                havePreviousRaw = true;
                return;
            }
            if (point.time > pathEnd) {
                if (havePreviousRaw && pathEnd > previousRaw.time && previousRaw.time < pathEnd)
                    point = interpolate(previousRaw, point, pathEnd);
                else
                    return;
                forceKeep = true;
            }
            if (point.time < cutoff) {
                previousRaw = point;
                havePreviousRaw = true;
                return;
            }
            if (!havePreviousKept) {
                TrailPoint first = point;
                if (havePreviousRaw && previousRaw.time < cutoff && point.time > cutoff)
                    first = interpolate(previousRaw, point, cutoff);
                previousKept = first;
                havePreviousKept = true;
                nextSampleTime = first.time + sampleInterval;
            } else if (forceKeep || point.time >= nextSampleTime) {
                if (!appendSegment(previousKept, point, renderNow, pathEnd, duration)) {
                    overflow = true;
                    return;
                }
                previousKept = point;
                nextSampleTime = point.time + sampleInterval;
            }
            previousRaw = point;
            havePreviousRaw = true;
        };

        if (count > 0) {
            const int normalizedHead = ((head % HistoryCapacity) + HistoryCapacity) % HistoryCapacity;
            const int first = (normalizedHead - count + 1 + HistoryCapacity * 2) % HistoryCapacity;
            for (int sample = 0; sample < count && !overflow; ++sample) {
                const int index = (first + sample) % HistoryCapacity;
                acceptPoint(history[static_cast<std::size_t>(index)], sample == count - 1 && !liveEndpoint);
                if (havePreviousRaw && previousRaw.time >= pathEnd) break;
            }
        }
        if (liveEndpoint && !overflow) acceptPoint(*liveEndpoint, true);
        return !overflow;
    }

};

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;

bool Renderer::initialize(HWND window, int width, int height, std::string& error) {
    error.clear();
    if (!impl_) {
        error = "Renderer internal state is unavailable.";
        return false;
    }
    if (!window) {
        error = "Renderer initialization requires a valid HWND.";
        return false;
    }
    if (width < 0 || height < 0) {
        error = "Renderer dimensions cannot be negative.";
        return false;
    }
    impl_->window = window;
    impl_->width = width;
    impl_->height = height;
    impl_->renderScale = 1.0f;
    impl_->initialized = false;
    impl_->suspended = width == 0 || height == 0;
    if (!impl_->createDeviceResources(error)) return false;
    error.clear();
    return true;
}

bool Renderer::resize(int width, int height, std::string& error) {
    error.clear();
    if (!impl_ || !impl_->initialized || !impl_->swapChain) {
        error = "Renderer must be initialized before resize.";
        return false;
    }
    if (width < 0 || height < 0) {
        error = "Renderer dimensions cannot be negative.";
        return false;
    }
    impl_->width = width;
    impl_->height = height;
    impl_->suspended = width == 0 || height == 0;
    if (impl_->suspended) {
        impl_->hasPresented = false;
        return true;
    }
    if (width == impl_->targetWidth && height == impl_->targetHeight) {
        // targetWidth/Height are render-scale dimensions, so only skip when both the
        // swap chain and the active scene target already match the requested size.
        DXGI_SWAP_CHAIN_DESC1 desc{};
        if (SUCCEEDED(impl_->swapChain->GetDesc1(&desc)) &&
            desc.Width == static_cast<UINT>(width) && desc.Height == static_cast<UINT>(height) &&
            impl_->renderScale == 1.0f)
            return true;
    }

    if (impl_->context) {
        ID3D11RenderTargetView* noTarget = nullptr;
        impl_->context->OMSetRenderTargets(1, &noTarget, nullptr);
        impl_->context->Flush();
    }
    impl_->backBufferView.reset();
    impl_->captureView.reset();
    impl_->captureTexture.reset();
    impl_->captureWidth = 0;
    impl_->captureHeight = 0;
    impl_->sceneResource.reset();
    impl_->sceneView.reset();
    impl_->sceneTexture.reset();
    impl_->targetWidth = 0;
    impl_->targetHeight = 0;

    HRESULT result = impl_->swapChain->ResizeBuffers(SwapBufferCount, static_cast<UINT>(width),
        static_cast<UINT>(height), DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(result)) {
        const std::string original = hresultMessage("IDXGISwapChain::ResizeBuffers", result);
        std::string recoveryError;
        if (impl_->createDeviceResources(recoveryError)) {
            error.clear();
            return true;
        }
        error = original + "; swap-chain recreation failed: " + recoveryError;
        return false;
    }
    if (!impl_->createBackBufferViews(error) || !impl_->createSceneTarget(error)) {
        const std::string original = error;
        std::string recoveryError;
        if (impl_->createDeviceResources(recoveryError)) {
            error.clear();
            return true;
        }
        error = original + "; renderer resource recreation failed: " + recoveryError;
        return false;
    }
    impl_->hasPresented = false;
    error.clear();
    return true;
}

bool Renderer::draw(std::span<const Bird> birds, std::span<const Ghost> ghosts,
                    const Settings& settings, float simulationTime,
                    float logicalWidth, float logicalHeight, std::string& error) {
    error.clear();
    if (!impl_ || !impl_->initialized || !impl_->device || !impl_->context || !impl_->swapChain ||
        !impl_->backBufferView || !impl_->sceneView || !impl_->sceneResource || !impl_->birdVertexShader ||
        !impl_->compositeVertexShader || !impl_->compositePixelShader) {
        error = "Renderer is not initialized with complete D3D11 resources.";
        return false;
    }
    if (birds.size() > static_cast<std::size_t>(MaxBirds)) {
        error = "Renderer supports at most " + std::to_string(MaxBirds) + " birds.";
        return false;
    }
    if (ghosts.size() > static_cast<std::size_t>(MaxGhosts)) {
        error = "Renderer supports at most " + std::to_string(MaxGhosts) + " trail ghosts.";
        return false;
    }
    if (impl_->suspended || impl_->width <= 0 || impl_->height <= 0) return true;
    if (!std::isfinite(logicalWidth) || !std::isfinite(logicalHeight) ||
        logicalWidth <= 0.0f || logicalHeight <= 0.0f) {
        error = "Logical client width and height must be finite positive values.";
        return false;
    }
    if (!std::isfinite(simulationTime)) simulationTime = 0.0f;

    const HRESULT removalState = impl_->device->GetDeviceRemovedReason();
    if (FAILED(removalState)) {
        if (!impl_->createDeviceResources(error)) return false;
    }
    const float renderScale = bounded(settings.renderScale, 0.5f, 1.0f, 1.0f);
    if (renderScale != impl_->renderScale || !impl_->sceneTexture) {
        impl_->renderScale = renderScale;
        if (!impl_->createSceneTarget(error)) {
            const std::string original = error;
            const HRESULT removedReason = impl_->device ? impl_->device->GetDeviceRemovedReason() : S_OK;
            if (FAILED(removedReason)) {
                std::string recoveryError;
                if (impl_->createDeviceResources(recoveryError)) {
                    error.clear();
                    return true;
                }
                error = original + "; device recreation failed: " + recoveryError;
            }
            return false;
        }
    }

    const float size = bounded(settings.size, 1.0f, 16.0f, 6.58f);
    const float brightness = bounded(settings.brightness, 0.1f, 2.0f, 1.31f);
    const float contrast = bounded(settings.contrast, 0.5f, 2.0f, 1.2f);
    const float duration = bounded(settings.trailDuration, 0.1f, 0.5f, 0.28f);
    const float trailHz = bounded(static_cast<float>(settings.trailHz), 10.0f, 60.0f, 30.0f);
    const float sampleInterval = 1.0f / trailHz;
    const Color settingColor = normalizedColor(settings.color);
    const bool randomColor = settings.randomColor;
    const bool trailsEnabled = settings.trails;

    std::size_t birdCount = 0;
    impl_->trailCount = 0;
    for (const Bird& bird : birds) {
        if (bird.phase == Phase::Dead) continue;
        if (!std::isfinite(bird.position.x) || !std::isfinite(bird.position.y)) continue;
        if (birdCount >= impl_->birdInstances.size()) break;
        float vx = std::isfinite(bird.velocity.x) ? bird.velocity.x : 0.0f;
        float vy = std::isfinite(bird.velocity.y) ? bird.velocity.y : 0.0f;
        const std::uint32_t phaseHash = (bird.id + 1u) * 374761393u;
        const float fallbackPhase = static_cast<float>((phaseHash ^ (phaseHash >> 13)) & 0xffffu) *
                                    (2.0f * Pi / 65536.0f);
        const float phase = std::isfinite(bird.wingPhase) ? bird.wingPhase : fallbackPhase;
        float deathProgress = -1.0f;
        float phaseAlpha = 1.0f;
        float pathEnd = simulationTime;
        if (bird.phase == Phase::Dying) {
            const float elapsed = std::max(0.0f, std::isfinite(bird.phaseTime) ? bird.phaseTime : 0.0f);
            deathProgress = std::clamp(elapsed / 0.28f, 0.0f, 1.0f);
            pathEnd = simulationTime - elapsed;
        } else if (bird.phase == Phase::Entering) {
            const float elapsed = std::max(0.0f, std::isfinite(bird.phaseTime) ? bird.phaseTime : 0.0f);
            phaseAlpha = std::clamp(elapsed / 0.45f, 0.0f, 1.0f);
            phaseAlpha = phaseAlpha * phaseAlpha * (3.0f - 2.0f * phaseAlpha);
        } else if (bird.phase == Phase::Leaving) {
            float timeToViewportEdge = std::numeric_limits<float>::infinity();
            if (vx > 1.0e-4f)
                timeToViewportEdge = std::min(timeToViewportEdge, (logicalWidth - bird.position.x) / vx);
            else if (vx < -1.0e-4f)
                timeToViewportEdge = std::min(timeToViewportEdge, (0.0f - bird.position.x) / vx);
            if (vy > 1.0e-4f)
                timeToViewportEdge = std::min(timeToViewportEdge, (logicalHeight - bird.position.y) / vy);
            else if (vy < -1.0e-4f)
                timeToViewportEdge = std::min(timeToViewportEdge, (0.0f - bird.position.y) / vy);
            if (std::isfinite(timeToViewportEdge)) {
                const float progress = std::clamp(1.0f - timeToViewportEdge / 0.42f, 0.0f, 1.0f);
                phaseAlpha = 1.0f - progress * progress * (3.0f - 2.0f * progress);
            }
        }
        Color birdColor = normalizedColor(bird.color, settingColor);
        impl_->birdInstances[birdCount++] = {
            bird.position.x, bird.position.y, vx, vy, phase, deathProgress, phaseAlpha,
            birdColor.r, birdColor.g, birdColor.b
        };

        if (trailsEnabled && bird.historyCount > 0) {
            TrailPoint endpoint{};
            endpoint.position = bird.position;
            endpoint.time = pathEnd;
            endpoint.color = birdColor;
            if (!impl_->appendHistory(bird.history, bird.historyHead, bird.historyCount,
                                      simulationTime, pathEnd, duration, sampleInterval, &endpoint)) {
                error = "Trail segment capacity exceeded while collecting bird histories.";
                return false;
            }
        }
    }

    if (trailsEnabled) {
        for (const Ghost& ghost : ghosts) {
            if (ghost.historyCount <= 1 || !std::isfinite(ghost.retiredAt)) continue;
            const float pathEnd = std::min(simulationTime, ghost.retiredAt);
            if (pathEnd <= simulationTime - duration) continue;
            if (!impl_->appendHistory(ghost.history, ghost.historyHead, ghost.historyCount,
                                      simulationTime, pathEnd, duration, sampleInterval, nullptr)) {
                error = "Trail segment capacity exceeded while collecting retired histories.";
                return false;
            }
        }
    }

    if (birdCount > 0) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT result = impl_->context->Map(impl_->birdInstanceBuffer.get(), 0,
            D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(result)) {
            if (impl_->recoverFromFailure(result, "Map(bird instance buffer)", error)) return true;
            return false;
        }
        std::memcpy(mapped.pData, impl_->birdInstances.data(), birdCount * sizeof(BirdInstance));
        impl_->context->Unmap(impl_->birdInstanceBuffer.get(), 0);
    }
    if (impl_->trailCount > 0) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT result = impl_->context->Map(impl_->trailInstanceBuffer.get(), 0,
            D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(result)) {
            if (impl_->recoverFromFailure(result, "Map(trail instance buffer)", error)) return true;
            return false;
        }
        std::memcpy(mapped.pData, impl_->trailInstances.data(), impl_->trailCount * sizeof(TrailInstance));
        impl_->context->Unmap(impl_->trailInstanceBuffer.get(), 0);
    }

    const BirdVertexConstants birdVsConstants{
        logicalWidth, logicalHeight, size, simulationTime
    };
    const BirdPixelConstants birdPsConstants{
        settingColor.r, settingColor.g, settingColor.b, brightness,
        contrast, randomColor ? 1u : 0u, {0.0f, 0.0f}
    };
    const TrailVertexConstants trailVsConstants{
        logicalWidth, logicalHeight, simulationTime, duration,
        std::clamp(size * 0.2f, 0.55f, 1.8f), {0.0f, 0.0f, 0.0f}
    };
    const TrailPixelConstants trailPsConstants{
        settingColor.r, settingColor.g, settingColor.b, brightness,
        contrast, randomColor ? 1u : 0u, {0.0f, 0.0f}
    };
    impl_->context->UpdateSubresource(impl_->birdVertexConstants.get(), 0, nullptr, &birdVsConstants, 0, 0);
    impl_->context->UpdateSubresource(impl_->birdPixelConstants.get(), 0, nullptr, &birdPsConstants, 0, 0);
    impl_->context->UpdateSubresource(impl_->trailVertexConstants.get(), 0, nullptr, &trailVsConstants, 0, 0);
    impl_->context->UpdateSubresource(impl_->trailPixelConstants.get(), 0, nullptr, &trailPsConstants, 0, 0);

    ID3D11RenderTargetView* sceneTarget = impl_->sceneView.get();
    impl_->context->OMSetRenderTargets(1, &sceneTarget, nullptr);
    D3D11_VIEWPORT sceneViewport{};
    sceneViewport.Width = static_cast<float>(impl_->targetWidth);
    sceneViewport.Height = static_cast<float>(impl_->targetHeight);
    sceneViewport.MinDepth = 0.0f;
    sceneViewport.MaxDepth = 1.0f;
    impl_->context->RSSetViewports(1, &sceneViewport);
    impl_->context->RSSetState(impl_->rasterizer.get());
    const float background[4] = {0.012f, 0.034f, 0.07f, 1.0f};
    impl_->context->ClearRenderTargetView(sceneTarget, background);
    impl_->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    impl_->context->OMSetBlendState(impl_->alphaBlend.get(), nullptr, 0xffffffffu);

    if (trailsEnabled && impl_->trailCount > 0) {
        const UINT strides[] = {static_cast<UINT>(sizeof(float) * 2), static_cast<UINT>(sizeof(TrailInstance))};
        const UINT offsets[] = {0, 0};
        ID3D11Buffer* buffers[] = {impl_->trailCornerBuffer.get(), impl_->trailInstanceBuffer.get()};
        impl_->context->IASetInputLayout(impl_->trailInputLayout.get());
        impl_->context->IASetVertexBuffers(0, 2, buffers, strides, offsets);
        impl_->context->VSSetShader(impl_->trailVertexShader.get(), nullptr, 0);
        ID3D11Buffer* vsConstants = impl_->trailVertexConstants.get();
        impl_->context->VSSetConstantBuffers(0, 1, &vsConstants);
        impl_->context->PSSetShader(impl_->trailPixelShader.get(), nullptr, 0);
        ID3D11Buffer* psConstants = impl_->trailPixelConstants.get();
        impl_->context->PSSetConstantBuffers(1, 1, &psConstants);
        impl_->context->DrawInstanced(6, static_cast<UINT>(impl_->trailCount), 0, 0);
    }
    if (birdCount > 0) {
        const UINT strides[] = {static_cast<UINT>(sizeof(BirdVertex)), static_cast<UINT>(sizeof(BirdInstance))};
        const UINT offsets[] = {0, 0};
        ID3D11Buffer* buffers[] = {impl_->birdMeshBuffer.get(), impl_->birdInstanceBuffer.get()};
        impl_->context->IASetInputLayout(impl_->birdInputLayout.get());
        impl_->context->IASetVertexBuffers(0, 2, buffers, strides, offsets);
        impl_->context->VSSetShader(impl_->birdVertexShader.get(), nullptr, 0);
        ID3D11Buffer* vsConstants = impl_->birdVertexConstants.get();
        impl_->context->VSSetConstantBuffers(0, 1, &vsConstants);
        impl_->context->PSSetShader(impl_->birdPixelShader.get(), nullptr, 0);
        ID3D11Buffer* psConstants = impl_->birdPixelConstants.get();
        impl_->context->PSSetConstantBuffers(1, 1, &psConstants);
        impl_->context->DrawInstanced(static_cast<UINT>(BirdMesh.size()), static_cast<UINT>(birdCount), 0, 0);
    }

    impl_->compositeTo(impl_->backBufferView.get());

    const HRESULT presentResult = impl_->swapChain->Present(0, 0);
    if (FAILED(presentResult)) {
        if (impl_->recoverFromFailure(presentResult, "IDXGISwapChain::Present", error)) return true;
        return false;
    }
    impl_->hasPresented = true;
    error.clear();
    return true;
}

bool Renderer::capture(const std::wstring& path, std::string& error) {
    error.clear();
    if (!impl_ || !impl_->initialized || !impl_->device || !impl_->context || !impl_->swapChain ||
        !impl_->hasPresented) {
        error = "A rendered frame is required before PNG capture.";
        return false;
    }
    if (path.empty()) {
        error = "PNG capture path cannot be empty.";
        return false;
    }

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeCom = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        error = hresultMessage("CoInitializeEx", comResult);
        return false;
    }
    struct ComUninitializer {
        bool active;
        ~ComUninitializer() { if (active) CoUninitialize(); }
    } comUninitializer{uninitializeCom};

    if (!impl_->sceneTexture || !impl_->sceneResource || !impl_->compositeVertexShader ||
        !impl_->compositePixelShader || !impl_->linearSampler) {
        error = "A complete rendered scene is required before PNG capture.";
        return false;
    }
    if (!impl_->createCaptureTarget(error)) {
        const std::string original = error;
        const HRESULT removedReason = impl_->device->GetDeviceRemovedReason();
        if (FAILED(removedReason)) {
            std::string recoveryError;
            if (impl_->createDeviceResources(recoveryError))
                error = "The D3D11 device was recreated; retry PNG capture.";
            else
                error = original + "; device recreation failed: " + recoveryError;
        }
        return false;
    }
    // Re-composite the app-owned scene texture at native client resolution.
    // Reading the swap-chain surface after Present is not reliable in flip-discard mode.
    impl_->compositeTo(impl_->captureView.get());
    D3D11_TEXTURE2D_DESC desc{};
    impl_->captureTexture->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    HRESULT result = impl_->device->CreateTexture2D(&stagingDesc, nullptr, staging.put());
    if (FAILED(result)) {
        if (impl_->recoverFromFailure(result, "CreateTexture2D(capture staging)", error))
            error = "The D3D11 device was recreated; retry PNG capture.";
        return false;
    }
    impl_->context->CopyResource(staging.get(), impl_->captureTexture.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = impl_->context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) {
        if (impl_->recoverFromFailure(result, "Map(capture staging)", error))
            error = "The D3D11 device was recreated; retry PNG capture.";
        return false;
    }
    MappedResource mappedGuard{impl_->context.get(), staging.get()};

    ComPtr<IWICImagingFactory> factory;
    result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(factory.put()));
    if (FAILED(result)) {
        error = hresultMessage("CoCreateInstance(WIC imaging factory)", result);
        return false;
    }
    ComPtr<IWICStream> stream;
    result = factory->CreateStream(stream.put());
    if (FAILED(result)) {
        error = hresultMessage("IWICImagingFactory::CreateStream", result);
        return false;
    }
    result = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (FAILED(result)) {
        error = hresultMessage("IWICStream::InitializeFromFilename", result);
        return false;
    }
    ComPtr<IWICBitmapEncoder> encoder;
    result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put());
    if (FAILED(result)) {
        error = hresultMessage("IWICImagingFactory::CreateEncoder(PNG)", result);
        return false;
    }
    result = encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);
    if (FAILED(result)) {
        error = hresultMessage("IWICBitmapEncoder::Initialize", result);
        return false;
    }
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    result = encoder->CreateNewFrame(frame.put(), properties.put());
    if (FAILED(result)) {
        error = hresultMessage("IWICBitmapEncoder::CreateNewFrame", result);
        return false;
    }
    result = frame->Initialize(properties.get());
    if (SUCCEEDED(result)) result = frame->SetSize(desc.Width, desc.Height);
    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(result)) result = frame->SetPixelFormat(&pixelFormat);
    if (SUCCEEDED(result) && !IsEqualGUID(pixelFormat, GUID_WICPixelFormat32bppBGRA)) {
        result = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    }
    const std::uint64_t byteCount64 = static_cast<std::uint64_t>(mapped.RowPitch) * desc.Height;
    if (SUCCEEDED(result) && byteCount64 <= std::numeric_limits<UINT>::max()) {
        result = frame->WritePixels(desc.Height, mapped.RowPitch, static_cast<UINT>(byteCount64),
                                    static_cast<BYTE*>(mapped.pData));
    } else if (SUCCEEDED(result)) {
        result = E_INVALIDARG;
    }
    if (SUCCEEDED(result)) result = frame->Commit();
    if (SUCCEEDED(result)) result = encoder->Commit();
    // mappedGuard releases the staging-resource map on every exit path.
    if (FAILED(result)) {
        error = hresultMessage("Writing PNG pixels", result);
        return false;
    }
    error.clear();
    return true;
}

std::string Renderer::adapterName() const {
    return impl_ ? impl_->adapterName : std::string{};
}

} // namespace flock
