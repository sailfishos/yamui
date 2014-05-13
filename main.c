#include "minui/minui.h"
#include "os-update.h"

#include <unistd.h>

int main () {
  //const char *welcome_msg = "OS update in progress..";
  //gr_text(1,20, welcome_msg, 1);

  if ( osUpdateScreenInit() )
    return -1;

  if ( loadLogo("test") )
    return -2;

  int i = 0;
  while (i <= 100){
    osUpdateScreenShowProgress(i);
    sleep(1);
    i += 10;
  }

  osUpdateScreenExit();

  return 0;
}

