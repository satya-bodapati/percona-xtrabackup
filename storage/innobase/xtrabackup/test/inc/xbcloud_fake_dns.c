/* LD_PRELOAD library for xbcloud tests: getaddrinfo() of FAKE_DNS_NAME
   returns the IP address currently written in FAKE_DNS_FILE, so a test can
   move a host name to a new address while xbcloud runs. Every lookup of the
   name is logged to FAKE_DNS_LOG. Other names go to the real resolver. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

typedef int (*gai_fn)(const char *, const char *, const struct addrinfo *,
                      struct addrinfo **);

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
  gai_fn real = (gai_fn)dlsym(RTLD_NEXT, "getaddrinfo");
  const char *name = getenv("FAKE_DNS_NAME");
  const char *file = getenv("FAKE_DNS_FILE");
  if (node == NULL || name == NULL || file == NULL || strcmp(node, name) != 0)
    return real(node, service, hints, res);

  char ip[64] = "";
  FILE *f = fopen(file, "r");
  if (f != NULL) {
    if (fgets(ip, sizeof(ip), f) == NULL) ip[0] = '\0';
    fclose(f);
  }
  ip[strcspn(ip, " \r\n")] = '\0';

  const char *log = getenv("FAKE_DNS_LOG");
  if (log != NULL) {
    FILE *l = fopen(log, "a");
    if (l != NULL) {
      struct timeval tv;
      gettimeofday(&tv, NULL);
      char ts[16];
      strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&tv.tv_sec));
      fprintf(l, "%s.%03ld lookup %s -> %s\n", ts, (long)tv.tv_usec / 1000,
              node, ip);
      fclose(l);
    }
  }

  struct addrinfo h;
  memset(&h, 0, sizeof(h));
  if (hints != NULL) h = *hints;
  h.ai_flags |= AI_NUMERICHOST;
  return real(ip, service, &h, res);
}
