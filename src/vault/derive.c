#include "derive.h"

#include <string.h>

#include "monocypher-ed25519.h" /* crypto_sha512_hmac */
#include "monocypher.h"         /* crypto_wipe */
#include "sha256.h"

/* The secp256k1 group order, big-endian, one u32 per limb, most significant
 * first. Present not because this module does any curve arithmetic -- it does
 * none -- but because BIP-32 defines a child key as (IL + kpar) mod n, and
 * that modulus is the curve's. */
static const uint32_t GROUP_ORDER_N[8] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFEu,
                                          0xBAAEDCE6u, 0xAF48A03Bu, 0xBFD25E8Cu, 0xD0364141u};

#define BN_LIMBS 8
#define HARDENED 0x80000000u

/* ---- 256-bit arithmetic ------------------------------------------------
 *
 * Every operand here is secret key material, so nothing branches on a value.
 * The conditional subtraction below is done by computing both answers and
 * selecting one with a mask, rather than by an `if`. */

static void bn_load(const uint8_t in[32], uint32_t out[BN_LIMBS]) {
    for (size_t i = 0; i < BN_LIMBS; i++) {
        out[i] = ((uint32_t)in[i * 4] << 24) | ((uint32_t)in[i * 4 + 1] << 16) |
                 ((uint32_t)in[i * 4 + 2] << 8) | (uint32_t)in[i * 4 + 3];
    }
}

static void bn_store(const uint32_t in[BN_LIMBS], uint8_t out[32]) {
    for (size_t i = 0; i < BN_LIMBS; i++) {
        out[i * 4] = (uint8_t)(in[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(in[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(in[i] >> 8);
        out[i * 4 + 3] = (uint8_t)in[i];
    }
}

/* 1 if a < b. Computed as a borrow-out, so it costs the same for every input. */
static uint32_t bn_lt(const uint32_t a[BN_LIMBS], const uint32_t b[BN_LIMBS]) {
    uint32_t borrow = 0;
    for (size_t i = BN_LIMBS; i-- > 0;) {
        uint64_t t = (uint64_t)a[i] - (uint64_t)b[i] - (uint64_t)borrow;
        borrow = (t >> 32) ? 1u : 0u;
    }
    return borrow;
}

static uint32_t bn_is_zero(const uint32_t a[BN_LIMBS]) {
    uint32_t acc = 0;
    for (size_t i = 0; i < BN_LIMBS; i++) {
        acc |= a[i];
    }
    /* acc is public information only in that it is zero or not; the compare
     * itself does not depend on which limb differed. */
    return acc == 0 ? 1u : 0u;
}

/* out = (a + b) mod n, for a and b already < n. Their sum is < 2n, so exactly
 * one conditional subtraction of n is enough. */
static void bn_add_mod_n(const uint32_t a[BN_LIMBS], const uint32_t b[BN_LIMBS],
                          uint32_t out[BN_LIMBS]) {
    uint32_t sum[BN_LIMBS];
    uint32_t carry = 0;
    for (size_t i = BN_LIMBS; i-- > 0;) {
        uint64_t t = (uint64_t)a[i] + (uint64_t)b[i] + (uint64_t)carry;
        sum[i] = (uint32_t)t;
        carry = (uint32_t)(t >> 32);
    }

    uint32_t reduced[BN_LIMBS];
    uint32_t borrow = 0;
    for (size_t i = BN_LIMBS; i-- > 0;) {
        uint64_t t = (uint64_t)sum[i] - (uint64_t)GROUP_ORDER_N[i] - (uint64_t)borrow;
        reduced[i] = (uint32_t)t;
        borrow = (t >> 32) ? 1u : 0u;
    }

    /* Take the reduced value when the sum overflowed 256 bits (carry), or when
     * subtracting n did not borrow (so sum >= n). */
    const uint32_t take = (carry | (borrow ^ 1u)) & 1u;
    const uint32_t mask = (uint32_t)0 - take;
    for (size_t i = 0; i < BN_LIMBS; i++) {
        out[i] = (sum[i] & ~mask) | (reduced[i] & mask);
    }

    crypto_wipe(sum, sizeof(sum));
    crypto_wipe(reduced, sizeof(reduced));
}

/* ---- HMAC-SHA256 -------------------------------------------------------
 *
 * RFC 2104 over sha256.c. HMAC-SHA512 comes from monocypher (already linked
 * for OTA signature checking), but nothing in this firmware needed an
 * HMAC-SHA256 before now: LUD-05's domain step is the first caller. */

#define SHA256_BLOCK 64

static void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                         uint8_t out[32]) {
    uint8_t block[SHA256_BLOCK];
    memset(block, 0, sizeof(block));
    if (key_len > SHA256_BLOCK) {
        sha256(key, key_len, block);
    } else {
        memcpy(block, key, key_len);
    }

    uint8_t pad[SHA256_BLOCK];
    sha256_ctx_t ctx;
    uint8_t inner[32];

    for (size_t i = 0; i < SHA256_BLOCK; i++) {
        pad[i] = block[i] ^ 0x36u;
    }
    sha256_init(&ctx);
    sha256_update(&ctx, pad, sizeof(pad));
    sha256_update(&ctx, msg, msg_len);
    sha256_final(&ctx, inner);

    for (size_t i = 0; i < SHA256_BLOCK; i++) {
        pad[i] = block[i] ^ 0x5Cu;
    }
    sha256_init(&ctx);
    sha256_update(&ctx, pad, sizeof(pad));
    sha256_update(&ctx, inner, sizeof(inner));
    sha256_final(&ctx, out);

    crypto_wipe(block, sizeof(block));
    crypto_wipe(pad, sizeof(pad));
    crypto_wipe(inner, sizeof(inner));
    crypto_wipe(&ctx, sizeof(ctx));
}

/* ---- BIP-32 ------------------------------------------------------------ */

static const uint8_t BIP32_MASTER_KEY[] = {'B', 'i', 't', 'c', 'o', 'i', 'n', ' ',
                                            's', 'e', 'e', 'd'};

/* True if `key` is a usable BIP-32 private key: non-zero and below the group
 * order. */
static bool key_in_range(const uint8_t key[32]) {
    uint32_t k[BN_LIMBS];
    bn_load(key, k);
    const bool ok = !bn_is_zero(k) && bn_lt(k, GROUP_ORDER_N);
    crypto_wipe(k, sizeof(k));
    return ok;
}

bool derive_master(const uint8_t *seed, size_t seed_len, derive_xprv_t *out) {
    if (!seed || !out || seed_len < DERIVE_SEED_MIN_LEN || seed_len > DERIVE_SEED_MAX_LEN) {
        return false;
    }

    uint8_t i[64];
    crypto_sha512_hmac(i, BIP32_MASTER_KEY, sizeof(BIP32_MASTER_KEY), seed, seed_len);

    bool ok = key_in_range(i);
    if (ok) {
        memcpy(out->key, i, DERIVE_KEY_LEN);
        memcpy(out->chain_code, i + 32, DERIVE_KEY_LEN);
    }
    crypto_wipe(i, sizeof(i));
    return ok;
}

bool derive_child_hardened(const derive_xprv_t *parent, uint32_t index, derive_xprv_t *out) {
    if (!parent || !out || (index & HARDENED)) {
        return false;
    }

    uint32_t kpar[BN_LIMBS];
    bn_load(parent->key, kpar);

    /* BIP-32's hardened child data: 0x00 || ser256(kpar) || ser32(i). The
     * leading zero is what makes the 33 bytes the same width as the compressed
     * public key an unhardened child would hash instead. */
    uint8_t data[1 + 32 + 4];
    data[0] = 0x00;
    memcpy(data + 1, parent->key, DERIVE_KEY_LEN);

    bool ok = false;
    uint8_t i[64];
    uint32_t il[BN_LIMBS];
    uint32_t child[BN_LIMBS];

    /* "In case parse256(IL) >= n or ki = 0, proceed with the next value for
     * i." Both are negligible, and neither has ever been observed, but a
     * derivation that silently produced a different secret than the path names
     * would lose exactly the notes this module exists to recover. Hardened
     * indices stay hardened as we advance, so the walk never leaves the
     * hardened range. */
    for (uint32_t attempt = index; attempt < HARDENED; attempt++) {
        const uint32_t hardened = attempt | HARDENED;
        data[33] = (uint8_t)(hardened >> 24);
        data[34] = (uint8_t)(hardened >> 16);
        data[35] = (uint8_t)(hardened >> 8);
        data[36] = (uint8_t)hardened;

        crypto_sha512_hmac(i, parent->chain_code, DERIVE_KEY_LEN, data, sizeof(data));
        bn_load(i, il);

        if (!bn_lt(il, GROUP_ORDER_N)) {
            continue;
        }
        bn_add_mod_n(il, kpar, child);
        if (bn_is_zero(child)) {
            continue;
        }

        bn_store(child, out->key);
        memcpy(out->chain_code, i + 32, DERIVE_KEY_LEN);
        ok = true;
        break;
    }

    crypto_wipe(data, sizeof(data));
    crypto_wipe(i, sizeof(i));
    crypto_wipe(kpar, sizeof(kpar));
    crypto_wipe(il, sizeof(il));
    crypto_wipe(child, sizeof(child));
    return ok;
}

bool derive_domain(const derive_xprv_t *master, const uint8_t *domain, size_t domain_len,
                    derive_domain_t *out) {
    if (!master || !domain || !out || domain_len == 0) {
        return false;
    }

    /* cashHashingKey = m/139'/0'. LUD-05 puts its hashing key at m/138'/0 and
     * this is the same shape one purpose over, so the two trees never touch. */
    derive_xprv_t hashing;
    bool ok = derive_child_hardened(master, DERIVE_PURPOSE_LNURLCASH, &hashing) &&
              derive_child_hardened(&hashing, 0u, &hashing);
    if (!ok) {
        derive_wipe(&hashing);
        return false;
    }

    uint8_t material[32];
    hmac_sha256(hashing.key, DERIVE_KEY_LEN, domain, domain_len, material);

    for (size_t i = 0; i < 4; i++) {
        const uint32_t raw = ((uint32_t)material[i * 4] << 24) |
                             ((uint32_t)material[i * 4 + 1] << 16) |
                             ((uint32_t)material[i * 4 + 2] << 8) | (uint32_t)material[i * 4 + 3];
        /* Drop the top bit: a hardened index occupies 31 bits, and these four
         * levels are hardened like every other. LUD-05 uses the raw u32 as an
         * UNHARDENED index, where a value with the high bit set silently means
         * "hardened" instead -- a wart this scheme does not inherit, at the
         * cost of 4 bits out of 128 that were only ever separating domains.
         * The spec has to pin this either way, or two wallets will derive two
         * different trees for one mint and a restore will find nothing. */
        out->d[i] = raw & (HARDENED - 1u);
    }

    crypto_wipe(material, sizeof(material));
    derive_wipe(&hashing);
    return true;
}

bool derive_note_secret(const derive_xprv_t *master, const uint8_t *domain, size_t domain_len,
                         uint32_t index, uint8_t out[DERIVE_SECRET_LEN]) {
    if (!master || !out || (index & HARDENED)) {
        return false;
    }

    derive_domain_t dom;
    if (!derive_domain(master, domain, domain_len, &dom)) {
        return false;
    }

    derive_xprv_t node;
    bool ok = derive_child_hardened(master, DERIVE_PURPOSE_LNURLCASH, &node);
    for (size_t i = 0; ok && i < 4; i++) {
        ok = derive_child_hardened(&node, dom.d[i], &node);
    }
    ok = ok && derive_child_hardened(&node, index, &node);

    if (ok) {
        /* The derived private key's 32 bytes are the note secret verbatim --
         * nothing is hashed on top. What the mint sees is sha256 of this, as
         * `h`, exactly as it would be for a random one. */
        memcpy(out, node.key, DERIVE_SECRET_LEN);
    }
    derive_wipe(&node);
    crypto_wipe(&dom, sizeof(dom));
    return ok;
}

void derive_wipe(derive_xprv_t *xprv) {
    if (xprv) {
        crypto_wipe(xprv, sizeof(*xprv));
    }
}
