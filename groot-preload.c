#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "utils.h"
#include "groot-ns.h"

static void
_groot_init_main (int argc, char *argv[])
{
  const char *enable;
  const char *env_wrap = NULL;
  char **wrapdirs = NULL;
  int num_wrapdirs = 0;

  enable = getenv ("GROOT_ENABLE");
  if (enable == NULL || *enable == 0)
    return;

  env_wrap = getenv ("GROOT_WRAPFS");
  if (env_wrap)
    {
      autofree char *data = xstrdup (env_wrap);
      char *iterator = data;

      while (iterator)
        {
          char *path = strsep (&iterator, ":");
          num_wrapdirs++;
          wrapdirs = xrealloc (wrapdirs, num_wrapdirs * sizeof (char *));
          wrapdirs[num_wrapdirs-1] = xstrdup (path);
        }
    }

  /* Don't recursively enable groot */
  unsetenv ("LD_PRELOAD");
  unsetenv ("GROOT_ENABLE");

  groot_setup_ns ((const char **)wrapdirs, num_wrapdirs);
  strfreev (wrapdirs);
}

__attribute__((section(".init_array"))) void *_groot_init_main_constructor = &_groot_init_main;
