#pragma once

#include <stdint.h>

typedef uint32_t vlc_fourcc_t;

#define VLC_FOURCC(a, b, c, d) \
    ((vlc_fourcc_t)( \
        ((uint32_t)(a) & 0xFFu) | \
        (((uint32_t)(b) & 0xFFu) << 8) | \
        (((uint32_t)(c) & 0xFFu) << 16) | \
        (((uint32_t)(d) & 0xFFu) << 24)))

/* Common software chromas */
#define VLC_CODEC_I420 VLC_FOURCC('I', '4', '2', '0')
#define VLC_CODEC_J420 VLC_FOURCC('J', '4', '2', '0')
#define VLC_CODEC_YV12 VLC_FOURCC('Y', 'V', '1', '2')
#define VLC_CODEC_I422 VLC_FOURCC('I', '4', '2', '2')
#define VLC_CODEC_I444 VLC_FOURCC('I', '4', '4', '4')
#define VLC_CODEC_YUVA VLC_FOURCC('Y', 'U', 'V', 'A')
#define VLC_CODEC_RGB24 VLC_FOURCC('R', 'V', '2', '4')
#define VLC_CODEC_RGB32 VLC_FOURCC('R', 'V', '3', '2')
#define VLC_CODEC_RGBA  VLC_FOURCC('R', 'G', 'B', 'A')
#define VLC_CODEC_ARGB  VLC_FOURCC('A', 'R', 'G', 'B')
#define VLC_CODEC_BGRA  VLC_FOURCC('B', 'G', 'R', 'A')
#define VLC_CODEC_NV12  VLC_FOURCC('N', 'V', '1', '2')
#define VLC_CODEC_NV21  VLC_FOURCC('N', 'V', '2', '1')
#define VLC_CODEC_P010  VLC_FOURCC('P', '0', '1', '0')

/* Hardware / packed variants used by the filter */
#define VLC_CODEC_D3D9_OPAQUE       0x20000001u
#define VLC_CODEC_D3D9_OPAQUE_10B   0x20000002u
#define VLC_CODEC_D3D11_OPAQUE      0x20000003u
#define VLC_CODEC_D3D11_OPAQUE_10B  0x20000004u
#define VLC_CODEC_VAAPI_420         0x20000005u
#define VLC_CODEC_VAAPI_420_10BPP   0x20000006u
#define VLC_CODEC_CVPX_NV12         0x20000007u
#define VLC_CODEC_CVPX_I420         0x20000008u
#define VLC_CODEC_CVPX_BGRA         0x20000009u
#define VLC_CODEC_CVPX_P010         0x2000000Au

/* Higher bit-depth planar variants used by the filter */
#define VLC_CODEC_I420_9L           0x20000101u
#define VLC_CODEC_I420_9B           0x20000102u
#define VLC_CODEC_I420_10L          0x20000103u
#define VLC_CODEC_I420_10B          0x20000104u
#define VLC_CODEC_I420_12L          0x20000105u
#define VLC_CODEC_I420_12B          0x20000106u
#define VLC_CODEC_I420_16L          0x20000107u
#define VLC_CODEC_I420_16B          0x20000108u
#define VLC_CODEC_I422_9L           0x20000109u
#define VLC_CODEC_I422_9B           0x2000010Au
#define VLC_CODEC_I422_10L          0x2000010Bu
#define VLC_CODEC_I422_10B          0x2000010Cu
#define VLC_CODEC_I422_12L          0x2000010Du
#define VLC_CODEC_I422_12B          0x2000010Eu
#define VLC_CODEC_I444_9L           0x2000010Fu
#define VLC_CODEC_I444_9B           0x20000110u
#define VLC_CODEC_I444_10L          0x20000111u
#define VLC_CODEC_I444_10B          0x20000112u
#define VLC_CODEC_I444_12L          0x20000113u
#define VLC_CODEC_I444_12B          0x20000114u
#define VLC_CODEC_I444_16L          0x20000115u
#define VLC_CODEC_I444_16B          0x20000116u
#define VLC_CODEC_YUVA_444_10L      0x20000117u
#define VLC_CODEC_YUVA_444_10B      0x20000118u
