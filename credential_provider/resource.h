#pragma once

#include <windows.h>

// Our credential provider CLSID
// {B8F4C7A1-3D5E-4F2B-A9C6-1D8E7F3A5B2C}
DEFINE_GUID(CLSID_FaceLoginProvider,
    0xb8f4c7a1, 0x3d5e, 0x4f2b, 0xa9, 0xc6, 0x1d, 0x8e, 0x7f, 0x3a, 0x5b, 0x2c);

// Resource IDs
#define IDI_FACELOGIN 101
#define IDB_TILEIMAGE 102
// TILE_PIXELS: 128×128 premultiplied-alpha BGRA raw pixel data
#define IDR_TILE_PIXELS 103
