#include <ratelimit/ratelimit_c.h>
#include <stdio.h>

int main(void) {
  printf("version=%s\n", rl_version());
  return 0;
}
