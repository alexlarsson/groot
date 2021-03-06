#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "utils.h"
#include "groot-ns.h"

/* For some reason the regular unsetenv doesn't seem to work well in an initializer... */
static void
__unsetenv (const char *name)
{
  size_t len = strlen (name);
  char **ep;

  ep = __environ;
  while (*ep != NULL)
    {
      if (strncmp (*ep, name, len) == 0 && (*ep)[len] == '=')
        {
          char **dp = ep;
          do
            dp[0] = dp[1];
          while (*dp++);
        }
      else
        ++ep;
    }
}

static void
_groot_init_main (int argc, char *argv[])
{
  const char *debug = NULL;
  const char *env_wrap = NULL;
  const char *disabled = NULL;
  char **wrapdirs = NULL;
  int num_wrapdirs = 0;

  disabled = getenv ("GROOT_DISABLED");
  env_wrap = getenv ("GROOT_WRAPFS");
  debug = getenv ("GROOT_DEBUG");

  /* Don't recursively enable groot */
  __unsetenv ("LD_PRELOAD");

  if (disabled != NULL)
    return;

  /* If something re-enables LD_PRELOAD inside, disable it anyway */
  if (disabled == NULL)
    setenv ("GROOT_DISABLED", "1", 1);

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

  if (debug)
    enable_debuglog ();

  __debug__(("Enabling grootfs for %s - wrap %s", argv[0], env_wrap));

  groot_setup_ns ((const char **)wrapdirs, num_wrapdirs);
  strfreev (wrapdirs);
}

__attribute__((section(".init_array"))) void *_groot_init_main_constructor = &_groot_init_main;
