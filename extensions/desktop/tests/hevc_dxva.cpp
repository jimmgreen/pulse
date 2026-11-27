// hevc_dxva.cpp - see hevc_dxva.h. References: ITU-T H.265 (2013) clauses 7.3/7.4 (syntax), 8.3.1 (POC),
// 8.3.2 (RPS), C.5.2 (output and removal of pictures); the DXVA HEVC specification for the buffers.
#include "hevc_dxva.h"

#include <dxva.h>

#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace hevc {
namespace {

// Table 7-6, coded order.
constexpr uint8_t kDefaultIntra[64] = {16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 16, 17, 16, 17, 18,
                                       17, 18, 18, 17, 18, 21, 19, 20, 21, 20, 19, 21, 24, 22, 22, 24,
                                       24, 22, 22, 24, 25, 25, 27, 30, 27, 25, 25, 29, 31, 35, 35, 31,
                                       29, 36, 41, 44, 41, 36, 47, 54, 54, 47, 65, 70, 65, 88, 88, 115};
constexpr uint8_t kDefaultInter[64] = {16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17, 17, 17, 18,
                                       18, 18, 18, 18, 18, 20, 20, 20, 20, 20, 20, 20, 24, 24, 24, 24,
                                       24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 28, 28, 28, 28, 28,
                                       28, 33, 33, 33, 33, 33, 41, 41, 41, 41, 54, 54, 54, 71, 71, 91};

// Reads an RBSP (emulation-prevention bytes removed) MSB first; reads past the end return zeros.
class Bits {
public:
    Bits(const uint8_t* nal, size_t size, size_t skip) {
        rbsp_.reserve(size);
        int zeros = 0;
        for (size_t i = skip; i < size; ++i) {
            if (zeros >= 2 && nal[i] == 3) {
                zeros = 0;
                continue;
            }
            zeros = nal[i] == 0 ? zeros + 1 : 0;
            rbsp_.push_back(nal[i]);
        }
    }
    uint32_t U(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 1) | Bit();
        return v;
    }
    bool Flag() { return Bit() != 0; }
    uint32_t Ue() {
        int lz = 0;
        while (!Bit() && lz < 32) ++lz;
        if (lz == 0) return 0;
        return ((1u << lz) - 1u) + U(lz);
    }
    int32_t Se() {
        const uint32_t k = Ue();
        return (k & 1) ? static_cast<int32_t>((k + 1) / 2) : -static_cast<int32_t>(k / 2);
    }
    void Skip(int n) { pos_ += static_cast<size_t>(n); }
    size_t Pos() const { return pos_; }
    bool Overrun() const { return pos_ > rbsp_.size() * 8; }

private:
    uint32_t Bit() {
        const size_t byte = pos_ >> 3;
        const uint32_t b = byte < rbsp_.size() ? (rbsp_[byte] >> (7 - (pos_ & 7))) & 1u : 0u;
        ++pos_;
        return b;
    }
    std::vector<uint8_t> rbsp_;
    size_t pos_ = 0;
};

int CeilLog2(int v) {
    int r = 0;
    while ((1 << r) < v) ++r;
    return r;
}

void SkipProfileTierLevel(Bits& b, int max_sub_layers_minus1) {
    b.Skip(88);  // general profile space/tier/idc, 32 compatibility flags, 4 constraint flags, 43 + 1 bits
    b.Skip(8);   // general_level_idc
    bool prof[8] = {}, lev[8] = {};
    for (int i = 0; i < max_sub_layers_minus1; ++i) {
        prof[i] = b.Flag();
        lev[i] = b.Flag();
    }
    if (max_sub_layers_minus1 > 0)
        for (int i = max_sub_layers_minus1; i < 8; ++i) b.Skip(2);
    for (int i = 0; i < max_sub_layers_minus1; ++i) {
        if (prof[i]) b.Skip(88);
        if (lev[i]) b.Skip(8);
    }
}

void DefaultScalingList(ScalingList& s) {
    for (int m = 0; m < 6; ++m) {
        std::memset(s.sl[0][m], 16, 16);
        for (int size = 1; size < 4; ++size) {
            std::memcpy(s.sl[size][m], m < 3 ? kDefaultIntra : kDefaultInter, 64);
            s.dc[size][m] = 16;
        }
    }
    // sizeId 3 has two matrices in version 1: 0 = intra, 1 = inter.
    std::memcpy(s.sl[3][1], kDefaultInter, 64);
}

void ParseScalingListData(Bits& b, ScalingList& s) {
    for (int size = 0; size < 4; ++size) {
        const int matrices = size == 3 ? 2 : 6;
        const int coefs = std::min(64, 1 << (4 + (size << 1)));
        for (int m = 0; m < matrices; ++m) {
            const bool intra = size == 3 ? m == 0 : m < 3;
            if (!b.Flag()) {  // scaling_list_pred_mode_flag == 0
                const int delta = static_cast<int>(b.Ue());
                if (delta == 0) {
                    if (size == 0) std::memset(s.sl[0][m], 16, 16);
                    else std::memcpy(s.sl[size][m], intra ? kDefaultIntra : kDefaultInter, 64);
                    s.dc[size][m] = 16;
                } else {
                    const int ref = std::max(0, m - delta);
                    std::memcpy(s.sl[size][m], s.sl[size][ref], 64);
                    s.dc[size][m] = s.dc[size][ref];
                }
            } else {
                int next = 8;
                if (size > 1) {
                    next = b.Se() + 8;
                    s.dc[size][m] = static_cast<uint8_t>(next);
                }
                for (int i = 0; i < coefs; ++i) {
                    next = (next + b.Se() + 256) % 256;
                    s.sl[size][m][i] = static_cast<uint8_t>(next);
                }
                if (size <= 1) s.dc[size][m] = 16;
            }
        }
    }
}

// 7.3.7 st_ref_pic_set(idx); sets[] holds the SPS sets parsed so far. Returns false on bad data.
bool ParseStRps(Bits& b, StRps& out, int idx, const StRps* sets, int num_sets, bool in_slice, int* ref_num_delta) {
    out = StRps{};
    if (ref_num_delta) *ref_num_delta = 0;
    const bool predict = idx != 0 && b.Flag();
    if (predict) {
        const int delta_idx = in_slice ? static_cast<int>(b.Ue()) + 1 : 1;
        const int ref_idx = idx - delta_idx;
        if (ref_idx < 0 || ref_idx >= num_sets) return false;
        const StRps& ref = sets[ref_idx];
        if (ref_num_delta) *ref_num_delta = ref.NumDelta();
        const int sign = static_cast<int>(b.U(1));
        const int delta_rps = (1 - 2 * sign) * (static_cast<int>(b.Ue()) + 1);
        bool used[33] = {}, use_delta[33] = {};
        for (int j = 0; j <= ref.NumDelta(); ++j) {
            used[j] = b.Flag();
            use_delta[j] = used[j] ? true : b.Flag();
        }
        int i = 0;
        for (int j = ref.num_pos - 1; j >= 0; --j) {
            const int d = ref.delta_s1[j] + delta_rps;
            if (d < 0 && use_delta[ref.num_neg + j] && i < 16) {
                out.delta_s0[i] = d;
                out.used_s0[i++] = used[ref.num_neg + j];
            }
        }
        if (delta_rps < 0 && use_delta[ref.NumDelta()] && i < 16) {
            out.delta_s0[i] = delta_rps;
            out.used_s0[i++] = used[ref.NumDelta()];
        }
        for (int j = 0; j < ref.num_neg; ++j) {
            const int d = ref.delta_s0[j] + delta_rps;
            if (d < 0 && use_delta[j] && i < 16) {
                out.delta_s0[i] = d;
                out.used_s0[i++] = used[j];
            }
        }
        out.num_neg = i;
        i = 0;
        for (int j = ref.num_neg - 1; j >= 0; --j) {
            const int d = ref.delta_s0[j] + delta_rps;
            if (d > 0 && use_delta[j] && i < 16) {
                out.delta_s1[i] = d;
                out.used_s1[i++] = used[j];
            }
        }
        if (delta_rps > 0 && use_delta[ref.NumDelta()] && i < 16) {
            out.delta_s1[i] = delta_rps;
            out.used_s1[i++] = used[ref.NumDelta()];
        }
        for (int j = 0; j < ref.num_pos; ++j) {
            const int d = ref.delta_s1[j] + delta_rps;
            if (d > 0 && use_delta[ref.num_neg + j] && i < 16) {
                out.delta_s1[i] = d;
                out.used_s1[i++] = used[ref.num_neg + j];
            }
        }
        out.num_pos = i;
        return true;
    }
    out.num_neg = static_cast<int>(b.Ue());
    out.num_pos = static_cast<int>(b.Ue());
    if (out.num_neg > 16 || out.num_pos > 16) return false;
    int prev = 0;
    for (int i = 0; i < out.num_neg; ++i) {
        prev -= static_cast<int>(b.Ue()) + 1;
        out.delta_s0[i] = prev;
        out.used_s0[i] = b.Flag();
    }
    prev = 0;
    for (int i = 0; i < out.num_pos; ++i) {
        prev += static_cast<int>(b.Ue()) + 1;
        out.delta_s1[i] = prev;
        out.used_s1[i] = b.Flag();
    }
    return true;
}

bool IsIrap(int t) { return t >= 16 && t <= 23; }
bool IsIdr(int t) { return t == 19 || t == 20; }
bool IsBla(int t) { return t >= 16 && t <= 18; }
bool IsRasl(int t) { return t == 8 || t == 9; }
bool IsRadl(int t) { return t == 6 || t == 7; }
bool IsSubLayerNonRef(int t) { return t <= 14 && (t % 2) == 0; }

}  // namespace

bool Decoder::Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, int extra_surfaces, std::string& err) {
    dev_ = dev;
    ctx_ = ctx;
    extra_ = std::max(1, extra_surfaces);  // 0 stalls: the frame output but not yet shown lives outside the DPB
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&vdev_))) || FAILED(ctx->QueryInterface(IID_PPV_ARGS(&vctx_)))) {
        err = "no D3D11 video device";
        return false;
    }
    return true;
}

void Decoder::SplitNals(const uint8_t* d, size_t n, std::vector<Nal>& out) {
    auto push = [&](const uint8_t* p, size_t len) {
        while (len > 0 && p[len - 1] == 0) --len;  // trailing_zero_8bits
        if (len < 2) return;
        out.push_back({p, len, (p[0] >> 1) & 0x3F, (p[1] & 7) - 1});
    };
    const bool annexb = n >= 3 && d[0] == 0 && d[1] == 0 && (d[2] == 1 || (n >= 4 && d[2] == 0 && d[3] == 1));
    if (annexb) {
        size_t i = 0, start = SIZE_MAX;
        while (i + 2 < n) {
            if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
                if (start != SIZE_MAX) push(d + start, i - start);
                i += 3;
                start = i;
            } else {
                ++i;
            }
        }
        if (start != SIZE_MAX && start < n) push(d + start, n - start);
        return;
    }
    size_t i = 0;  // 4-byte length prefixes (hvcC samples)
    while (i + 4 <= n) {
        const size_t len = (static_cast<size_t>(d[i]) << 24) | (static_cast<size_t>(d[i + 1]) << 16) |
                           (static_cast<size_t>(d[i + 2]) << 8) | d[i + 3];
        i += 4;
        if (len == 0 || i + len > n) break;
        push(d + i, len);
        i += len;
    }
}

void Decoder::ParseConfig(const uint8_t* data, size_t size) {
    std::vector<Nal> nals;
    SplitNals(data, size, nals);
    for (const auto& n : nals) {
        if (n.type == 33) ParseSps(n);
        else if (n.type == 34) ParsePps(n);
    }
}

void Decoder::ParseSps(const Nal& n) {
    Bits b(n.data, n.size, 2);
    Sps s;
    b.Skip(4);  // sps_video_parameter_set_id
    s.max_sub_layers_minus1 = static_cast<int>(b.U(3));
    b.Skip(1);
    SkipProfileTierLevel(b, s.max_sub_layers_minus1);
    const uint32_t id = b.Ue();
    if (id >= 16) return;
    s.chroma_format_idc = static_cast<int>(b.Ue());
    if (s.chroma_format_idc == 3) s.separate_colour_plane = b.Flag();
    s.width = static_cast<int>(b.Ue());
    s.height = static_cast<int>(b.Ue());
    if (b.Flag()) {
        s.conf_left = static_cast<int>(b.Ue());
        s.conf_right = static_cast<int>(b.Ue());
        s.conf_top = static_cast<int>(b.Ue());
        s.conf_bottom = static_cast<int>(b.Ue());
    }
    s.bit_depth_luma = static_cast<int>(b.Ue()) + 8;
    s.bit_depth_chroma = static_cast<int>(b.Ue()) + 8;
    s.log2_max_poc_lsb = static_cast<int>(b.Ue()) + 4;
    const bool ordering_all = b.Flag();
    for (int i = ordering_all ? 0 : s.max_sub_layers_minus1; i <= s.max_sub_layers_minus1; ++i) {
        s.max_dec_pic_buffering[i] = static_cast<int>(b.Ue()) + 1;
        s.max_num_reorder[i] = static_cast<int>(b.Ue());
        b.Ue();  // max_latency_increase_plus1
    }
    if (!ordering_all)
        for (int i = 0; i < s.max_sub_layers_minus1; ++i) {
            s.max_dec_pic_buffering[i] = s.max_dec_pic_buffering[s.max_sub_layers_minus1];
            s.max_num_reorder[i] = s.max_num_reorder[s.max_sub_layers_minus1];
        }
    s.log2_min_cb = static_cast<int>(b.Ue()) + 3;
    s.log2_diff_max_min_cb = static_cast<int>(b.Ue());
    s.log2_min_tb = static_cast<int>(b.Ue()) + 2;
    s.log2_diff_max_min_tb = static_cast<int>(b.Ue());
    s.max_th_depth_inter = static_cast<int>(b.Ue());
    s.max_th_depth_intra = static_cast<int>(b.Ue());
    s.scaling_list_enabled = b.Flag();
    if (s.scaling_list_enabled) {
        DefaultScalingList(s.sl);
        if (b.Flag()) ParseScalingListData(b, s.sl);
    }
    s.amp = b.Flag();
    s.sao = b.Flag();
    s.pcm = b.Flag();
    if (s.pcm) {
        s.pcm_bd_luma = static_cast<int>(b.U(4)) + 1;
        s.pcm_bd_chroma = static_cast<int>(b.U(4)) + 1;
        s.log2_min_pcm = static_cast<int>(b.Ue()) + 3;
        s.log2_diff_max_min_pcm = static_cast<int>(b.Ue());
        s.pcm_loop_filter_disabled = b.Flag();
    }
    s.num_st_rps = static_cast<int>(b.Ue());
    if (s.num_st_rps > 64) return;
    for (int i = 0; i < s.num_st_rps; ++i)
        if (!ParseStRps(b, s.st_rps[i], i, s.st_rps, i, false, nullptr)) return;
    s.long_term_present = b.Flag();
    if (s.long_term_present) {
        s.num_lt_sps = static_cast<int>(b.Ue());
        if (s.num_lt_sps > 32) return;
        for (int i = 0; i < s.num_lt_sps; ++i) {
            s.lt_poc_lsb_sps[i] = static_cast<int>(b.U(s.log2_max_poc_lsb));
            s.lt_used_sps[i] = b.Flag();
        }
    }
    s.temporal_mvp = b.Flag();
    s.strong_intra_smoothing = b.Flag();
    if (b.Overrun() || s.width <= 0 || s.height <= 0) return;
    s.valid = true;
    sps_[id] = s;
}

void Decoder::ParsePps(const Nal& n) {
    Bits b(n.data, n.size, 2);
    Pps p;
    const uint32_t id = b.Ue();
    if (id >= 64) return;
    p.sps_id = static_cast<int>(b.Ue());
    p.dependent_slices = b.Flag();
    p.output_flag_present = b.Flag();
    p.num_extra_slice_header_bits = static_cast<int>(b.U(3));
    p.sign_data_hiding = b.Flag();
    p.cabac_init_present = b.Flag();
    p.num_ref_idx_l0_default_minus1 = static_cast<int>(b.Ue());
    p.num_ref_idx_l1_default_minus1 = static_cast<int>(b.Ue());
    p.init_qp_minus26 = b.Se();
    p.constrained_intra_pred = b.Flag();
    p.transform_skip = b.Flag();
    p.cu_qp_delta_enabled = b.Flag();
    if (p.cu_qp_delta_enabled) p.diff_cu_qp_delta_depth = static_cast<int>(b.Ue());
    p.cb_qp_offset = b.Se();
    p.cr_qp_offset = b.Se();
    p.slice_chroma_qp_offsets_present = b.Flag();
    p.weighted_pred = b.Flag();
    p.weighted_bipred = b.Flag();
    p.transquant_bypass = b.Flag();
    p.tiles = b.Flag();
    p.entropy_sync = b.Flag();
    if (p.tiles) {
        p.num_tile_columns_minus1 = static_cast<int>(b.Ue());
        p.num_tile_rows_minus1 = static_cast<int>(b.Ue());
        if (p.num_tile_columns_minus1 > 18 || p.num_tile_rows_minus1 > 20) return;
        p.uniform_spacing = b.Flag();
        if (!p.uniform_spacing) {
            for (int i = 0; i < p.num_tile_columns_minus1; ++i) p.column_width_minus1[i] = static_cast<int>(b.Ue());
            for (int i = 0; i < p.num_tile_rows_minus1; ++i) p.row_height_minus1[i] = static_cast<int>(b.Ue());
        }
        p.loop_filter_across_tiles = b.Flag();
    }
    p.loop_filter_across_slices = b.Flag();
    if (b.Flag()) {  // deblocking_filter_control_present_flag
        p.deblocking_override_enabled = b.Flag();
        p.deblocking_disabled = b.Flag();
        if (!p.deblocking_disabled) {
            p.beta_offset_div2 = b.Se();
            p.tc_offset_div2 = b.Se();
        }
    }
    p.has_sl = b.Flag();
    if (p.has_sl) {
        DefaultScalingList(p.sl);
        ParseScalingListData(b, p.sl);
    }
    p.lists_modification_present = b.Flag();
    p.log2_parallel_merge_level_minus2 = static_cast<int>(b.Ue());
    p.slice_header_extension_present = b.Flag();
    if (b.Overrun()) return;
    p.valid = true;
    pps_[id] = p;
}

bool Decoder::ParseSliceHeader(const Nal& n, Slice& s) {
    Bits b(n.data, n.size, 2);
    s.nal_type = n.type;
    s.tid = n.tid;
    s.first = b.Flag();
    if (IsIrap(n.type)) b.Skip(1);  // no_output_of_prior_pics_flag
    s.pps_id = static_cast<int>(b.Ue());
    if (s.pps_id >= 64 || !pps_[s.pps_id].valid) return false;
    const Pps& pps = pps_[s.pps_id];
    if (pps.sps_id >= 16 || !sps_[pps.sps_id].valid) return false;
    const Sps& sps = sps_[pps.sps_id];
    if (!s.first) {
        if (pps.dependent_slices) s.dependent = b.Flag();
        const int ctb = 1 << (sps.log2_min_cb + sps.log2_diff_max_min_cb);
        const int ctbs = ((sps.width + ctb - 1) / ctb) * ((sps.height + ctb - 1) / ctb);
        b.Skip(CeilLog2(ctbs));  // slice_segment_address
    }
    if (s.dependent || !s.first) return true;  // picture-level fields come from the first slice only
    b.Skip(pps.num_extra_slice_header_bits);
    s.slice_type = static_cast<int>(b.Ue());
    if (pps.output_flag_present) s.pic_output = b.Flag();
    if (sps.separate_colour_plane) b.Skip(2);
    if (!IsIdr(n.type)) {
        s.poc_lsb = static_cast<int>(b.U(sps.log2_max_poc_lsb));
        s.st_rps_sps = b.Flag();
        if (!s.st_rps_sps) {
            const size_t before = b.Pos();
            if (!ParseStRps(b, s.st_rps, sps.num_st_rps, sps.st_rps, sps.num_st_rps, true, &s.ref_rps_num_delta))
                return false;
            s.st_rps_bits = static_cast<int>(b.Pos() - before);
        } else {
            if (sps.num_st_rps > 1) s.st_rps_idx = static_cast<int>(b.U(CeilLog2(sps.num_st_rps)));
            if (s.st_rps_idx >= sps.num_st_rps) return false;
            s.st_rps = sps.st_rps[s.st_rps_idx];
        }
        if (sps.long_term_present) {
            int num_lt_sps = 0;
            if (sps.num_lt_sps > 0) num_lt_sps = static_cast<int>(b.Ue());
            const int num_lt_pics = static_cast<int>(b.Ue());
            s.num_lt = std::min(32, num_lt_sps + num_lt_pics);
            int msb_cycle = 0;
            for (int i = 0; i < s.num_lt; ++i) {
                if (i < num_lt_sps) {
                    int idx = 0;
                    if (sps.num_lt_sps > 1) idx = static_cast<int>(b.U(CeilLog2(sps.num_lt_sps)));
                    s.lt_poc[i] = sps.lt_poc_lsb_sps[idx];
                    s.lt_used[i] = sps.lt_used_sps[idx];
                } else {
                    s.lt_poc[i] = static_cast<int>(b.U(sps.log2_max_poc_lsb));
                    s.lt_used[i] = b.Flag();
                }
                s.lt_msb_present[i] = b.Flag();
                if (s.lt_msb_present[i]) {
                    const int d = static_cast<int>(b.Ue());
                    msb_cycle = (i == 0 || i == num_lt_sps) ? d : msb_cycle + d;
                    // Stored as the msb cycle; resolved against the current POC in DecodePicture.
                    s.lt_poc[i] = s.lt_poc[i] - msb_cycle * (1 << sps.log2_max_poc_lsb) - (1 << 30);
                }
            }
        }
    }
    return !b.Overrun();
}

bool Decoder::CreateDecoder(const Sps& sps, std::string& err) {
    decoder_.Reset();
    texture_.Reset();
    views_.clear();
    out_queue_.clear();
    const bool ten = sps.bit_depth_luma > 8;
    profile_ = ten ? D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10 : D3D11_DECODER_PROFILE_HEVC_VLD_MAIN;
    format_ = ten ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    BOOL ok = FALSE;
    if (FAILED(vdev_->CheckVideoDecoderFormat(&profile_, format_, &ok)) || !ok) {
        err = ten ? "GPU has no HEVC Main10 decoding" : "GPU has no HEVC Main decoding";
        return false;
    }
    D3D11_VIDEO_DECODER_DESC desc{};
    desc.Guid = profile_;
    desc.SampleWidth = static_cast<UINT>(sps.width);
    desc.SampleHeight = static_cast<UINT>(sps.height);
    desc.OutputFormat = format_;
    UINT configs = 0;
    vdev_->GetVideoDecoderConfigCount(&desc, &configs);
    D3D11_VIDEO_DECODER_CONFIG chosen{};
    bool found = false;
    for (UINT i = 0; i < configs && !found; ++i) {
        D3D11_VIDEO_DECODER_CONFIG cfg{};
        if (SUCCEEDED(vdev_->GetVideoDecoderConfig(&desc, i, &cfg)) && cfg.ConfigBitstreamRaw == 1) {
            chosen = cfg;
            found = true;
        }
    }
    if (!found) {
        err = "decoder has no short-slice (ConfigBitstreamRaw=1) configuration";
        return false;
    }
    HRESULT hr = vdev_->CreateVideoDecoder(&desc, &chosen, &decoder_);
    if (FAILED(hr)) {
        err = "CreateVideoDecoder failed";
        return false;
    }
    // The pool: what the stream says it needs, plus the frame being shown.
    dpb_size_ = sps.max_dec_pic_buffering[sps.max_sub_layers_minus1];
    const int count = dpb_size_ + extra_;
    coded_w_ = (static_cast<UINT>(sps.width) + 15u) & ~15u;
    coded_h_ = (static_cast<UINT>(sps.height) + 15u) & ~15u;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = coded_w_;
    td.Height = coded_h_;
    td.MipLevels = 1;
    td.ArraySize = static_cast<UINT>(count);
    td.Format = format_;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DECODER;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &texture_))) {
        err = "decoder texture array";
        return false;
    }
    views_.resize(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC vd{};
        vd.DecodeProfile = profile_;
        vd.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
        vd.Texture2D.ArraySlice = static_cast<UINT>(i);
        if (FAILED(vdev_->CreateVideoDecoderOutputView(texture_.Get(), &vd, &views_[static_cast<size_t>(i)]))) {
            err = "decoder output view";
            return false;
        }
    }
    pics_.assign(static_cast<size_t>(count), Pic{});
    const int sub_w = sps.chroma_format_idc == 1 || sps.chroma_format_idc == 2 ? 2 : 1;
    const int sub_h = sps.chroma_format_idc == 1 ? 2 : 1;
    crop_ = {sps.conf_left * sub_w, sps.conf_top * sub_h, sps.width - sps.conf_right * sub_w,
             sps.height - sps.conf_bottom * sub_h};
    return true;
}

int Decoder::CountNeededOutput() const {
    int n = 0;
    for (const auto& p : pics_) n += p.needed_output ? 1 : 0;
    return n;
}

void Decoder::Bump() {
    int best = -1;
    for (size_t i = 0; i < pics_.size(); ++i)
        if (pics_[i].needed_output && (best < 0 || pics_[i].poc < pics_[static_cast<size_t>(best)].poc)) best = static_cast<int>(i);
    if (best < 0) return;
    pics_[static_cast<size_t>(best)].needed_output = false;
    pics_[static_cast<size_t>(best)].queued = true;
    out_queue_.push_back(best);
}

bool Decoder::DecodeAccessUnit(const uint8_t* data, size_t size, int64_t pts, std::string& err) {
    std::vector<Nal> nals, slices;
    SplitNals(data, size, nals);
    Slice first;
    bool have_first = false;
    for (const auto& n : nals) {
        if ((n.data[0] & 0x01) || (n.data[1] >> 3)) continue;  // nuh_layer_id > 0
        if (n.type == 33) ParseSps(n);
        else if (n.type == 34) ParsePps(n);
        else if (n.type == 36 || n.type == 37) no_rasl_next_ = true;  // end of sequence / bitstream
        else if (n.type <= 21 && (n.type <= 9 || n.type >= 16)) {
            if (!have_first) {
                Slice s;
                if (!ParseSliceHeader(n, s) || !s.first) continue;  // not decodable yet (no parameter sets)
                first = s;
                have_first = true;
            }
            slices.push_back(n);
        }
    }
    if (!have_first) return true;
    return DecodePicture(slices, first, pts, err);
}

bool Decoder::DecodePicture(const std::vector<Nal>& slices, const Slice& sh, int64_t pts, std::string& err) {
    const Pps& pps = pps_[sh.pps_id];
    const Sps& sps = sps_[pps.sps_id];
    const int htid = sps.max_sub_layers_minus1;
    if (!texture_ || active_sps_ != pps.sps_id || coded_w_ != ((static_cast<UINT>(sps.width) + 15u) & ~15u) ||
        coded_h_ != ((static_cast<UINT>(sps.height) + 15u) & ~15u)) {
        if (!CreateDecoder(sps, err)) return false;
        active_sps_ = pps.sps_id;
        no_rasl_next_ = true;
    }
    const bool irap = IsIrap(sh.nal_type), idr = IsIdr(sh.nal_type);
    bool no_rasl = false;
    if (irap) {
        no_rasl = idr || IsBla(sh.nal_type) || no_rasl_next_;
        assoc_irap_no_rasl_ = no_rasl;
        no_rasl_next_ = false;
    } else if (no_rasl_next_) {
        ++skipped_;  // nothing decodable before the first IRAP
        return true;
    }
    if (IsRasl(sh.nal_type) && assoc_irap_no_rasl_) {
        ++skipped_;  // its leading references are gone (stream start, seek, loop)
        return true;
    }

    // 8.3.1 picture order count
    const int max_lsb = 1 << sps.log2_max_poc_lsb;
    int msb = 0;
    if (!(irap && no_rasl)) {
        const int prev_lsb = prev_tid0_poc_ & (max_lsb - 1);
        const int prev_msb = prev_tid0_poc_ - prev_lsb;
        if (sh.poc_lsb < prev_lsb && prev_lsb - sh.poc_lsb >= max_lsb / 2) msb = prev_msb + max_lsb;
        else if (sh.poc_lsb > prev_lsb && sh.poc_lsb - prev_lsb > max_lsb / 2) msb = prev_msb - max_lsb;
        else msb = prev_msb;
    }
    const int poc = msb + (idr ? 0 : sh.poc_lsb);

    // C.5.2.2: an IRAP that starts a new sequence outputs everything still waiting, drops all references.
    if (irap && no_rasl) {
        while (CountNeededOutput() > 0) Bump();
        for (auto& p : pics_) p.short_ref = p.long_ref = false;
    }

    // 8.3.2 reference picture set
    std::vector<int> st_before, st_after, st_foll, lt_curr, lt_foll;
    std::vector<bool> lt_curr_full, lt_foll_full;
    if (!idr) {
        for (int i = 0; i < sh.st_rps.num_neg; ++i)
            (sh.st_rps.used_s0[i] ? st_before : st_foll).push_back(poc + sh.st_rps.delta_s0[i]);
        for (int i = 0; i < sh.st_rps.num_pos; ++i)
            (sh.st_rps.used_s1[i] ? st_after : st_foll).push_back(poc + sh.st_rps.delta_s1[i]);
        for (int i = 0; i < sh.num_lt; ++i) {
            int lt = sh.lt_poc[i];
            const bool full = sh.lt_msb_present[i];
            if (full) lt = (lt + (1 << 30)) + poc - (poc & (max_lsb - 1));
            if (sh.lt_used[i]) {
                lt_curr.push_back(lt);
                lt_curr_full.push_back(full);
            } else {
                lt_foll.push_back(lt);
                lt_foll_full.push_back(full);
            }
        }
    }
    auto find = [&](int want, bool full_poc) -> int {
        for (size_t i = 0; i < pics_.size(); ++i) {
            const Pic& p = pics_[i];
            if (!(p.short_ref || p.long_ref)) continue;
            if (full_poc ? p.poc == want : (p.poc & (max_lsb - 1)) == want) return static_cast<int>(i);
        }
        return -1;
    };
    std::vector<int> idx_before, idx_after, idx_lt;
    std::vector<bool> keep_short(pics_.size(), false), keep_long(pics_.size(), false);
    for (size_t i = 0; i < lt_curr.size(); ++i) {
        const int f = find(lt_curr[i], lt_curr_full[i]);
        idx_lt.push_back(f);
        if (f >= 0) keep_long[static_cast<size_t>(f)] = true;
    }
    for (size_t i = 0; i < lt_foll.size(); ++i)
        if (const int f = find(lt_foll[i], lt_foll_full[i]); f >= 0) keep_long[static_cast<size_t>(f)] = true;
    for (int want : st_before) {
        const int f = find(want, true);
        idx_before.push_back(f);
        if (f >= 0) keep_short[static_cast<size_t>(f)] = true;
    }
    for (int want : st_after) {
        const int f = find(want, true);
        idx_after.push_back(f);
        if (f >= 0) keep_short[static_cast<size_t>(f)] = true;
    }
    for (int want : st_foll)
        if (const int f = find(want, true); f >= 0) keep_short[static_cast<size_t>(f)] = true;
    for (size_t i = 0; i < pics_.size(); ++i) {
        pics_[i].long_ref = keep_long[i];
        pics_[i].short_ref = keep_short[i] && !keep_long[i];
    }
    const bool intra_only = sh.slice_type == 2 && slices.size() == 1;
    const bool missing = std::any_of(idx_before.begin(), idx_before.end(), [](int v) { return v < 0; }) ||
                         std::any_of(idx_after.begin(), idx_after.end(), [](int v) { return v < 0; }) ||
                         std::any_of(idx_lt.begin(), idx_lt.end(), [](int v) { return v < 0; });
    if (missing && !intra_only && !irap) {
        ++skipped_;  // a reference this picture predicts from is not in the DPB
        return true;
    }

    // C.5.2.2 "bumping" until the reorder limit holds and a surface is free for the current picture.
    auto free_surface = [&]() {
        for (size_t i = 0; i < pics_.size(); ++i)
            if (pics_[i].Free()) return static_cast<int>(i);
        return -1;
    };
    while (CountNeededOutput() > sps.max_num_reorder[htid]) Bump();
    int surface = free_surface();
    while (surface < 0 && CountNeededOutput() > 0) {
        Bump();
        surface = free_surface();
    }
    if (surface < 0) {
        err = "no free decoder surface";
        return false;
    }

    auto to_list = [](const std::vector<int>& v) {
        std::vector<int> out;
        for (int i : v) out.push_back(i < 0 ? 0xFF : i);
        return out;
    };
    if (!Submit(slices, sh, sps, pps, surface, poc, irap, idr, to_list(idx_before), to_list(idx_after), to_list(idx_lt), err))
        return false;
    Pic& cur = pics_[static_cast<size_t>(surface)];
    cur = Pic{};
    cur.in_use = true;
    cur.short_ref = true;
    cur.needed_output = sh.pic_output;
    cur.poc = poc;
    cur.pts = pts;
    ++decoded_;
    // C.5.2.3: output once more pictures wait than the stream allows to be reordered.
    while (CountNeededOutput() > sps.max_num_reorder[htid]) Bump();
    if (sh.tid == 0 && !IsRasl(sh.nal_type) && !IsRadl(sh.nal_type) && !IsSubLayerNonRef(sh.nal_type)) prev_tid0_poc_ = poc;
    return true;
}

bool Decoder::Submit(const std::vector<Nal>& slices, const Slice& sh, const Sps& sps, const Pps& pps, int surface, int poc,
                     bool irap, bool idr, const std::vector<int>& st_before, const std::vector<int>& st_after,
                     const std::vector<int>& lt_curr, std::string& err) {
    DXVA_PicParams_HEVC pp{};
    const int min_cb = 1 << sps.log2_min_cb;
    pp.PicWidthInMinCbsY = static_cast<USHORT>(sps.width / min_cb);
    pp.PicHeightInMinCbsY = static_cast<USHORT>(sps.height / min_cb);
    pp.chroma_format_idc = static_cast<USHORT>(sps.chroma_format_idc);
    pp.separate_colour_plane_flag = sps.separate_colour_plane;
    pp.bit_depth_luma_minus8 = static_cast<USHORT>(sps.bit_depth_luma - 8);
    pp.bit_depth_chroma_minus8 = static_cast<USHORT>(sps.bit_depth_chroma - 8);
    pp.log2_max_pic_order_cnt_lsb_minus4 = static_cast<USHORT>(sps.log2_max_poc_lsb - 4);
    pp.NoPicReorderingFlag = sps.max_num_reorder[sps.max_sub_layers_minus1] == 0;
    pp.NoBiPredFlag = 0;
    pp.CurrPic.Index7Bits = static_cast<UCHAR>(surface);
    pp.sps_max_dec_pic_buffering_minus1 = static_cast<UCHAR>(sps.max_dec_pic_buffering[sps.max_sub_layers_minus1] - 1);
    pp.log2_min_luma_coding_block_size_minus3 = static_cast<UCHAR>(sps.log2_min_cb - 3);
    pp.log2_diff_max_min_luma_coding_block_size = static_cast<UCHAR>(sps.log2_diff_max_min_cb);
    pp.log2_min_transform_block_size_minus2 = static_cast<UCHAR>(sps.log2_min_tb - 2);
    pp.log2_diff_max_min_transform_block_size = static_cast<UCHAR>(sps.log2_diff_max_min_tb);
    pp.max_transform_hierarchy_depth_inter = static_cast<UCHAR>(sps.max_th_depth_inter);
    pp.max_transform_hierarchy_depth_intra = static_cast<UCHAR>(sps.max_th_depth_intra);
    pp.num_short_term_ref_pic_sets = static_cast<UCHAR>(sps.num_st_rps);
    pp.num_long_term_ref_pics_sps = static_cast<UCHAR>(sps.num_lt_sps);
    pp.num_ref_idx_l0_default_active_minus1 = static_cast<UCHAR>(pps.num_ref_idx_l0_default_minus1);
    pp.num_ref_idx_l1_default_active_minus1 = static_cast<UCHAR>(pps.num_ref_idx_l1_default_minus1);
    pp.init_qp_minus26 = static_cast<CHAR>(pps.init_qp_minus26);
    if (!idr && !sh.st_rps_sps) {
        pp.ucNumDeltaPocsOfRefRpsIdx = static_cast<UCHAR>(sh.ref_rps_num_delta);
        pp.wNumBitsForShortTermRPSInSlice = static_cast<USHORT>(sh.st_rps_bits);
    }
    pp.scaling_list_enabled_flag = sps.scaling_list_enabled;
    pp.amp_enabled_flag = sps.amp;
    pp.sample_adaptive_offset_enabled_flag = sps.sao;
    pp.pcm_enabled_flag = sps.pcm;
    if (sps.pcm) {
        pp.pcm_sample_bit_depth_luma_minus1 = static_cast<UINT32>(sps.pcm_bd_luma - 1);
        pp.pcm_sample_bit_depth_chroma_minus1 = static_cast<UINT32>(sps.pcm_bd_chroma - 1);
        pp.log2_min_pcm_luma_coding_block_size_minus3 = static_cast<UINT32>(sps.log2_min_pcm - 3);
        pp.log2_diff_max_min_pcm_luma_coding_block_size = static_cast<UINT32>(sps.log2_diff_max_min_pcm);
        pp.pcm_loop_filter_disabled_flag = sps.pcm_loop_filter_disabled;
    }
    pp.long_term_ref_pics_present_flag = sps.long_term_present;
    pp.sps_temporal_mvp_enabled_flag = sps.temporal_mvp;
    pp.strong_intra_smoothing_enabled_flag = sps.strong_intra_smoothing;
    pp.dependent_slice_segments_enabled_flag = pps.dependent_slices;
    pp.output_flag_present_flag = pps.output_flag_present;
    pp.num_extra_slice_header_bits = static_cast<UINT32>(pps.num_extra_slice_header_bits);
    pp.sign_data_hiding_enabled_flag = pps.sign_data_hiding;
    pp.cabac_init_present_flag = pps.cabac_init_present;
    pp.constrained_intra_pred_flag = pps.constrained_intra_pred;
    pp.transform_skip_enabled_flag = pps.transform_skip;
    pp.cu_qp_delta_enabled_flag = pps.cu_qp_delta_enabled;
    pp.pps_slice_chroma_qp_offsets_present_flag = pps.slice_chroma_qp_offsets_present;
    pp.weighted_pred_flag = pps.weighted_pred;
    pp.weighted_bipred_flag = pps.weighted_bipred;
    pp.transquant_bypass_enabled_flag = pps.transquant_bypass;
    pp.tiles_enabled_flag = pps.tiles;
    pp.entropy_coding_sync_enabled_flag = pps.entropy_sync;
    pp.uniform_spacing_flag = pps.uniform_spacing;
    pp.loop_filter_across_tiles_enabled_flag = pps.loop_filter_across_tiles;
    pp.pps_loop_filter_across_slices_enabled_flag = pps.loop_filter_across_slices;
    pp.deblocking_filter_override_enabled_flag = pps.deblocking_override_enabled;
    pp.pps_deblocking_filter_disabled_flag = pps.deblocking_disabled;
    pp.lists_modification_present_flag = pps.lists_modification_present;
    pp.slice_segment_header_extension_present_flag = pps.slice_header_extension_present;
    pp.IrapPicFlag = irap;
    pp.IdrPicFlag = idr;
    pp.IntraPicFlag = irap;
    pp.pps_cb_qp_offset = static_cast<CHAR>(pps.cb_qp_offset);
    pp.pps_cr_qp_offset = static_cast<CHAR>(pps.cr_qp_offset);
    if (pps.tiles) {
        pp.num_tile_columns_minus1 = static_cast<UCHAR>(pps.num_tile_columns_minus1);
        pp.num_tile_rows_minus1 = static_cast<UCHAR>(pps.num_tile_rows_minus1);
        if (!pps.uniform_spacing) {
            for (int i = 0; i < pps.num_tile_columns_minus1; ++i) pp.column_width_minus1[i] = static_cast<USHORT>(pps.column_width_minus1[i]);
            for (int i = 0; i < pps.num_tile_rows_minus1; ++i) pp.row_height_minus1[i] = static_cast<USHORT>(pps.row_height_minus1[i]);
        }
    }
    pp.diff_cu_qp_delta_depth = static_cast<UCHAR>(pps.diff_cu_qp_delta_depth);
    pp.pps_beta_offset_div2 = static_cast<CHAR>(pps.beta_offset_div2);
    pp.pps_tc_offset_div2 = static_cast<CHAR>(pps.tc_offset_div2);
    pp.log2_parallel_merge_level_minus2 = static_cast<UCHAR>(pps.log2_parallel_merge_level_minus2);
    pp.CurrPicOrderCntVal = poc;

    // Every reference picture still in the DPB, then the current RPS lists as indices into it.
    std::memset(pp.RefPicList, 0xFF, sizeof(pp.RefPicList));
    int map[64];
    std::fill(std::begin(map), std::end(map), 0xFF);
    int n = 0;
    for (size_t i = 0; i < pics_.size() && n < 15; ++i) {
        if (static_cast<int>(i) == surface || !(pics_[i].short_ref || pics_[i].long_ref)) continue;
        pp.RefPicList[n].Index7Bits = static_cast<UCHAR>(i);
        pp.RefPicList[n].AssociatedFlag = pics_[i].long_ref ? 1 : 0;
        pp.PicOrderCntValList[n] = pics_[i].poc;
        map[i] = n++;
    }
    std::memset(pp.RefPicSetStCurrBefore, 0xFF, 8);
    std::memset(pp.RefPicSetStCurrAfter, 0xFF, 8);
    std::memset(pp.RefPicSetLtCurr, 0xFF, 8);
    for (size_t i = 0; i < st_before.size() && i < 8; ++i)
        pp.RefPicSetStCurrBefore[i] = static_cast<UCHAR>(st_before[i] == 0xFF ? 0xFF : map[st_before[i]]);
    for (size_t i = 0; i < st_after.size() && i < 8; ++i)
        pp.RefPicSetStCurrAfter[i] = static_cast<UCHAR>(st_after[i] == 0xFF ? 0xFF : map[st_after[i]]);
    for (size_t i = 0; i < lt_curr.size() && i < 8; ++i)
        pp.RefPicSetLtCurr[i] = static_cast<UCHAR>(lt_curr[i] == 0xFF ? 0xFF : map[lt_curr[i]]);
    pp.StatusReportFeedbackNumber = ++feedback_;

    DXVA_Qmatrix_HEVC qm{};
    if (sps.scaling_list_enabled) {
        const ScalingList& sl = pps.has_sl ? pps.sl : sps.sl;
        for (int m = 0; m < 6; ++m) {
            std::memcpy(qm.ucScalingLists0[m], sl.sl[0][m], 16);
            std::memcpy(qm.ucScalingLists1[m], sl.sl[1][m], 64);
            std::memcpy(qm.ucScalingLists2[m], sl.sl[2][m], 64);
            qm.ucScalingListDCCoefSizeID2[m] = sl.dc[2][m];
        }
        for (int m = 0; m < 2; ++m) {
            std::memcpy(qm.ucScalingLists3[m], sl.sl[3][m], 64);
            qm.ucScalingListDCCoefSizeID3[m] = sl.dc[3][m];
        }
    }

    // Bitstream: each slice NAL behind a 3-byte start code; padded to 128 bytes.
    bitstream_.clear();
    std::vector<DXVA_Slice_HEVC_Short> sc;
    for (const auto& s : slices) {
        DXVA_Slice_HEVC_Short e{};
        e.BSNALunitDataLocation = static_cast<UINT>(bitstream_.size());
        static const uint8_t start[3] = {0, 0, 1};
        bitstream_.insert(bitstream_.end(), start, start + 3);
        bitstream_.insert(bitstream_.end(), s.data, s.data + s.size);
        e.SliceBytesInBuffer = static_cast<UINT>(s.size + 3);
        sc.push_back(e);
    }
    const size_t padded = (bitstream_.size() + 127) & ~static_cast<size_t>(127);
    if (!sc.empty()) sc.back().SliceBytesInBuffer += static_cast<UINT>(padded - bitstream_.size());
    bitstream_.resize(padded, 0);

    HRESULT hr = E_PENDING;
    for (int tries = 0; tries < 100 && hr == E_PENDING; ++tries) {
        hr = vctx_->DecoderBeginFrame(decoder_.Get(), views_[static_cast<size_t>(surface)].Get(), 0, nullptr);
        if (hr == E_PENDING) { Sleep(1); ++pending_waits_; }
    }
    if (FAILED(hr)) {
        err = "DecoderBeginFrame failed";
        return false;
    }
    auto put = [&](D3D11_VIDEO_DECODER_BUFFER_TYPE type, const void* src, UINT size) {
        UINT cap = 0;
        void* dst = nullptr;
        if (FAILED(vctx_->GetDecoderBuffer(decoder_.Get(), type, &cap, &dst)) || !dst || cap < size) return false;
        std::memcpy(dst, src, size);
        return SUCCEEDED(vctx_->ReleaseDecoderBuffer(decoder_.Get(), type));
    };
    D3D11_VIDEO_DECODER_BUFFER_DESC bufs[4]{};
    UINT nb = 0;
    bool ok = put(D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, &pp, sizeof(pp));
    bufs[nb].BufferType = D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS;
    bufs[nb++].DataSize = sizeof(pp);
    if (ok && sps.scaling_list_enabled) {
        ok = put(D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX, &qm, sizeof(qm));
        bufs[nb].BufferType = D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX;
        bufs[nb++].DataSize = sizeof(qm);
    }
    const UINT sc_size = static_cast<UINT>(sc.size() * sizeof(DXVA_Slice_HEVC_Short));
    ok = ok && put(D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, sc.data(), sc_size);
    bufs[nb].BufferType = D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL;
    bufs[nb].DataSize = sc_size;
    bufs[nb++].NumMBsInBuffer = static_cast<UINT>(sc.size());
    ok = ok && put(D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, bitstream_.data(), static_cast<UINT>(bitstream_.size()));
    bufs[nb].BufferType = D3D11_VIDEO_DECODER_BUFFER_BITSTREAM;
    bufs[nb].DataSize = static_cast<UINT>(bitstream_.size());
    bufs[nb++].NumMBsInBuffer = static_cast<UINT>(sc.size());
    if (ok) ok = SUCCEEDED(vctx_->SubmitDecoderBuffers(decoder_.Get(), nb, bufs));
    vctx_->DecoderEndFrame(decoder_.Get());
    if (!ok) err = "decoder buffers (too small or rejected)";
    return ok;
}

bool Decoder::PopOutput(Frame& out, bool flush) {
    if (flush)
        while (CountNeededOutput() > 0) Bump();
    if (out_queue_.empty()) return false;
    const int s = out_queue_.front();
    out_queue_.pop_front();
    Pic& p = pics_[static_cast<size_t>(s)];
    p.queued = false;
    p.held = true;
    out = Frame{s, p.pts, p.poc};
    return true;
}

void Decoder::Release(int surface) {
    if (surface >= 0 && static_cast<size_t>(surface) < pics_.size()) pics_[static_cast<size_t>(surface)].held = false;
}

void Decoder::Reset() {
    for (auto& p : pics_) {
        p.short_ref = p.long_ref = false;
        p.needed_output = false;
    }
    prev_tid0_poc_ = 0;
    no_rasl_next_ = true;
}

void Decoder::Close() {
    out_queue_.clear();
    pics_.clear();
    views_.clear();
    texture_.Reset();
    decoder_.Reset();
    vctx_.Reset();
    vdev_.Reset();
}

}  // namespace hevc
