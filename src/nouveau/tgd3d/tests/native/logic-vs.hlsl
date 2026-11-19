float4 main(uint id : SV_VertexID) : SV_Position { float2 xy = float2((id << 1) & 2, id & 2); return float4(xy * 2 - 1, 0, 1); }
