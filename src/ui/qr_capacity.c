#include "qr_capacity.h"

/* Byte-mode payload capacity at ECC level L, indexed by QR version, from
 * ISO/IEC 18004's capacity tables. */
static const uint16_t CAPACITY_ECC_L[QR_MAX_VERSION + 1] = {
    0,   17,  32,  53,  78,  106, 134, 154, 192, 230, 271,
    321, 367, 425, 458, 520, 586, 644, 718, 792, 858,
};

uint16_t qr_capacity_for_version(uint8_t version) {
    if (version < 1 || version > QR_MAX_VERSION) {
        return 0;
    }
    return CAPACITY_ECC_L[version];
}

uint8_t qr_version_for_length(size_t len) {
    for (uint8_t v = 1; v <= QR_MAX_VERSION; v++) {
        if (CAPACITY_ECC_L[v] >= len) {
            return v;
        }
    }
    return 0;
}

int qr_square_modules(int qr_size) {
    if (qr_size <= 0) {
        return 0;
    }
    return qr_size + 2 * QR_QUIET_ZONE_MODULES;
}

int qr_scale_for(int qr_size, int screen_w, int screen_h) {
    const int modules = qr_square_modules(qr_size);
    if (modules <= 0 || screen_w <= 0 || screen_h <= 0) {
        return 0;
    }
    /* The SHORTER dimension, not the height. Taking one dimension alone
     * produced a square wider than a landscape panel, and the centring below
     * then went negative. */
    const int shorter = screen_w < screen_h ? screen_w : screen_h;
    return shorter / modules; /* 0 when even one pixel per module will not fit */
}

void qr_origin(int qr_size, int scale, int screen_w, int screen_h, int *x0, int *y0) {
    const int px = qr_square_modules(qr_size) * scale;
    if (x0) {
        *x0 = (screen_w - px) / 2;
    }
    if (y0) {
        *y0 = (screen_h - px) / 2;
    }
}

int qr_module_at(int pixel_in_square, int scale) {
    if (scale <= 0) {
        return -1;
    }
    return pixel_in_square / scale - QR_QUIET_ZONE_MODULES;
}
