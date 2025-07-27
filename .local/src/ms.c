/** 
 * Simple cli command that prints out milliseconds since epoch 
 *
 * If present within $PATH when scriptsort is run, it can be used to
 * calculate the time taken to execute a `source /path/to/script` 
 * execution.
 */
#include <stdio.h>
#include <time.h>

int main() {
  time_t now;
  struct tm *timeinfo;
  char buffer[80];

  time(&now);
  timeinfo = localtime(&now);

  strftime(buffer, sizeof(buffer), "%s", timeinfo);
  printf("%s000", buffer);

  return 0;
}
