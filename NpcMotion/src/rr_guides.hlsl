// DLSS Ray Reconstruction's guides from FINAL FANTASY VII REBIRTH's G-buffer, at the render size, one thread a pixel.
// The G-buffer as the game decodes it (Shader Injector's 1.0.0.5 includes, which follow the game's DXIL):
//   GBufferA: world normal * 0.5 + 0.5; .a the packed dielectric specular
//   GBufferB: metallic (an eye's coat), -, roughness, shading model in the low 4 bits of .a
//   GBufferC: base colour (an sRGB view: linear), material AO
// Guides:
// - diffuse albedo: base colour * (1 - metallic);
// - specular albedo: NVIDIA's EnvBRDFApprox2 (the DLSS-RR integration guide's appendix) of the game's F0, the squared
//   roughness and N.V. The game's F0: dielectric = clamp(s < 2/3 ? 0.06 s : 0.36 s - 0.20, 0, 0.16) of s = GBufferA.a,
//   F0 = dielectric + metallic * (base colour - dielectric);
// - the world normal with the linear roughness in .w (NVSDK_NGX_DLSS_Roughness_Mode_Packed); the game's roughness:
//   hair 0.48, skin * 0.93655.
// Shading model 0 (unlit: the sky, cleared pixels) gets NVIDIA's defaults: albedo 0.5, specular albedo 0, normal 0.
// ShowGuide (development) writes one guide over DLSS's output, nearest sampled, to look at on screen.
// SPDX-License-Identifier: MIT

cbuffer Constants : register(b0) {
  float4 WorldToView0;     // TranslatedWorldToView's rows (xyz); row vectors: v_view = v_world * M
  float4 WorldToView1;
  float4 WorldToView2;
  float2 ProjectionScale;  // ViewToClip[0][0], [1][1]
  uint2 Size;              // the render size (the view's top-left part of the textures)
  uint2 OutputSize;        // ShowGuide: DLSS's output size
  uint Guide;              // ShowGuide: 1 diffuse albedo, 2 specular albedo, 3 normal, 4 roughness
  uint Unused;
};
Texture2D<float4> GBufferA : register(t0);
Texture2D<float4> GBufferB : register(t1);
Texture2D<float4> GBufferC : register(t2);
Texture2D<float4> GuideDiffuse : register(t3);
Texture2D<float4> GuideSpecular : register(t4);
Texture2D<float4> GuideNormals : register(t5);
RWTexture2D<float4> DiffuseAlbedo : register(u0);
RWTexture2D<float4> SpecularAlbedo : register(u1);
RWTexture2D<float4> NormalsRoughness : register(u2);
RWTexture2D<float4> Output : register(u3);

static const uint kUnlit = 0u;
static const uint kSkin = 3u;  // SHADINGMODELID_PREINTEGRATED_SKIN
static const uint kHair = 7u;
static const uint kEye = 9u;

// The DLSS-RR integration guide's appendix (Ray Tracing Gems, chapter 32).
float3 EnvBRDFApprox2(float3 specular_color, float alpha, float NoV) {
  NoV = abs(NoV);
  float4 X;
  X.x = 1.0;
  X.y = NoV;
  X.z = NoV * NoV;
  X.w = NoV * X.z;
  float4 Y;
  Y.x = 1.0;
  Y.y = alpha;
  Y.z = alpha * alpha;
  Y.w = alpha * Y.z;
  const float2x2 M1 = float2x2(0.99044, -1.28514, 1.29678, -0.755907);
  const float3x3 M2 = float3x3(1.0, 2.92338, 59.4188, 20.3225, -27.0302, 222.592, 121.563, 626.13, 316.627);
  const float2x2 M3 = float2x2(0.0365463, 3.32707, 9.0632, -9.04756);
  const float3x3 M4 = float3x3(1.0, 3.59685, -1.36772, 9.04401, -16.3174, 9.22949, 5.56589, 19.7886, -20.2123);
  float bias = dot(mul(M1, X.xy), Y.xy) * rcp(dot(mul(M2, X.xyw), Y.xyw));
  const float scale = dot(mul(M3, X.xy), Y.xy) * rcp(dot(mul(M4, X.xzw), Y.xyw));
  bias *= saturate(specular_color.g * 50.0);  // the guide's hack for a specular reflectance of 0
  return mad(specular_color, max(0.0, scale), max(0.0, bias));
}

[numthreads(8, 8, 1)]
void Guides(uint3 id : SV_DispatchThreadID) {
  if (id.x >= Size.x || id.y >= Size.y) return;
  const int3 p = int3(id.xy, 0);
  const float4 a = GBufferA.Load(p);
  const float4 b = GBufferB.Load(p);
  const uint shading_model = (uint)round(saturate(b.a) * 255.0) & 15u;
  if (shading_model == kUnlit) {
    DiffuseAlbedo[id.xy] = float4(0.5, 0.5, 0.5, 1.0);
    SpecularAlbedo[id.xy] = float4(0.0, 0.0, 0.0, 1.0);
    NormalsRoughness[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
    return;
  }
  const float3 encoded = a.xyz * 2.0 - 1.0;
  const float length_sq = dot(encoded, encoded);
  const float3 normal = length_sq > 1e-6 ? encoded * rsqrt(length_sq) : float3(0.0, 0.0, 1.0);
  const float metallic = shading_model == kEye ? 0.0 : b.r;
  float roughness = b.b;
  if (shading_model == kSkin)
    roughness *= 0.93655;
  else if (shading_model == kHair)
    roughness = 0.48;
  const float3 base_color = GBufferC.Load(p).rgb;
  const float s = saturate(a.a);
  const float dielectric = clamp(s < 2.0 / 3.0 ? s * 0.06 : s * 0.36 - 0.20, 0.0, 0.16);
  const float3 f0 = dielectric + metallic * (base_color - dielectric);
  // N.V in view space: the pixel's view ray from the projection (Unreal's view space: x right, y up, z forward).
  const float3 n_view = normal.x * WorldToView0.xyz + normal.y * WorldToView1.xyz + normal.z * WorldToView2.xyz;
  const float2 ndc = float2((id.x + 0.5) / Size.x * 2.0 - 1.0, 1.0 - (id.y + 0.5) / Size.y * 2.0);
  const float3 ray = normalize(float3(ndc / ProjectionScale, 1.0));
  const float NoV = dot(n_view, -ray);
  DiffuseAlbedo[id.xy] = float4(base_color * (1.0 - metallic), 1.0);
  SpecularAlbedo[id.xy] = float4(EnvBRDFApprox2(f0, roughness * roughness, NoV), 1.0);
  NormalsRoughness[id.xy] = float4(normal, roughness);
}

[numthreads(8, 8, 1)]
void ShowGuide(uint3 id : SV_DispatchThreadID) {
  if (id.x >= OutputSize.x || id.y >= OutputSize.y) return;
  const uint2 source = min(uint2((float2(id.xy) + 0.5) * float2(Size) / float2(OutputSize)), Size - 1u);
  const int3 p = int3(source, 0);
  float3 c;
  if (Guide == 1u)
    c = GuideDiffuse.Load(p).rgb;
  else if (Guide == 2u)
    c = GuideSpecular.Load(p).rgb;
  else if (Guide == 3u)
    c = GuideNormals.Load(p).xyz * 0.5 + 0.5;
  else
    c = GuideNormals.Load(p).www;
  Output[id.xy] = float4(c, 1.0);
}
