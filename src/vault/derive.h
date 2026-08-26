#ifndef LNURLVAULT_DERIVE_H
#define LNURLVAULT_DERIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Seed-recoverable note secrets (LUD-25, "Seed-recoverable note secrets").
 *
 * Every secret this vault generates for a note -- a rotate/split/merge
 * preimage behind `h`/`h2`, and the mint-time secret behind a comment's hash
 * -- is drawn from the hardware RNG today (vault.c's new_note). SERVICE never
 * sees them, so that is fine for SERVICE and silently fatal for the holder: a
 * seed phrase backs up nothing that was never derived from it, so a restore
 * onto a fresh device recovers no outstanding note, and nothing surfaces the
 * loss until someone tries to redeem one whose preimage no longer exists
 * anywhere.
 *
 * Deriving them instead makes the seed the backup. The scheme mirrors LUD-05's
 * domain-hashing steps under its own purpose, so note secrets and a linkingKey
 * can never share key material:
 *
 *   cashHashingKey  = CKDpriv(master, 139' / 0')
 *   domainMaterial  = HMAC-SHA256(cashHashingKey, full SERVICE domain)
 *   d1..d4          = first 16 bytes of domainMaterial as four big-endian u32
 *   secret_i        = CKDpriv(master, 139' / d1' / d2' / d3' / d4' / i')
 *
 * ---------------------------------------------------------------------------
 * EVERY LEVEL IS HARDENED, and that is a deliberate divergence from the
 * section as drafted in TWO places: the draft hardens 139' and i' but not
 * d1..d4, and its cashHashingKey is 139'/0 where this uses 139'/0'. The
 * second one matters as much as the first -- it changes cashHashingKey,
 * therefore d1..d4, therefore every secret, so a wallet following the draft
 * and this firmware do not find each other's notes even once the d1..d4
 * question is settled. It is also the whole reason this module is arithmetic
 * rather than an elliptic-curve port.
 *
 * BIP-32's unhardened CKDpriv hashes serP(point(kpar)) -- the PARENT PUBLIC
 * KEY -- so computing one requires a secp256k1 point multiplication. This
 * firmware carries NO secp256k1 (it does carry curve25519, in monocypher,
 * for Ed25519 OTA signature checks -- different curve, nothing reusable
 * here). What it has for this is SHA-256 (sha256.c) plus SHA-512 and
 * HMAC-SHA512, which come linked in with monocypher-ed25519.c for those OTA
 * checks. Hardened CKDpriv needs exactly those plus 256-bit modular
 * addition, which is what bn_add_mod_n below is. Unhardened levels would put
 * a secp256k1 implementation into the reference vault's firmware.
 *
 * Nothing is lost by hardening. Unhardened derivation buys one thing: deriving
 * child PUBLIC keys without the private key. A note secret has no public
 * counterpart in this protocol -- SERVICE only ever sees sha256(secret), never
 * a point -- so the capability has nothing to act on here. LUD-05 leaves those
 * levels unhardened because a linkingKey is a real signing keypair; copying
 * its path shape into a scheme for opaque 32-byte preimages copies a
 * constraint that does not apply, at the cost of the devices most likely to
 * hold these notes.
 * ---------------------------------------------------------------------------
 *
 * Portable: no ESP-IDF, no storage, no RNG. Everything is passed in, so the
 * whole module is exercised natively (test/native/test_derive.c) against
 * BIP-32's own published vectors. */

#define DERIVE_SEED_MIN_LEN 16
#define DERIVE_SEED_MAX_LEN 64
#define DERIVE_KEY_LEN 32
#define DERIVE_SECRET_LEN 32

/* LUD-25's purpose, distinct from LUD-05's 138'. Hardened, as is every level
 * this module will derive. */
#define DERIVE_PURPOSE_LNURLCASH 139u

/* A BIP-32 extended private key: the 32-byte key and its chain code. Holds
 * secret material -- callers wipe it with derive_wipe() when done. */
typedef struct {
    uint8_t key[DERIVE_KEY_LEN];
    uint8_t chain_code[DERIVE_KEY_LEN];
} derive_xprv_t;

/* BIP-32 master key from a seed: HMAC-SHA512("Bitcoin seed", seed).
 *
 * False if the seed length is out of BIP-32's range, or in the negligible case
 * where the derived key is zero or >= the group order (BIP-32 says such a seed
 * is invalid; there is no next index to advance to at the master step). */
bool derive_master(const uint8_t *seed, size_t seed_len, derive_xprv_t *out);

/* One hardened BIP-32 CKDpriv step. `index` is the raw child number WITHOUT
 * the hardening bit -- this function only ever derives hardened children, so
 * it sets 0x80000000 itself and rejects an index that already carries it
 * rather than silently deriving a different child than the caller named.
 *
 * `out` may alias `parent`.
 *
 * False if the index is out of range. BIP-32's "proceed with the next value
 * for i" case (IL >= n, or a zero child key) is handled internally: with
 * hardened indices the next index is still hardened, so the walk stays inside
 * this module and a caller never sees a gap. */
bool derive_child_hardened(const derive_xprv_t *parent, uint32_t index, derive_xprv_t *out);

/* The four unsigned 32-bit values LUD-05's domain step produces, which this
 * scheme reuses under DERIVE_PURPOSE_LNURLCASH. Exposed so the path is
 * testable independently of any seed. */
typedef struct {
    uint32_t d[4];
} derive_domain_t;

/* domainMaterial = HMAC-SHA256(cashHashingKey, domain), split into four
 * big-endian u32. `domain` is the full SERVICE domain as bytes, exactly as
 * LUD-05 hashes it -- no normalisation happens here, so a caller that lets
 * "Mint.example" and "mint.example" both through will derive two different
 * trees for one service. Pass the same bytes the note's host is stored as.
 *
 * False on a NULL argument or an empty domain. */
bool derive_domain(const derive_xprv_t *master, const uint8_t *domain, size_t domain_len,
                    derive_domain_t *out);

/* secret_i for a SERVICE domain: the full path in one call. `index` is the
 * per-SERVICE counter, incremented once per secret the vault generates,
 * whether it is a mint secret or a rotate/split/merge preimage -- they are the
 * same kind of thing, an opaque 32 bytes only this device ever needs to
 * produce or reproduce.
 *
 * The derived private key's 32 bytes ARE the secret; nothing is hashed on top.
 *
 * False on a bad argument, or if `index` already carries the hardening bit. */
bool derive_note_secret(const derive_xprv_t *master, const uint8_t *domain, size_t domain_len,
                         uint32_t index, uint8_t out[DERIVE_SECRET_LEN]);

/* Wipes an extended key. Every caller holding one on the stack must call this
 * before it goes out of scope; the compiler is entitled to elide a plain
 * memset on a dead object and this is not a plain memset. */
void derive_wipe(derive_xprv_t *xprv);

#endif
