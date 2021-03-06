/*******************************************************************************
* Copyright 2016-2018 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include <assert.h>
#include <math.h>

#include "c_types_map.hpp"
#include "mkldnn_thread.hpp"
#include "type_helpers.hpp"

#include "ref_lrn.hpp"

namespace mkldnn {
namespace impl {
namespace cpu {

static inline float fast_negative_powf(float omega, float beta) {
    float Y;
    if (beta == 0.75f) {
        Y = 1.0f / sqrtf(omega);
        Y *= sqrtf(Y);
    } else {
        Y = 1.0f /powf(omega, beta);
    }
    return Y;
};

template <impl::data_type_t data_type>
template <mkldnn_memory_format_t fmt>
void ref_lrn_fwd_t<data_type>::execute_forward() {
    using namespace alg_kind;
    using namespace memory_format;

    auto src = reinterpret_cast<const data_t *>(this->input_memory(0));
    auto dst = reinterpret_cast<data_t*>(this->memory(0));
    auto ws = reinterpret_cast<data_t*>(this->memory(1));

    const memory_desc_wrapper data_d(conf_.src_pd());
    const memory_desc_wrapper ws_d(conf_.workspace_pd());
    MAYBE_UNUSED(ws_d);

    const int C = conf_.C();
    const int H = conf_.H();
    const int W = conf_.W();
    const int CHW = C * H * W;
    const bool across_channels = conf_.desc()->alg_kind == lrn_across_channels;
    constexpr int blksize = fmt == nChw16c ? 16 : 8;

    auto data_off =[&](int mb, int c, int h, int w) {
        switch (fmt) {
        case nChw16c:
        case nChw8c: return (size_t)mb * CHW + c / blksize * H * W * blksize
                     + h * W * blksize + w * blksize + c % blksize;
        case nchw: return (size_t)mb * CHW + c * H * W + h * W + w;
        case nhwc: return (size_t)mb * CHW + h * W * C + w * C + c;
        default: return (size_t)data_d.off(mb, c, h, w);
        }
    };

    auto ker = [=](data_t *d, int mb, int oc, int oh, int ow) {
        const float alpha = static_cast<float>(conf_.desc()->lrn_alpha);
        const float beta = static_cast<float>(conf_.desc()->lrn_beta);
        const float k = static_cast<float>(conf_.desc()->lrn_k);

        const int size = conf_.desc()->local_size;
        const int half_size = (size - 1) / 2;

        float sum = 0;
        if (across_channels) {
            const int c_st = nstl::max(oc - half_size + 0, 0);
            const int c_en = nstl::min(oc + half_size + 1, C);

            for (int c = c_st; c < c_en; ++c) {
                const float s = src[data_off(mb, c, oh, ow)];
                sum += s * s;
            }
        } else {
            int h_st = nstl::max(oh - half_size + 0, 0);
            int h_en = nstl::min(oh + half_size + 1, H);
            int w_st = nstl::max(ow - half_size + 0, 0);
            int w_en = nstl::min(ow + half_size + 1, W);
            for (int h = h_st; h < h_en; ++h) {
                for (int w = w_st; w < w_en; ++w) {
                    const float s = src[data_off(mb, oc, h, w)];
                    sum += s * s;
                }
            }
        }
        const int summands = across_channels ? size : size * size;
        sum = k + alpha * sum / summands;
        size_t off = data_off(mb, oc, oh, ow);
        if (ws)
            ws[off] = static_cast<data_t>(sum);
        d[0] = static_cast<data_t>(src[off] * fast_negative_powf(sum, beta));
    };

    const int MB = conf_.MB();
    if (fmt == nChw16c || fmt == nChw8c) {
#       pragma omp parallel for collapse(4) schedule(static)
        for (int mb = 0; mb < MB; ++mb)
        for (int c = 0; c < C; c += blksize)
        for (int h = 0; h < H; ++h)
        for (int w = 0; w < W; ++w)
        {
            const size_t off = (size_t)(mb * CHW + c * H * W + (h * W + w)
                * blksize);
            PRAGMA_OMP_SIMD()
            for (int cc = 0; cc < blksize; ++cc)
                ker(&dst[off + cc], mb, c + cc, h, w);
        }
    } else if (fmt == nhwc) {
#       pragma omp parallel for collapse(4) schedule(static)
        for (int mb = 0; mb < MB; ++mb)
        for (int h = 0; h < H; ++h)
        for (int w = 0; w < W; ++w)
        for (int c = 0; c < C; ++c)
        {
            const size_t off = (size_t)(mb * CHW + h * W * C + w * C + c);
            ker(&dst[off], mb, c, h, w);
        }
    } else {
#       pragma omp parallel for collapse(4) schedule(static)
        for (int mb = 0; mb < MB; ++mb)
        for (int c = 0; c < C; ++c)
        for (int h = 0; h < H; ++h)
        for (int w = 0; w < W; ++w)
        {
            const size_t off = data_off(mb, c, h, w);
            ker(&dst[off], mb, c, h, w);
        }
    }
}

template <impl::data_type_t data_type>
template <mkldnn_memory_format_t fmt>
void ref_lrn_bwd_t<data_type>::execute_backward() {
    using namespace alg_kind;
    using namespace memory_format;

    auto src = reinterpret_cast<const data_t *>(this->input_memory(0));
    auto diff_dst = reinterpret_cast<const data_t *>(this->input_memory(1));
    auto diff_src = reinterpret_cast<data_t*>(this->memory(0));

    const memory_desc_wrapper data_d(conf_.src_pd());
    const memory_desc_wrapper diff_data_d(conf_.diff_dst_pd());
    MAYBE_UNUSED(diff_data_d);

    const int MB = conf_.MB();
    const int C = conf_.C();
    const int H = conf_.H();
    const int W = conf_.W();
    const int CHW = C * H * W;
    constexpr int blksize = fmt == nChw16c ? 16 : 8;

    const float alpha = static_cast<float>(conf_.desc()->lrn_alpha);
    const float beta = static_cast<float>(conf_.desc()->lrn_beta);
    const float k = static_cast<float>(conf_.desc()->lrn_k);
    const int kernel_size = conf_.desc()->local_size;
    const int half_ksize = (kernel_size - 1) / 2;

    auto data_off =[&](int mb, int c, int h, int w) {
        switch (fmt) {
        case nChw16c:
        case nChw8c: return (size_t)mb * CHW + c/blksize * H * W * blksize
                     + h * W * blksize + w * blksize + c%blksize;
        case nchw: return (size_t)mb * CHW + c * H * W + h * W + w;
        case nhwc: return (size_t)mb * CHW + h * W * C + w * C + c;
        default: return (size_t)data_d.off(mb, c, h, w);
        }
    };

    auto ker = [=](data_t *d, int mb, int oc, int oh, int ow) {
        const int c_st = nstl::max(oc - half_ksize + 0, 0);
        const int c_en = nstl::min(oc + half_ksize + 1, C);

        float A = 0, B = 0, omega_mid = 0;
        for (int c = c_st; c < c_en; c++) {
            float sum = 0.0;
            for (int i = c_st; i < c_en; ++i) {
                const float value = src[data_off(mb, i, oh, ow)];
                sum += value * value;
            }
            const float omega = static_cast<float>(k + sum * alpha / kernel_size);
            if (c == oc) omega_mid = omega;
            float t = src[data_off(mb, c, oh, ow)]
                   * fast_negative_powf(omega, beta);
            B += 1.0f / omega * t * diff_dst[data_off(mb, c, oh, ow)];
        }

        const size_t off = data_off(mb, oc, oh, ow);
        A = fast_negative_powf(omega_mid, beta) * diff_dst[off];
        B *= src[off];
        B *= (2.0f * alpha * beta) / kernel_size;
        *d = static_cast<data_t>(A - B); // final cast down to data_t
    };

    if (fmt == nChw16c || fmt == nChw8c) {
#       pragma omp parallel for collapse(4) schedule(static)
        for (int mb = 0; mb < MB; ++mb)
        for (int c = 0; c < C; c += blksize)
        for (int h = 0; h < H; ++h)
        for (int w = 0; w < W; ++w)
        {
            const size_t off = (size_t)(mb * CHW + c * H * W + (h * W + w)
                * blksize);
            PRAGMA_OMP_SIMD()
            for (int cc = 0; cc < blksize; ++cc)
                ker(&diff_src[off + cc], mb, c + cc, h, w);
        }
    } else if (fmt == nhwc) {
#       pragma omp parallel for collapse(4) schedule(static)
        for (int mb = 0; mb < MB; ++mb)
        for (int h = 0; h < H; ++h)
        for (int w = 0; w < W; ++w)
        for (int c = 0; c < C; ++c)
        {
            const size_t off = (size_t)(mb * CHW + h * W * C + w * C + c);
            ker(&diff_src[off], mb, c, h, w);
        }
    } else {
#       pragma omp parallel for collapse(4) schedule(static)
        for (int mb = 0; mb < MB; ++mb)
        for (int c = 0; c < C; ++c)
        for (int h = 0; h < H; ++h)
        for (int w = 0; w < W; ++w)
        {
            const size_t off = data_off(mb, c, h, w);
            ker(&diff_src[off], mb, c, h, w);
        }
    }
}

template void ref_lrn_fwd_t<data_type::f32>::execute_forward<memory_format::nChw16c>();
template void ref_lrn_fwd_t<data_type::f32>::execute_forward<memory_format::nChw8c>();
template void ref_lrn_fwd_t<data_type::f32>::execute_forward<memory_format::nchw>();
template void ref_lrn_fwd_t<data_type::f32>::execute_forward<memory_format::nhwc>();
template void ref_lrn_fwd_t<data_type::f32>::execute_forward<memory_format::any>();
template void ref_lrn_bwd_t<data_type::f32>::execute_backward<memory_format::nChw16c>();
template void ref_lrn_bwd_t<data_type::f32>::execute_backward<memory_format::nChw8c>();
template void ref_lrn_bwd_t<data_type::f32>::execute_backward<memory_format::nchw>();
template void ref_lrn_bwd_t<data_type::f32>::execute_backward<memory_format::nhwc>();
template void ref_lrn_bwd_t<data_type::f32>::execute_backward<memory_format::any>();

}
}
}

// vim: et ts=4 sw=4 cindent cino^=l0,\:0,N-s
