#include <copyfile.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

int main(int argc, char **argv) {
  errno = 0;
  int res = copyfile(argv[1], argv[2], NULL, COPYFILE_ALL | COPYFILE_EXCL);
  if (res == -1) {
    printf("%i:%i:%s", res, errno, strerror(errno));
  }
  return errno;
}
