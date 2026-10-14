// MacGlass wire protocol. mac/Sources/Protocol.swift mirrors these values; keep them in sync.
//
// Plain TCP over the Thunderbolt link. Every message is an 8-byte header
// {u32 type, u32 payload_length}, little-endian, followed by the payload.
#pragma once

#include <stdint.h>

#define MG_PROTOCOL_VERSION 8
#define MG_DEFAULT_PORT "47800"

enum mg_msg_type {
    MG_HELLO = 1,   // Mac -> client: u32 protocol_version, u32 codec fourcc
    MG_FRAME = 2,   // Mac -> client: u32 width, u32 height, u64 capture_ns, u32 codec fourcc,
                    // u32 flags (MG_FRAME_KEY), then one ProRes frame or HEVC Annex-B access unit
    MG_CURSOR = 3,  // Mac -> client: u16 width, u16 height, u16 hot_x, u16 hot_y (pixels), then
                    // straight-alpha RGBA8 rows, top first. Sent when the cursor shape changes.
    MG_PATCH = 4,   // Mac -> client: u32 frame_width, u32 frame_height, u32 x, u32 y, u64 capture_ns,
                    // then one ProRes picture to place at (x, y) in the current frame
    MG_AUDIO = 5,   // Mac -> client: u32 sample_rate, u32 channels, then interleaved f32 samples
    MG_CURSOR_POS = 6, // Mac -> client: u32 x, u32 y (pixels in the frame), u32 visible. Sent when it moves.
    MG_RESIZE = 10, // client -> Mac: u32 width, u32 height (pixels)
    MG_POINTER = 11,     // client -> Mac: f32 x, f32 y, position within the frame from 0 to 1
    MG_BUTTON = 12,      // client -> Mac: u8 button (enum mg_button), u8 down, u16 reserved
    MG_SCROLL = 13,      // client -> Mac: f32 dx, f32 dy in wheel steps; +x scrolls right, +y up
    MG_KEY = 14,         // client -> Mac: u16 USB HID usage (page 7), u8 down, u8 repeat
    MG_RELEASE_ALL = 15, // client -> Mac: no payload; release every key and button
    MG_CLIPBOARD = 16,   // both ways: u8 kind (enum mg_clip_kind), then the content
    MG_SET_CODEC = 17,   // client -> Mac: u32 fourcc for full frames: MG_CODEC_PRORES or MG_CODEC_HEVC
    MG_KEYFRAME = 18,    // client -> Mac: no payload; send the next full frame as a keyframe
};

#define MG_FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define MG_CODEC_PRORES MG_FOURCC('a', 'p', '4', 'h') // ProRes; the Mac picks 4444 or 422 HQ by frame size
#define MG_CODEC_HEVC MG_FOURCC('h', 'v', 'c', '1')   // HEVC Main 4:4:4 10
#define MG_FRAME_KEY 1

enum mg_clip_kind { MG_CLIP_TEXT, MG_CLIP_PNG }; // UTF-8 text, PNG image

enum mg_button { MG_BUTTON_LEFT, MG_BUTTON_RIGHT, MG_BUTTON_MIDDLE, MG_BUTTON_BACK, MG_BUTTON_FORWARD };

// Discovery: a UDP datagram MG_DISCOVER_QUERY sent to the default port (e.g. to ff02::1 on the
// Thunderbolt interface) is answered with MG_DISCOVER_REPLY " <protocol_version> <computer name>".
#define MG_DISCOVER_QUERY "MACGLASS?"
#define MG_DISCOVER_REPLY "MACGLASS"

#define MG_FRAME_HEADER_SIZE 24
#define MG_PATCH_HEADER_SIZE 24
