#ifndef PAGE_LEVELS_HLSLI
#define PAGE_LEVELS_HLSLI
// The datasets a page of ground is read from, by the number a shader knows
// them by (GpuTerrain::dataset): 0..3 are H4, H8, H16 and H64 on 512 m pages;
// 4 is H256 on 2 km pages and 5 is H1024 on 8 km pages. The last two have the
// H64 page's shape and live in its atlas; in the table they share layer 4,
// H1024 from half the table's width (GpuTerrain::publishTable).
static const int kPageDatasets = 6;

float pageMetresOf(int level) { return level == 4 ? 2048.0 : level == 5 ? 8192.0 : 512.0; }
float pageStepOf(int level)
{
    return level == 0 ? 4.0 : level == 1 ? 8.0 : level == 2 ? 16.0 : level == 3 ? 64.0 : level == 4 ? 256.0 : 1024.0;
}

// Where in the table the page holding p is, at a dataset: xy the texture
// coordinate, z the layer (the dataset). The table wraps (GpuTerrain::
// publishTable): a page sits at its coordinates modulo the side and the texel
// says which page it holds. `origin` is the page's corner in metres, `page`
// its coordinates.
float3 pageTableTexel(float2 p, int level, float2 tableSize, out float2 origin, out float2 page)
{
    const float metres = pageMetresOf(level);
    page = floor(p / metres);
    origin = page * metres;
    const float2 index = page - floor(page / tableSize) * tableSize;
    return float3((index + 0.5) / tableSize, (float)level);
}
// A texel as the table holds it - the uv of the page's first sample, uv per
// metre (atlases are square: one number), and which turn of the wrap the page
// is on - as the rest reads it: uv, and uv per metre. Nought when the texel
// holds some other page, or none.
float4 pageEntryFrom(float4 held, float2 page, float2 tableSize)
{
    const float2 turn = floor(page / tableSize);
    if (abs(held.w - (turn.x * 1024.0 + turn.y + 0.25)) > 0.1) return float4(0, 0, 0, 0);
    return float4(held.xy, held.z, held.z);
}
#endif
