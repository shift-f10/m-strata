// src/core/raon1_geometry.cpp - Raon1Geometry::from_gguf.  See include/strata/core/raon1_geometry.hpp.
//
// Mirrors Glm5Geometry::from_gguf (src/core/glm_model.cu) key-for-key where raon1 shares GLM's metadata, with
// raon1's additions: rope.dimension_count (the decoupled-RoPE dim), attention.latent_norm_rms_epsilon,
// ssm.time_step_rank, activation.situ_*, and hyper_connection.magnitude.  raon1 carries NO indexer.* keys.
#include "strata/core/raon1_geometry.hpp"

#include "strata/artifact/gguf_reader.hpp"

namespace strata::core {

bool Raon1Geometry::from_gguf(const strata::GgufFile& g, Raon1Geometry& out, std::string& err) {
    const std::string p = "raon1.";
    const auto num = [&](const std::string& key, double def) -> double {
        const auto* v = g.get(p + key);
        if (!v) return def;
        if (v->type == strata::MetaType::ARRAY) {
            if (v->items.empty() || !v->items[0].is_num()) return def;
            return v->items[0].num();
        }
        if (!v->is_num()) return def;
        return v->num();
    };
    const auto arr_i = [&](const std::string& key, std::vector<int>& dst, int n_layers) {
        const auto* v = g.get(p + key);
        if (v && v->type == strata::MetaType::ARRAY && v->items.size() >= (size_t) n_layers)
            for (int i = 0; i < n_layers; ++i) dst[(size_t) i] = (int) v->items[(size_t) i].u;
        else if (v && v->is_num())
            std::fill(dst.begin(), dst.end(), (int) v->u);
    };

    out.n_embd = (int) num("embedding_length", out.n_embd);
    out.n_layers = (int) num("block_count", out.n_layers);
    out.n_ctx_train = (int64_t) num("context_length", (double) out.n_ctx_train);
    out.n_head = (int) num("attention.head_count", out.n_head);

    // Gated-MLA latent dims.  key_length_mla is the FULL per-head q/k dim (nope + rope); the decoupled-RoPE
    // portion is rope.dimension_count, so qk_nope = qk - qk_rope.
    out.q_lora = (int) num("attention.q_lora_rank", out.q_lora);
    out.kv_lora = (int) num("attention.kv_lora_rank", out.kv_lora);
    out.qk = (int) num("attention.key_length_mla", out.qk);
    out.qk_rope = (int) num("rope.dimension_count", out.qk_rope);
    out.v_head = (int) num("attention.value_length_mla", out.v_head);
    out.latent_norm_eps = (float) num("attention.latent_norm_rms_epsilon", out.latent_norm_eps);

    // RDA (gated-delta SSM).
    out.kda_head_dim = (int) num("kda.head_dim", out.kda_head_dim);
    out.d_conv = (int) num("ssm.conv_kernel", out.d_conv);
    out.dt_rank = (int) num("ssm.time_step_rank", out.dt_rank);
    out.kda_lb = (float) num("kda.gate_lower_bound", out.kda_lb);

    // Hyper-connections.
    out.hc = (int) num("hyper_connection.count", out.hc);
    out.hc_eps = (float) num("hyper_connection.epsilon", out.hc_eps);
    out.hc_magnitude = (float) num("hyper_connection.magnitude", out.hc_magnitude);
    out.norm_eps = (float) num("attention.layer_norm_rms_epsilon", out.norm_eps);

    // MoE + dense FFN.
    out.n_expert = (int) num("expert_count", out.n_expert);
    out.n_exp_used = (int) num("expert_used_count", out.n_exp_used);
    out.n_ff_exp = (int) num("expert_feed_forward_length", out.n_ff_exp);
    out.n_shared = (int) num("expert_shared_count", out.n_shared);
    out.n_ff_dense = (int) num("feed_forward_length", out.n_ff_dense);
    out.dense_lead = (int) num("leading_dense_block_count", out.dense_lead);
    out.w_scale = (float) num("expert_weights_scale", out.w_scale);
    out.norm_w = (int) num("expert_weights_norm", out.norm_w);
    out.gating_func = (int) num("expert_gating_func", out.gating_func);

    // The `situ` activation.
    out.situ_beta = (float) num("activation.situ_beta", out.situ_beta);
    out.situ_linear_beta = (float) num("activation.situ_linear_beta", out.situ_linear_beta);

    if (const auto* v = g.get(p + "vocab_size")) out.n_vocab = (int) v->u;

    // The per-layer mixer array: 0 = RDA (recurrent), nonzero = Gated-MLA.
    out.n_head_kv.assign((size_t) out.n_layers, 0);
    arr_i("attention.head_count_kv", out.n_head_kv, out.n_layers);

    // Guards: the runner's kernels pin these.
    if (out.n_layers <= 0 || (int) out.n_head_kv.size() != out.n_layers) {
        err = "raon1_geometry: bad block_count / head_count_kv";
        return false;
    }
    if (out.gating_func != 2) {
        err = "raon1_geometry: expert_gating_func " + std::to_string(out.gating_func) +
              " is not sigmoid (2); raon1 routes with a sigmoid gate";
        return false;
    }
    if (out.hc != 4) {
        err = "raon1_geometry: hc != 4 is not supported (the mHC kernels pin 4 streams)";
        return false;
    }
    if (out.qk_nope() != out.v_head) {
        err = "raon1_geometry: qk_nope (" + std::to_string(out.qk_nope()) + ") != v_head (" +
              std::to_string(out.v_head) + "); the MLA head geometry is off";
        return false;
    }
    if (out.qk_rope < 0 || out.qk_rope >= out.qk) {
        err = "raon1_geometry: rope.dimension_count (" + std::to_string(out.qk_rope) +
              ") out of range for key_length_mla (" + std::to_string(out.qk) + ")";
        return false;
    }
    return true;
}

}  // namespace strata::core
