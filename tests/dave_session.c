// Host check of the real libdave, with two members in one MLS group: an external sender
// adds B to A's pending group, A commits, B joins by welcome, and an Opus-sized frame is
// encrypted by A and decrypted by B. Built only when libdave is present (see Makefile).
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <dave/dave.h>

#include "dave/external_sender_wrapper.h"

static int reported;

static void on_failure(const char *source, const char *reason, void *user)
{
    (void)user;
    reported++;
    printf("  libdave reported (%s): %s\n", source, reason);
}

int main(void)
{
    const uint64_t group = 1234567890123ull;
    const char *user_a = "1234123412341234", *user_b = "5678567856785678";
    const char *ids[] = {user_a, user_b};
    assert(daveMaxSupportedProtocolVersion() >= 1);

    DAVEExternalSenderHandle ext = daveExternalSenderCreate(group);
    DAVESessionHandle a = daveSessionCreate(NULL, "", on_failure, NULL);
    DAVESessionHandle b = daveSessionCreate(NULL, "", on_failure, NULL);
    assert(ext && a && b);

    uint8_t *ext_bytes = NULL;
    size_t ext_len = 0;
    daveExternalSenderGetMarshalledExternalSender(ext, &ext_bytes, &ext_len);
    assert(ext_bytes && ext_len > 0);
    daveSessionSetExternalSender(a, ext_bytes, ext_len);
    daveSessionSetExternalSender(b, ext_bytes, ext_len);
    daveFree(ext_bytes);
    daveSessionInit(a, 1, group, user_a);
    daveSessionInit(b, 1, group, user_b);

    uint8_t *kp_b = NULL;
    size_t kp_b_len = 0;
    daveSessionGetMarshalledKeyPackage(b, &kp_b, &kp_b_len);
    assert(kp_b && kp_b_len > 0 && kp_b[0] == 0);
    uint8_t *proposal = NULL;
    size_t proposal_len = 0;
    daveExternalSenderProposeAdd(ext, 0, kp_b, kp_b_len, &proposal, &proposal_len);
    assert(proposal && proposal_len > 0);
    daveFree(kp_b);

    uint8_t *commit_welcome = NULL;
    size_t cw_len = 0;
    daveSessionProcessProposals(a, proposal, proposal_len, ids, 2, &commit_welcome, &cw_len);
    assert(commit_welcome && cw_len > 0);
    daveFree(proposal);

    uint8_t *commit = NULL, *welcome = NULL;
    size_t commit_len = 0, welcome_len = 0;
    daveExternalSenderSplitCommitWelcome(ext, commit_welcome, cw_len, &commit, &commit_len, &welcome,
                                         &welcome_len);
    assert(commit && welcome);
    daveFree(commit_welcome);

    DAVECommitResultHandle cr = daveSessionProcessCommit(a, commit, commit_len);
    assert(cr && !daveCommitResultIsFailed(cr));
    DAVEWelcomeResultHandle wr = daveSessionProcessWelcome(b, welcome, welcome_len, ids, 2);
    assert(wr);
    daveFree(commit);
    daveFree(welcome);

    DAVEKeyRatchetHandle ratchet_a = daveSessionGetKeyRatchet(a, user_a);
    DAVEKeyRatchetHandle ratchet_b = daveSessionGetKeyRatchet(b, user_a);
    assert(ratchet_a && ratchet_b);
    DAVEEncryptorHandle enc = daveEncryptorCreate();
    DAVEDecryptorHandle dec = daveDecryptorCreate();
    daveEncryptorAssignSsrcToCodec(enc, 0, DAVE_CODEC_OPUS);
    daveEncryptorSetPassthroughMode(enc, false);
    daveEncryptorSetKeyRatchet(enc, ratchet_a);
    daveDecryptorTransitionToPassthroughMode(dec, false);
    daveDecryptorTransitionToKeyRatchet(dec, ratchet_b);

    uint8_t frame[80], sealed[256], plain[256];
    for (size_t i = 0; i < sizeof frame; i++)
        frame[i] = (uint8_t)(i * 3 + 1);
    size_t n = 0, m = 0;
    assert(daveEncryptorEncrypt(enc, DAVE_MEDIA_TYPE_AUDIO, 0, frame, sizeof frame, sealed, sizeof sealed, &n) ==
           DAVE_ENCRYPTOR_RESULT_CODE_SUCCESS);
    assert(n > sizeof frame && memcmp(sealed, frame, sizeof frame) != 0);
    assert(daveDecryptorDecrypt(dec, DAVE_MEDIA_TYPE_AUDIO, sealed, n, plain, sizeof plain, &m) ==
           DAVE_DECRYPTOR_RESULT_CODE_SUCCESS);
    assert(m == sizeof frame && memcmp(plain, frame, sizeof frame) == 0);

    printf("dave ok: two members, frame %zu -> %zu encrypted -> %zu decrypted, failures reported %d\n",
           sizeof frame, n, m, reported);
    daveCommitResultDestroy(cr);
    daveWelcomeResultDestroy(wr);
    daveKeyRatchetDestroy(ratchet_a);
    daveKeyRatchetDestroy(ratchet_b);
    daveDecryptorDestroy(dec);
    daveEncryptorDestroy(enc);
    daveSessionDestroy(a);
    daveSessionDestroy(b);
    daveExternalSenderDestroy(ext);
    return 0;
}
