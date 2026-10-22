// Build without libdave: no DAVE. Every call fails or does nothing; voice.c sees
// protocol version 0 and never reaches the MLS path. See dave.h.
#include <dave/dave.h>

uint16_t daveMaxSupportedProtocolVersion(void) { return 0; }
void daveFree(void *ptr) { (void)ptr; }

DAVESessionHandle daveSessionCreate(void *context, const char *authSessionId, DAVEMLSFailureCallback callback,
                                    void *userData) {
    (void)context; (void)authSessionId; (void)callback; (void)userData;
    return NULL;
}
void daveSessionDestroy(DAVESessionHandle session) { (void)session; }
void daveSessionInit(DAVESessionHandle session, uint16_t version, uint64_t groupId, const char *selfUserId) {
    (void)session; (void)version; (void)groupId; (void)selfUserId;
}
void daveSessionReset(DAVESessionHandle session) { (void)session; }
void daveSessionSetExternalSender(DAVESessionHandle session, const uint8_t *externalSender, size_t length) {
    (void)session; (void)externalSender; (void)length;
}
void daveSessionProcessProposals(DAVESessionHandle session, const uint8_t *proposals, size_t length,
                                 const char **recognizedUserIds, size_t recognizedUserIdsLength,
                                 uint8_t **commitWelcomeBytes, size_t *commitWelcomeBytesLength) {
    (void)session; (void)proposals; (void)length; (void)recognizedUserIds; (void)recognizedUserIdsLength;
    *commitWelcomeBytes = NULL;
    *commitWelcomeBytesLength = 0;
}
DAVECommitResultHandle daveSessionProcessCommit(DAVESessionHandle session, const uint8_t *commit, size_t length) {
    (void)session; (void)commit; (void)length;
    return NULL;
}
DAVEWelcomeResultHandle daveSessionProcessWelcome(DAVESessionHandle session, const uint8_t *welcome, size_t length,
                                                  const char **recognizedUserIds, size_t recognizedUserIdsLength) {
    (void)session; (void)welcome; (void)length; (void)recognizedUserIds; (void)recognizedUserIdsLength;
    return NULL;
}
void daveSessionGetMarshalledKeyPackage(DAVESessionHandle session, uint8_t **keyPackage, size_t *length) {
    (void)session;
    *keyPackage = NULL;
    *length = 0;
}
DAVEKeyRatchetHandle daveSessionGetKeyRatchet(DAVESessionHandle session, const char *userId) {
    (void)session; (void)userId;
    return NULL;
}
void daveKeyRatchetDestroy(DAVEKeyRatchetHandle keyRatchet) { (void)keyRatchet; }
bool daveCommitResultIsFailed(DAVECommitResultHandle commitResult) { (void)commitResult; return true; }
bool daveCommitResultIsIgnored(DAVECommitResultHandle commitResult) { (void)commitResult; return false; }
void daveCommitResultDestroy(DAVECommitResultHandle commitResult) { (void)commitResult; }
void daveWelcomeResultDestroy(DAVEWelcomeResultHandle welcomeResult) { (void)welcomeResult; }

void daveEncryptorAssignSsrcToCodec(DAVEEncryptorHandle encryptor, uint32_t ssrc, DAVECodec codecType) {
    (void)encryptor; (void)ssrc; (void)codecType;
}
size_t daveEncryptorGetMaxCiphertextByteSize(DAVEEncryptorHandle encryptor, DAVEMediaType mediaType, size_t frameSize) {
    (void)encryptor; (void)mediaType;
    return frameSize;
}
DAVEEncryptorHandle daveEncryptorCreate(void) { return NULL; }
void daveEncryptorDestroy(DAVEEncryptorHandle encryptor) { (void)encryptor; }
void daveEncryptorSetKeyRatchet(DAVEEncryptorHandle encryptor, DAVEKeyRatchetHandle keyRatchet) {
    (void)encryptor; (void)keyRatchet;
}
void daveEncryptorSetPassthroughMode(DAVEEncryptorHandle encryptor, bool passthroughMode) {
    (void)encryptor; (void)passthroughMode;
}
DAVEEncryptorResultCode daveEncryptorEncrypt(DAVEEncryptorHandle encryptor, DAVEMediaType mediaType, uint32_t ssrc,
                                             const uint8_t *frame, size_t frameLength, uint8_t *encryptedFrame,
                                             size_t encryptedFrameCapacity, size_t *bytesWritten) {
    (void)encryptor; (void)mediaType; (void)ssrc; (void)frame; (void)frameLength;
    (void)encryptedFrame; (void)encryptedFrameCapacity;
    *bytesWritten = 0;
    return DAVE_ENCRYPTOR_RESULT_CODE_MISSING_CRYPTOR;
}
DAVEDecryptorHandle daveDecryptorCreate(void) { return NULL; }
void daveDecryptorDestroy(DAVEDecryptorHandle decryptor) { (void)decryptor; }
void daveDecryptorTransitionToKeyRatchet(DAVEDecryptorHandle decryptor, DAVEKeyRatchetHandle keyRatchet) {
    (void)decryptor; (void)keyRatchet;
}
void daveDecryptorTransitionToPassthroughMode(DAVEDecryptorHandle decryptor, bool passthroughMode) {
    (void)decryptor; (void)passthroughMode;
}
DAVEDecryptorResultCode daveDecryptorDecrypt(DAVEDecryptorHandle decryptor, DAVEMediaType mediaType,
                                             const uint8_t *encryptedFrame, size_t encryptedFrameLength,
                                             uint8_t *frame, size_t frameCapacity, size_t *bytesWritten) {
    (void)decryptor; (void)mediaType; (void)encryptedFrame; (void)encryptedFrameLength; (void)frame;
    (void)frameCapacity;
    *bytesWritten = 0;
    return DAVE_DECRYPTOR_RESULT_CODE_MISSING_CRYPTOR;
}
