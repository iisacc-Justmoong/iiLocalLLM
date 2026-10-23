#include <ggml.h>
#include <ggml-backend.h>
#include <ggml-alloc.h>
#include <ggml-metal.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <type_traits>
#include <vector>

int main() {
    constexpr int k=128,m=128,experts=16,top=9,tokens=64;
    auto backend=ggml_backend_metal_init();
    if (!backend) { std::cerr << "No Metal backend\n"; return 77; }
    const auto cleanupBackend=std::unique_ptr<ggml_backend,std::decay_t<decltype(&ggml_backend_free)>>(backend,ggml_backend_free);
    auto* ctx=ggml_init({2*1024*1024,nullptr,true});
    if (!ctx) return 2;
    const auto cleanupContext=std::unique_ptr<ggml_context,decltype(&ggml_free)>(ctx,ggml_free);
    auto* a=ggml_new_tensor_3d(ctx,GGML_TYPE_F32,k,m,experts);
    auto* b=ggml_new_tensor_3d(ctx,GGML_TYPE_F32,k,1,tokens);
    auto* ids=ggml_new_tensor_2d(ctx,GGML_TYPE_I32,top,tokens);
    auto* out=ggml_mul_mat_id(ctx,a,b,ids);
    auto* graph=ggml_new_graph(ctx);
    ggml_build_forward_expand(graph,out);
    auto buffer=ggml_backend_alloc_ctx_tensors(ctx,backend);
    if (!buffer) { std::cerr << "No tensor buffer\n"; return 2; }
    const auto cleanupBuffer=std::unique_ptr<ggml_backend_buffer,decltype(&ggml_backend_buffer_free)>(buffer,ggml_backend_buffer_free);
    std::vector<float> weights(k*m*experts),input(k*tokens,1.0f),result(m*top*tokens);
    std::vector<int> selected(top*tokens);
    for (int e=0;e<experts;++e) std::fill_n(weights.begin()+e*k*m,k*m,float(e+1));
    for (int t=0;t<tokens;++t) for (int e=0;e<top;++e) selected[t*top+e]=(t+e)%experts;
    ggml_backend_tensor_set(a,weights.data(),0,weights.size()*sizeof(float));
    ggml_backend_tensor_set(b,input.data(),0,input.size()*sizeof(float));
    ggml_backend_tensor_set(ids,selected.data(),0,selected.size()*sizeof(int));
    std::cout << "COMPUTE_TOP9_BEGIN" << std::endl;
    if (ggml_backend_graph_compute(backend,graph)!=GGML_STATUS_SUCCESS) return 1;
    ggml_backend_tensor_get(out,result.data(),0,result.size()*sizeof(float));
    for (int t=0;t<tokens;++t) for (int e=0;e<top;++e) for (int row=0;row<m;++row) {
        const auto expected=float(k*(selected[t*top+e]+1));
        const auto actual=result[(t*top+e)*m+row];
        if (!std::isfinite(actual) || std::abs(actual-expected)>0.01f) {
            std::cerr << "Incorrect expert output: " << actual << " expected " << expected << '\n'; return 1;
        }
    }
    std::cout << "METAL_TOP9_PASS " << result.size() << " checked values" << std::endl;
    return 0;
}
