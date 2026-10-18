/* SPDX-License-Identifier: GPL-3.0-or-later */
/*** src type: no GUI code ***/
#include "dvdisaster.h"
#include "rs04-archive.h"
#ifdef G_OS_UNIX
#include <sys/wait.h>
#endif

/* Keep the experimental container frontend separate from the v1 GUI codec.
   Arguments are passed directly, never through a shell. The helper uses only
   Python's standard library and optionally loads our native FFT library. */
int RS04ArchiveMain(int argc, char **argv)
{  char *executable = g_find_program_in_path(argv[0]);
   char *directory = g_path_get_dirname(executable ? executable : argv[0]);
   char *helper = g_build_filename(directory, "contrib", "rs04_archive.py", NULL);
   char **child;
   const char *python = g_getenv("RS04_PYTHON");
   GError *error = NULL;
   int i, status = 0, result = 1;

   if(!g_file_test(helper, G_FILE_TEST_IS_REGULAR))
   {  g_free(helper);
      helper = g_build_filename(DOCDIR, "rs04-archive", "rs04_archive.py", NULL);
   }
#ifdef WITH_EMBEDDED_SRC_PATH_YES
   if(!g_file_test(helper, G_FILE_TEST_IS_REGULAR))
   {  g_free(helper);
      helper = g_build_filename(SRCDIR, "contrib", "rs04_archive.py", NULL);
   }
#endif
   if(!g_file_test(helper, G_FILE_TEST_IS_REGULAR))
   {  fprintf(stderr, "RS04 archival helper is missing; install the rs04-archive files.\n");
      goto done;
   }
   child = g_new0(char*, argc+6);
   child[0] = (char*)(python && *python ? python : "python3");
   child[1] = "-E";
   child[2] = "-s";
   child[3] = "-S";
   child[4] = "-B";
   child[5] = helper;
   for(i=2; i<argc; i++) child[i+4] = argv[i];
   if(!g_spawn_sync(NULL, child, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                    NULL, NULL, &status, &error))
   {  fprintf(stderr, "RS04 archival frontend requires Python 3: %s\n", error->message);
      g_error_free(error);
      result = 127;
   }
   else
   {
#ifdef G_OS_UNIX
      result = WIFEXITED(status) ? WEXITSTATUS(status) : 128+WTERMSIG(status);
#else
      result = status;
#endif
   }
   g_free(child);
done:
   g_free(helper);
   g_free(directory);
   g_free(executable);
   return result;
}
