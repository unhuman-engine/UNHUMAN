// RHI self-test (issue #30, UHE/src/UHE/Renderer/RHISelfTest.cpp): one compute
// kernel that reads and writes through descriptor set 1, the per-dispatch set
// a pipeline adds after the bindless set 0. Output = input * 2 + 1.

[[vk::binding(0, 1)]] StructuredBuffer<uint> gInput;
[[vk::binding(1, 1)]] RWStructuredBuffer<uint> gOutput;

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    gOutput[id.x] = gInput[id.x] * 2u + 1u;
}
