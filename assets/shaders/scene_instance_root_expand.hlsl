// Convert one instance-hierarchy replacement root into the common cluster
// candidate record. Raw uint words keep the layout stable on Metal and DXIL.

StructuredBuffer<uint> roots : register(t0, space0);
RWStructuredBuffer<uint> candidates : register(u0, space1);

cbuffer Expand : register(b0, space2) {
    uint rootCount;
    uint pad0;
    uint pad1;
    uint pad2;
};

float loadFloat(uint value) { return asfloat(value); }

[numthreads(64, 1, 1)]
void ExpandCS(uint3 id : SV_DispatchThreadID) {
    if (id.x >= rootCount) return;
    const uint root = id.x * 8;
    const uint output = id.x * 8;
    candidates[output + 0] = roots[root + 0];
    candidates[output + 1] = roots[root + 1];
    candidates[output + 2] = roots[root + 2];
    candidates[output + 3] = roots[root + 3];
    candidates[output + 4] = roots[root + 4];
    candidates[output + 5] = roots[root + 5];
    candidates[output + 6] = roots[root + 6];
    candidates[output + 7] = roots[root + 7];
}
