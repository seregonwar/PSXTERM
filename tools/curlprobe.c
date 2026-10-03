/*
 * psxterm-curlprobe: is libcurl itself usable inside a payload?
 *
 * curl the command exits -1 before printing anything - its argument parser
 * never runs - so the failure is in its own startup. This probe splits what
 * remains: it links the same libcurl the curl binary does, calls the same
 * initialisation and performs one real request, reporting each step. If this
 * works, the library is fine and the CLI's main/crt is at fault; if it dies
 * the same way, libcurl's initialisation is what needs the fix.
 */

#include <stdio.h>
#include <unistd.h>

#include <curl/curl.h>

int
main(void)
{
    CURL *easy;
    CURLcode rc;

    printf("curlprobe: libcurl %s\n", curl_version());
    fflush(stdout);

    if((rc = curl_global_init(CURL_GLOBAL_DEFAULT)) != CURLE_OK) {
        printf("curlprobe: global init failed (%d)\n", (int)rc);
        fflush(stdout);
        return 1;
    }
    printf("curlprobe: global init ok\n");
    fflush(stdout);

    if(!(easy = curl_easy_init())) {
        printf("curlprobe: easy init failed\n");
        fflush(stdout);
        return 1;
    }
    printf("curlprobe: easy init ok\n");
    fflush(stdout);

    curl_easy_setopt(easy, CURLOPT_URL, "https://example.com");
    curl_easy_setopt(easy, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, 20L);

    rc = curl_easy_perform(easy);
    printf("curlprobe: perform rc=%d (%s)\n", (int)rc, curl_easy_strerror(rc));
    fflush(stdout);

    if(rc == CURLE_OK) {
        long status = 0;

        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
        printf("curlprobe: HTTP %ld\n", status);
        fflush(stdout);
    }

    curl_easy_cleanup(easy);
    curl_global_cleanup();

    printf("curlprobe: done\n");
    fflush(stdout);

    return rc == CURLE_OK ? 0 : 1;
}
