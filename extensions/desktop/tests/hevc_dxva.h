// hevc_dxva.h - Prototype HEVC front end for D3D11 video decoding (DXVA, short slice format).
// Parses VPS/SPS/PPS and the picture-level part of slice headers, keeps the DPB (RPS marking, POC,
// output order), and decodes into a texture array sized from the stream itself:
// sps_max_dec_pic_buffering + extra surfaces, instead of a decoder's worst-case pool.
// Pull model: PopOutput() hands out one frame in display order; the caller releases it after showing it.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace hevc {

struct StRps {
    int num_neg = 0, num_pos = 0;
    int delta_s0[16] = {}, delta_s1[16] = {};
    bool used_s0[16] = {}, used_s1[16] = {};
    int NumDelta() const { return num_neg + num_pos; }
};

// ScalingList[sizeId][matrixId][i] in coded (up-right diagonal) order, as DXVA wants it.
struct ScalingList {
    uint8_t sl[4][6][64] = {};
    uint8_t dc[4][6] = {};
};

struct Sps {
    bool valid = false;
    int max_sub_layers_minus1 = 0, chroma_format_idc = 1;
    bool separate_colour_plane = false;
    int width = 0, height = 0, conf_left = 0, conf_right = 0, conf_top = 0, conf_bottom = 0;
    int bit_depth_luma = 8, bit_depth_chroma = 8, log2_max_poc_lsb = 4;
    int max_dec_pic_buffering[8] = {}, max_num_reorder[8] = {};
    int log2_min_cb = 3, log2_diff_max_min_cb = 0, log2_min_tb = 2, log2_diff_max_min_tb = 0;
    int max_th_depth_inter = 0, max_th_depth_intra = 0;
    bool scaling_list_enabled = false;
    ScalingList sl;
    bool amp = false, sao = false, pcm = false, pcm_loop_filter_disabled = false;
    int pcm_bd_luma = 8, pcm_bd_chroma = 8, log2_min_pcm = 3, log2_diff_max_min_pcm = 0;
    int num_st_rps = 0;
    StRps st_rps[65];
    bool long_term_present = false;
    int num_lt_sps = 0, lt_poc_lsb_sps[33] = {};
    bool lt_used_sps[33] = {};
    bool temporal_mvp = false, strong_intra_smoothing = false;
};

struct Pps {
    bool valid = false;
    int sps_id = 0;
    bool dependent_slices = false, output_flag_present = false, sign_data_hiding = false, cabac_init_present = false;
    int num_extra_slice_header_bits = 0, num_ref_idx_l0_default_minus1 = 0, num_ref_idx_l1_default_minus1 = 0;
    int init_qp_minus26 = 0;
    bool constrained_intra_pred = false, transform_skip = false, cu_qp_delta_enabled = false;
    int diff_cu_qp_delta_depth = 0, cb_qp_offset = 0, cr_qp_offset = 0;
    bool slice_chroma_qp_offsets_present = false, weighted_pred = false, weighted_bipred = false;
    bool transquant_bypass = false, tiles = false, entropy_sync = false, uniform_spacing = true;
    int num_tile_columns_minus1 = 0, num_tile_rows_minus1 = 0;
    int column_width_minus1[20] = {}, row_height_minus1[22] = {};
    bool loop_filter_across_tiles = true, loop_filter_across_slices = false;
    bool deblocking_override_enabled = false, deblocking_disabled = false;
    int beta_offset_div2 = 0, tc_offset_div2 = 0;
    bool has_sl = false;
    ScalingList sl;
    bool lists_modification_present = false, slice_header_extension_present = false;
    int log2_parallel_merge_level_minus2 = 0;
};

struct Frame {
    int surface = -1;
    int64_t pts = 0;
    int poc = 0;
};

class Decoder {
public:
    // extra_surfaces: added to sps_max_dec_pic_buffering for the texture array (>= 1 for the shown frame).
    bool Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, int extra_surfaces, std::string& err);
    // Parameter sets from the container (Annex B or length-prefixed), if any.
    void ParseConfig(const uint8_t* data, size_t size);
    // One access unit (one sample). False only on a fatal error (err says why).
    bool DecodeAccessUnit(const uint8_t* data, size_t size, int64_t pts, std::string& err);
    // Next frame in display order; flush=true first moves every waiting picture to the output.
    bool PopOutput(Frame& out, bool flush);
    void Release(int surface);
    // Seek/loop: forget references and POC state (frames already handed out stay valid).
    void Reset();
    void Close();

    bool Ready() const { return texture_ != nullptr; }
    ID3D11Texture2D* Texture() const { return texture_.Get(); }
    int Surfaces() const { return static_cast<int>(pics_.size()); }
    int DpbSize() const { return dpb_size_; }
    DXGI_FORMAT Format() const { return format_; }
    UINT CodedWidth() const { return coded_w_; }
    UINT CodedHeight() const { return coded_h_; }
    RECT Crop() const { return crop_; }
    uint64_t Decoded() const { return decoded_; }
    uint64_t Skipped() const { return skipped_; }
    uint64_t PendingWaits() const { return pending_waits_; }

private:
    struct Nal {
        const uint8_t* data;
        size_t size;
        int type, tid;
    };
    struct Slice {
        int nal_type = 0, tid = 0, pps_id = 0, slice_type = 2;
        bool first = false, dependent = false, pic_output = true;
        int poc_lsb = 0;
        bool st_rps_sps = false;
        int st_rps_idx = 0, st_rps_bits = 0, ref_rps_num_delta = 0;
        StRps st_rps;
        int num_lt = 0, lt_poc[32] = {};
        bool lt_used[32] = {}, lt_msb_present[32] = {};
    };
    struct Pic {
        bool in_use = false, short_ref = false, long_ref = false, needed_output = false, queued = false, held = false;
        int poc = 0;
        int64_t pts = 0;
        bool Free() const { return !short_ref && !long_ref && !needed_output && !queued && !held; }
    };

    static void SplitNals(const uint8_t* data, size_t size, std::vector<Nal>& out);
    void ParseSps(const Nal& n);
    void ParsePps(const Nal& n);
    bool ParseSliceHeader(const Nal& n, Slice& s);
    bool DecodePicture(const std::vector<Nal>& slices, const Slice& sh, int64_t pts, std::string& err);
    bool CreateDecoder(const Sps& sps, std::string& err);
    bool Submit(const std::vector<Nal>& slices, const Slice& sh, const Sps& sps, const Pps& pps, int surface, int poc,
                bool irap, bool idr, const std::vector<int>& st_before, const std::vector<int>& st_after,
                const std::vector<int>& lt_curr, std::string& err);
    void Bump();
    int CountNeededOutput() const;

    ID3D11Device* dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> vdev_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> vctx_;
    Microsoft::WRL::ComPtr<ID3D11VideoDecoder> decoder_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    std::vector<Microsoft::WRL::ComPtr<ID3D11VideoDecoderOutputView>> views_;
    GUID profile_{};
    DXGI_FORMAT format_ = DXGI_FORMAT_NV12;
    UINT coded_w_ = 0, coded_h_ = 0;
    RECT crop_{};
    int extra_ = 1, dpb_size_ = 0, active_sps_ = -1;

    Sps sps_[16];
    Pps pps_[64];
    std::vector<Pic> pics_;
    std::deque<int> out_queue_;
    int prev_tid0_poc_ = 0;
    bool no_rasl_next_ = true, assoc_irap_no_rasl_ = true;
    uint32_t feedback_ = 0;
    uint64_t decoded_ = 0, skipped_ = 0, pending_waits_ = 0;
    std::vector<uint8_t> bitstream_;
};

}  // namespace hevc
