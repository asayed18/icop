#ifndef NSFW_D3D11_SHADERS_H
#define NSFW_D3D11_SHADERS_H

static const char kFullscreenVertexShader[] =
    "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
    "VSOut main(uint id : SV_VertexID) {\n"
    "  VSOut o;\n"
    "  float2 p = float2((id << 1) & 2, id & 2);\n"
    "  o.uv = p;\n"
    "  o.pos = float4(p * float2(2,-2) + float2(-1,1), 0, 1);\n"
    "  return o;\n"
    "}\n";

static const char kBlurPixelShader[] =
    "Texture2D image : register(t0);\n"
    "SamplerState linearClamp : register(s0);\n"
    "cbuffer BlurData : register(b0) { float2 texel; float2 direction; };\n"
    "float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {\n"
    "  float2 step = texel * direction;\n"
    "  float4 c = image.Sample(linearClamp, uv) * 0.2270270270;\n"
    "  c += image.Sample(linearClamp, uv + step * 1.0) * 0.1945945946;\n"
    "  c += image.Sample(linearClamp, uv - step * 1.0) * 0.1945945946;\n"
    "  c += image.Sample(linearClamp, uv + step * 2.0) * 0.1216216216;\n"
    "  c += image.Sample(linearClamp, uv - step * 2.0) * 0.1216216216;\n"
    "  c += image.Sample(linearClamp, uv + step * 3.0) * 0.0540540541;\n"
    "  c += image.Sample(linearClamp, uv - step * 3.0) * 0.0540540541;\n"
    "  c += image.Sample(linearClamp, uv + step * 4.0) * 0.0162162162;\n"
    "  c += image.Sample(linearClamp, uv - step * 4.0) * 0.0162162162;\n"
    "  return c;\n"
    "}\n";

#endif
