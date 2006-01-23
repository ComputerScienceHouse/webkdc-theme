#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "webauth.h"
#include "webauthtest.h"

#define BUFSIZE 2048

int main(int argc, char *argv[])
{
    char orig_buffer[BUFSIZE];
    int s;
    TEST_VARS;

    START_TESTS(2);

    s = webauth_random_key(orig_buffer, WA_AES_128);

    TEST_OK2(WA_ERR_NONE, s);

    /*
    printf("status = [%d]\n", s);

    elen = webauth_hex_encode(orig_buffer, WA_AES_128, 
                              encoded_buffer, BUFSIZE);

    encoded_buffer[elen] = '\0';

    printf("buffer = [%s]\n", encoded_buffer);
    */

    s = webauth_random_bytes(orig_buffer, 32);

    TEST_OK2(WA_ERR_NONE, s);

    /*
    printf("status = [%d]\n", s);

    elen = webauth_hex_encode(orig_buffer, WA_AES_128, 
                              encoded_buffer, BUFSIZE);

    encoded_buffer[elen] = '\0';

    printf("buffer = [%s]\n", encoded_buffer);
    */

    END_TESTS;
    exit(NUM_FAILED_TESTS ? 1 : 0);
}
