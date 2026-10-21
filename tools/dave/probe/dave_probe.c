/*
 * Link/smoke test for libdave's C API (dave/dave.h).
 * Creates a session, inits it, produces an MLS key package, feeds a bogus
 * external sender (expects the MLS failure callback, not a crash), creates and
 * destroys encryptor/decryptor handles. Same source for host and PS5.
 */
#include <stdio.h>
#include <stdint.h>
#include <dave/dave.h>

static int failures;

static void on_mls_failure(const char* source, const char* reason, void* userData) {
    (void)userData;
    failures++;
    printf("mls failure callback: %s: %s\n", source, reason);
}

int main(void) {
    printf("libdave max protocol version: %u\n", (unsigned)daveMaxSupportedProtocolVersion());

    DAVESessionHandle session = daveSessionCreate(NULL, "ps5-probe", on_mls_failure, NULL);
    if (!session) {
        puts("daveSessionCreate returned NULL");
        return 1;
    }
    daveSessionInit(session, daveMaxSupportedProtocolVersion(), 1, "1000000000000001");

    uint8_t* keyPackage = NULL;
    size_t keyPackageLen = 0;
    daveSessionGetMarshalledKeyPackage(session, &keyPackage, &keyPackageLen);
    printf("key package: %zu bytes, first byte 0x%02x\n", keyPackageLen,
           keyPackageLen ? keyPackage[0] : 0);
    daveFree(keyPackage);

    const uint8_t bogusExternalSender[4] = {0, 1, 2, 3};
    daveSessionSetExternalSender(session, bogusExternalSender, sizeof bogusExternalSender);
    printf("bogus external sender: %d failure callback(s)\n", failures);

    DAVEEncryptorHandle encryptor = daveEncryptorCreate();
    DAVEDecryptorHandle decryptor = daveDecryptorCreate();
    printf("encryptor=%s decryptor=%s\n", encryptor ? "ok" : "null", decryptor ? "ok" : "null");
    daveDecryptorDestroy(decryptor);
    daveEncryptorDestroy(encryptor);
    daveSessionDestroy(session);
    puts("probe ok");
    return 0;
}
