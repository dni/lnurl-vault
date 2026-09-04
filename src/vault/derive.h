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
 * Deriving them instead makes the seed the backup. LUD-25's scheme:
 *
 *   cashHashingKey   = m/139'/0
 *   (d1, d2, d3, d4) = HMAC-SHA256(cashHashingKey, host)[0..16] as four
 *                      big-endian u32
 *   secret_i         = m/139'/d1/d2/d3/d4/i'
 *
 * ---------------------------------------------------------------------------
 * THIS MODULE DERIVES ONLY THE LAST STEP, and that is the whole design.
 *
 * `d1..d4` are RAW u32 used exactly as they fall out of the hash. BIP-32 reads
 * any index at or above 2^31 as hardened, so which of those four levels are
 * hardened is decided by the mint's own host name, and roughly half of them
 * will not be. An unhardened CKDpriv hashes serP(point(kpar)) -- the parent
 * PUBLIC key -- which is a secp256k1 point multiplication. This firmware
 * carries no secp256k1 at all (it carries curve25519, in monocypher, for
 * Ed25519 OTA signature checks: different curve, nothing reusable here), and
 * putting one in to derive four levels would be the largest single addition
 * to its attack surface for the least of it.
 *
 * It does not have to. Every unhardened level in that path sits at or above
 * `m/139'/d1/d2/d3/d4`, so a host that knows the seed derives that node once
 * per mint and hands the vault the 64 bytes. Beneath it there is only `i'`,
 * which is hardened by the spec's own notation and needs exactly what this
 * firmware already links: HMAC-SHA512 (monocypher, for OTA) plus 256-bit
 * modular addition, which is bn_add_mod_n in derive.c. No curve, no domain
 * hashing, no host-string normalisation on the device -- and no way for this
 * module to disagree with the reference wallet about a level it never walks.
 *
 * What that costs: whoever derives the node can derive every note secret this
 * vault will ever hold AT THAT MINT. It is provisioning material -- one mint's
 * subtree, not the wallet -- and in the topology this device is actually used
 * in, the host doing the provisioning is the wallet that holds the seed
 * anyway. It is not a key to hand to anything else.
 *
 * An earlier version of this module walked the whole path on-device by
 * hardening d1..d4 and masking their top bit. That derived a different tree
 * from every conforming wallet, which is worse than not implementing the
 * scheme at all: a restore finds nothing, silently, and only once the money
 * is gone.
 * ---------------------------------------------------------------------------
 *
 * Portable: no ESP-IDF, no storage, no RNG. Everything is passed in, so the
 * whole module is exercised natively (test/native/test_derive.c) against
 * BIP-32's own published vectors and lnurlcash-conformance's LUD-25 ones. */

#define DERIVE_SEED_MIN_LEN 16
#define DERIVE_SEED_MAX_LEN 64
#define DERIVE_KEY_LEN 32
#define DERIVE_SECRET_LEN 32
/* A node on the wire: key || chain code, the same 64 bytes the kit's
 * cashNodeToHex produces. Not a BIP-32 extended key -- no version bytes, no
 * depth, no parent fingerprint, no base58check -- because nothing here is
 * meant to travel as a portable xprv. */
#define DERIVE_NODE_LEN 64

/* LUD-25's purpose, distinct from LUD-05's 138'. Named for the host side and
 * for the tests; this firmware never walks it. */
#define DERIVE_PURPOSE_LNURLCASH 139u

/* A BIP-32 extended private key: the 32-byte key and its chain code. Holds
 * secret material -- callers wipe it with derive_wipe() when done. */
typedef struct {
    uint8_t key[DERIVE_KEY_LEN];
    uint8_t chain_code[DERIVE_KEY_LEN];
} derive_xprv_t;

/* BIP-32 master key from a seed: HMAC-SHA512("Bitcoin seed", seed).
 *
 * The device is provisioned with a domain node, not a seed, so nothing in the
 * firmware calls this. It is here because it is the entry point BIP-32's
 * published test vectors start from, and those vectors are the only external
 * check this arithmetic has.
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

/* Reads a provisioned domain node: 64 bytes, key || chain code.
 *
 * False on a NULL argument, or if the key is zero or at/above the group order.
 * Neither can happen to a node a conforming host derived, so a failure here is
 * a corrupted or fabricated provisioning blob rather than bad luck. */
bool derive_node_import(const uint8_t bytes[DERIVE_NODE_LEN], derive_xprv_t *out);

/* secret_i beneath a mint's domain node. `index` is the per-SERVICE counter,
 * incremented once per secret the vault generates, whether it is a mint secret
 * or a rotate/split/merge preimage -- they are the same kind of thing, an
 * opaque 32 bytes only this device ever needs to produce or reproduce. The
 * counter is not secret and is the holder's to persist and back up: a mint
 * answers a lookup for a burned note exactly as it answers one for a note it
 * never issued, so no scan can recover the vault's position for it.
 *
 * The derived private key's 32 bytes ARE the secret; nothing is hashed on top.
 * What the mint sees is sha256 of it, as `h`, exactly as for a random one.
 *
 * False on a bad argument, or if `index` already carries the hardening bit. */
bool derive_note_secret(const derive_xprv_t *domain_node, uint32_t index,
                         uint8_t out[DERIVE_SECRET_LEN]);

/* Wipes an extended key. Every caller holding one on the stack must call this
 * before it goes out of scope; the compiler is entitled to elide a plain
 * memset on a dead object and this is not a plain memset. */
void derive_wipe(derive_xprv_t *xprv);

#endif
