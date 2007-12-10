#include "wholeslide.h"

int main(int argc, char **argv) {
  wholeslide_t *wsd = ws_open(argv[1]);

  

  ws_close(wsd);
}
