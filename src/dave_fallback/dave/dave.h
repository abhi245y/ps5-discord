// DAVE (end-to-end voice encryption, MLS) session API.
//
// The names and signatures are discord/libdave's C API (cpp/includes/dave/dave.h),
// the subset voice.c uses. The real build links libdave and compiles against its
// header, so these declarations must not drift from it. dave_stub.c is the build
// without libdave: it reports protocol version 0, so Discord runs the call without
// E2EE and nothing here is called.
//
// Mapping from voice.c opcodes (see voice.c, dave_* functions):
//   op 4  SESSION_DESCRIPTION dave_protocol_version > 0  -> daveSessionCreate + daveSessionInit
//         (group id = channel id, self user id)
//   op 24 PREPARE_EPOCH epoch == 1                       -> daveSessionInit again (new group),
//                                                          daveSessionGetMarshalledKeyPackage -> op 26
//   op 25 MLS_EXTERNAL_SENDER                            -> daveSessionSetExternalSender
//   op 27 MLS_PROPOSALS   (payload minus the 1-byte optype) -> daveSessionProcessProposals -> op 28
//   op 29 ANNOUNCE_COMMIT (payload after the u16 transition id) -> daveSessionProcessCommit
//   op 30 WELCOME         (payload after the u16 transition id) -> daveSessionProcessWelcome
//   op 31 invalid commit/welcome                         -> sent when the above returns failure
//   op 21/22/23 transitions                              -> JSON only, no libdave call
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DAVE_EXPORT
#define DAVE_DECLARE_OPAQUE_HANDLE(x) typedef struct x##_s *x

DAVE_DECLARE_OPAQUE_HANDLE(DAVESessionHandle);
DAVE_DECLARE_OPAQUE_HANDLE(DAVECommitResultHandle);
DAVE_DECLARE_OPAQUE_HANDLE(DAVEWelcomeResultHandle);
DAVE_DECLARE_OPAQUE_HANDLE(DAVEKeyRatchetHandle);
DAVE_DECLARE_OPAQUE_HANDLE(DAVEEncryptorHandle);
DAVE_DECLARE_OPAQUE_HANDLE(DAVEDecryptorHandle);

typedef enum {
    DAVE_MEDIA_TYPE_AUDIO = 0,
    DAVE_MEDIA_TYPE_VIDEO = 1
} DAVEMediaType;

typedef enum {
    DAVE_ENCRYPTOR_RESULT_CODE_SUCCESS = 0,
    DAVE_ENCRYPTOR_RESULT_CODE_ENCRYPTION_FAILURE = 1,
    DAVE_ENCRYPTOR_RESULT_CODE_MISSING_KEY_RATCHET = 2,
    DAVE_ENCRYPTOR_RESULT_CODE_MISSING_CRYPTOR = 3,
} DAVEEncryptorResultCode;

typedef enum {
    DAVE_DECRYPTOR_RESULT_CODE_SUCCESS = 0,
    DAVE_DECRYPTOR_RESULT_CODE_DECRYPTION_FAILURE = 1,
    DAVE_DECRYPTOR_RESULT_CODE_MISSING_KEY_RATCHET = 2,
    DAVE_DECRYPTOR_RESULT_CODE_INVALID_NONCE = 3,
    DAVE_DECRYPTOR_RESULT_CODE_MISSING_CRYPTOR = 4,
} DAVEDecryptorResultCode;

// source and reason are NUL-terminated; called from the MLS code on a fatal error.
typedef void (*DAVEMLSFailureCallback)(const char *source, const char *reason, void *userData);

// Highest protocol version this build speaks. 0 means no DAVE: Identify sends 0.
DAVE_EXPORT uint16_t daveMaxSupportedProtocolVersion(void);
// Frees buffers the API returned (commit/welcome bytes, key packages).
DAVE_EXPORT void daveFree(void *ptr);

// Session. authSessionId may be "". callback may be NULL.
DAVE_EXPORT DAVESessionHandle daveSessionCreate(void *context, const char *authSessionId,
                                                DAVEMLSFailureCallback callback, void *userData);
DAVE_EXPORT void daveSessionDestroy(DAVESessionHandle session);
// Starts a group for groupId (the channel id) at protocol version; selfUserId is our id.
// Version 0 leaves the session without a group.
DAVE_EXPORT void daveSessionInit(DAVESessionHandle session, uint16_t version, uint64_t groupId,
                                 const char *selfUserId);
DAVE_EXPORT void daveSessionReset(DAVESessionHandle session);
// Credential and key of the voice server's MLS external sender (op 25).
DAVE_EXPORT void daveSessionSetExternalSender(DAVESessionHandle session, const uint8_t *externalSender,
                                              size_t length);
// MLS proposal message (op 27 after the optype byte). The result, when not NULL, is
// commit (+ welcome) bytes for op 28; free with daveFree. recognizedUserIds: the users
// in the call (op 11/13 tracking).
DAVE_EXPORT void daveSessionProcessProposals(DAVESessionHandle session, const uint8_t *proposals,
                                             size_t length, const char **recognizedUserIds,
                                             size_t recognizedUserIdsLength, uint8_t **commitWelcomeBytes,
                                             size_t *commitWelcomeBytesLength);
DAVE_EXPORT DAVECommitResultHandle daveSessionProcessCommit(DAVESessionHandle session, const uint8_t *commit,
                                                            size_t length);
DAVE_EXPORT DAVEWelcomeResultHandle daveSessionProcessWelcome(DAVESessionHandle session, const uint8_t *welcome,
                                                              size_t length, const char **recognizedUserIds,
                                                              size_t recognizedUserIdsLength);
// Our key package for op 26, free with daveFree.
DAVE_EXPORT void daveSessionGetMarshalledKeyPackage(DAVESessionHandle session, uint8_t **keyPackage,
                                                    size_t *length);
DAVE_EXPORT DAVEKeyRatchetHandle daveSessionGetKeyRatchet(DAVESessionHandle session, const char *userId);
DAVE_EXPORT void daveKeyRatchetDestroy(DAVEKeyRatchetHandle keyRatchet);

// A NULL handle from the Process* calls is a failure: send op 31.
DAVE_EXPORT bool daveCommitResultIsFailed(DAVECommitResultHandle commitResult);
DAVE_EXPORT bool daveCommitResultIsIgnored(DAVECommitResultHandle commitResult);
DAVE_EXPORT void daveCommitResultDestroy(DAVECommitResultHandle commitResult);
DAVE_EXPORT void daveWelcomeResultDestroy(DAVEWelcomeResultHandle welcomeResult);

// Media (0.5.0): the encryptor takes a ratchet from the session; the decryptor follows it per sender.
typedef enum { DAVE_CODEC_UNKNOWN = 0, DAVE_CODEC_OPUS = 1, DAVE_CODEC_H264 = 4 } DAVECodec;
DAVE_EXPORT void daveEncryptorAssignSsrcToCodec(DAVEEncryptorHandle encryptor, uint32_t ssrc, DAVECodec codecType);
DAVE_EXPORT size_t daveEncryptorGetMaxCiphertextByteSize(DAVEEncryptorHandle encryptor, DAVEMediaType mediaType,
                                                         size_t frameSize);
DAVE_EXPORT DAVEEncryptorHandle daveEncryptorCreate(void);
DAVE_EXPORT void daveEncryptorDestroy(DAVEEncryptorHandle encryptor);
DAVE_EXPORT void daveEncryptorSetKeyRatchet(DAVEEncryptorHandle encryptor, DAVEKeyRatchetHandle keyRatchet);
DAVE_EXPORT bool daveEncryptorHasKeyRatchet(DAVEEncryptorHandle encryptor);
DAVE_EXPORT void daveEncryptorSetPassthroughMode(DAVEEncryptorHandle encryptor, bool passthroughMode);
DAVE_EXPORT DAVEEncryptorResultCode daveEncryptorEncrypt(DAVEEncryptorHandle encryptor, DAVEMediaType mediaType,
                                                         uint32_t ssrc, const uint8_t *frame, size_t frameLength,
                                                         uint8_t *encryptedFrame, size_t encryptedFrameCapacity,
                                                         size_t *bytesWritten);
DAVE_EXPORT DAVEDecryptorHandle daveDecryptorCreate(void);
DAVE_EXPORT void daveDecryptorDestroy(DAVEDecryptorHandle decryptor);
DAVE_EXPORT void daveDecryptorTransitionToKeyRatchet(DAVEDecryptorHandle decryptor,
                                                     DAVEKeyRatchetHandle keyRatchet);
DAVE_EXPORT void daveDecryptorTransitionToPassthroughMode(DAVEDecryptorHandle decryptor, bool passthroughMode);
DAVE_EXPORT DAVEDecryptorResultCode daveDecryptorDecrypt(DAVEDecryptorHandle decryptor, DAVEMediaType mediaType,
                                                         const uint8_t *encryptedFrame, size_t encryptedFrameLength,
                                                         uint8_t *frame, size_t frameCapacity, size_t *bytesWritten);
