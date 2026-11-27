// RHI self-test, frame part (issue #32, RHISelfTest.cpp): an async compute
// pass fills a buffer through set 1, which a graphics-queue pass then copies
// out. Output = index * 3 + 7.

[[vk::binding(0, 1)]] RWStructuredBuffer<uint> gOutput;

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    gOutput[id.x] = id.x * 3u + 7u;
}
