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
