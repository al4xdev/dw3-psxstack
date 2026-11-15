// Alpha-blended RGBA overlay shader (runtime/render_gpu.c): draws touch controls over the swapchain.
Texture2D<float4> image : register(t0, space2);
cbuffer Present : register(b0, space3) {
    int4 dst; // the rectangle in the target: x, y, w, h
    int4 src; // the overlay's size: w, h, unused, unused
};

float4 main(float4 pos : SV_Position) : SV_Target0 {
    int2 p = int2(pos.xy) - dst.xy;
    int2 t = ((2 * p + 1) * src.xy) / (2 * dst.zw);
    return image.Load(int3(t, 0));
}
