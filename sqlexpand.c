// SQL variable expansion library
// This tool is specifically to allow $variable expansion in an SQL query in a safe way with correct quoting.
// Copyright ©2022 Andrews & Arnold Ltd, Adrian Kennard
// This software is provided under the terms of the GPL - see LICENSE file for more details

#include <stdio.h>
#include <string.h>
#include <popt.h>
#include <time.h>
#include <sys/time.h>
#include <stdlib.h>
#include <ctype.h>
#include <err.h>
#include "sqlexpand.h"

// If success, returns malloced query string, and sets *errp to NULL
// If success but warning, returns malloced query string, and sets *errp to warning text 
// If failure, returns NULL, and sets *errp to error text
char *sqlexpand(const char *query, sqlexpandgetvar_t * getvar, const char **errp)
{
   if (errp)
      *errp = NULL;
   const char *warn = NULL;
   char *expanded = NULL;
   size_t len;
   FILE *f = open_memstream(&expanded, &len);
   char *fail(const char *e) {  // For direct exit
      fclose(f);
      free(expanded);
      if (errp)
         *errp = e;
      return NULL;
   }
   char q = 0;
   const char *p = query;
   while (*p)
   {
      if (*p == '\\')
      {                         // Literal quote
         if (!p[1])
            return fail("Trailing \\");
         if (!q && (p[1] == '\'' || p[1] == '"' || p[1] == '`'))
            return fail("Backslashed quote out of quotes");
         fputc(*p++, f);
         fputc(*p++, f);
         continue;
      }
      if (q && *p == q)
      {
         q = 0;
         fputc(*p++, f);
         continue;
      }
      if (!q && (*p == '\'' || *p == '"' || *p == '`'))
      {
         q = *p;
         fputc(*p++, f);
         continue;
      }
      if (*p != '$')
      {                         // OK
         fputc(*p++, f);
         continue;
      }
      // $ expansion
      p++;
      const char *s = p,
          *e = p;               // The variable name

      // Simple for now - TODO
      while (isalpha(*e))
         e++;
      p = e;

      char *name = strndup(s, (int) (e - s));
      char *value = getenv(name);

      while (*value)
      {                         // Simple for now - TODO
         if ((q && *value == q) || *value == '\\')
            fputc(q, f);
         fputc(*value++, f);
      }

      free(name);
   }
   if (q)
      return fail("Mismatched quotes");
   fclose(f);
   if (warn && *errp)
      *errp = warn;
   return expanded;
}

#ifndef LIB
int main(int argc, const char *argv[])
{                               // Command line tool
   const char *query = NULL;
   {                            // POPT
      poptContext optCon;       // context for parsing command-line options
      const struct poptOption optionsTable[] = {
         POPT_AUTOHELP { }
      };

      optCon = poptGetContext(NULL, argc, argv, optionsTable, 0);
      poptSetOtherOptionHelp(optCon, "Query (use single quotes, duh)");

      int c;
      if ((c = poptGetNextOpt(optCon)) < -1)
         errx(1, "%s: %s\n", poptBadOption(optCon, POPT_BADOPTION_NOALIAS), poptStrerror(c));

      if (!poptPeekArg(optCon))
      {
         poptPrintUsage(optCon, stderr, 0);
         return -1;
      }
      query = poptGetArg(optCon);
      if (poptPeekArg(optCon))
      {
         poptPrintUsage(optCon, stderr, 0);
         return -1;
      }
      poptFreeContext(optCon);
   }

   const char *e = NULL;
   char *expanded = sqlexpand(query, getenv, &e);
   if (!expanded)
      errx(1, "Failed SQL expand: %s\n[%s]\n", e, query);
   printf("%s", expanded);
   if (e)
      warnx("Warning SQL expansion: %s\n[%s]\n[%s]\n", e, query, expanded);
   free(expanded);
   return 0;
}
#endif
