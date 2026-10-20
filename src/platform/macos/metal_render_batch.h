#pragma once
#include <cstdint>
struct ID3D11DeviceContext;
namespace acfa::metal {
struct RenderBatchStats { uint64_t draws=0,encoders=0,reused=0,ends=0,query_draws=0,feedback_draws=0,pipeline_requests=0,pipeline_fast_reuses=0,pipeline_lookup_hits=0,pipeline_descriptor_builds=0,resource_binds=0,resource_reuses=0,depth_requests=0,depth_reuses=0,constant_requests=0,constant_unchanged=0,constant_allocations=0; };
// Per-context cumulative counters, read without submitting or waiting for work.
RenderBatchStats render_batch_stats(ID3D11DeviceContext*);
}
