// The overlay's composite: the menu picture (premultiplied, sRGB-encoded) over the back buffer, in the buffer's
// encoding. Compiled offline to VS and PS bytecode headers.

Texture2D<float4> gMenu : register(t0);
SamplerState gPoint : register(s0);

cbuffer C : register(b0)
{
    uint mode;
    float nits;
};

struct V
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

V VSMain(uint id : SV_VertexID)
{
    V v;
    float2 uv = float2((id << 1) & 2, id & 2);
    v.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    v.uv = uv;
    return v;
}

float SrgbToLinear1(float c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float3 SrgbToLinear(float3 c)
{
    return float3(SrgbToLinear1(c.r), SrgbToLinear1(c.g), SrgbToLinear1(c.b));
}

float3 Pq(float3 n)
{
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 y = pow(saturate(n / 10000.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

float3 Rec709To2020(float3 c)
{
    return float3(dot(c, float3(0.6274, 0.3293, 0.0433)), dot(c, float3(0.0691, 0.9195, 0.0114)),
                  dot(c, float3(0.0164, 0.0880, 0.8956)));
}

float4 PSMain(V v) : SV_Target
{
    float4 m = gMenu.Sample(gPoint, v.uv);
    if (m.a <= 0.0)
        return float4(0.0, 0.0, 0.0, 0.0);
    float3 c = m.rgb / m.a;
    float3 o;
    if (mode == 0)
        o = c;
    else if (mode == 1)
        o = SrgbToLinear(c) * (nits / 80.0);
    else
        o = Pq(Rec709To2020(SrgbToLinear(c)) * nits);
    return float4(o * m.a, m.a);
}
