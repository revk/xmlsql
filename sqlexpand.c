// SQL variable expansion library
// This tool is specifically to allow $variable expansion in an SQL query in a safe way with correct quoting.
// Copyright ©2022 Andrews & Arnold Ltd, Adrian Kennard
// This software is provided under the terms of the GPL - see LICENSE file for more details

#include <stdio.h>
#include <string.h>
#include <popt.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>
#include <err.h>
#include <uuid/uuid.h>
#include "sqlexpand.h"

// If success, returns malloced query string, and sets *errp to NULL
// If success but warning, returns malloced query string, and sets *errp to warning text 
// If failure, returns NULL, and sets *errp to error text
char *sqlexpand(const char *query, sqlexpandgetvar_t * getvar, const char **errp, unsigned int flags)
{
   if (errp)
      *errp = NULL;
   if (!getvar)
      getvar = getenv;          // Default
   const char *warn = NULL;
   char *expanded = NULL;
   char *malloced = NULL;
   size_t len;
   FILE *f = open_memstream(&expanded, &len);
   char *fail(const char *e) {  // For direct exit
      fclose(f);
      free(expanded);
      free(malloced);
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
         p++;
         if (!*p)
            return fail("Trailing \\");
         if (*p == '\'' || *p == '"' || *p == '`')
         {
            if (!q)
               return fail("Backslashed quote out of quotes");
            if (q == *p)
               fputc(*p, f);
         } else
            fputc('\\', f);
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
      if (!q)
      {
         if (*p == '-' && p[1] == '-' && (!p[2] || isspace(p[2])))
            return fail("Comment (-- ) in SQL");
         if (*p == '/' && p[1] == '*')
            return fail("Comment (/*) in SQL");
         if (*p == '#')
            return fail("Comment (#) in SQL");
         if (*p == ';')
            return fail("Multiple commands in one SQL");
      }
      if (*p != '$')
      {                         // OK
         fputc(*p++, f);
         continue;
      }
      malloced = NULL;
      // $ expansion
      p++;
      char curly = 0;
      if (*p == '{')
         curly = *p++;
      char hash = 0,
          comma = 0,
          at = 0,
          percent = 0;
      // Prefix
      while (*p)
      {
         if (*p == '#')
            hash = 1;
         else if (*p == ',')
            comma = 1;
         else if (*p == '@')
            at = 1;
         else if (*p == '%')
            percent = 1;
         else
            break;
         p++;
      }
      // Variable
      const char *s = p,
          *e = p;               // The variable name
      if (strchr("+$-/\\", *e))
         e++;                   // Special
      else if (curly)
         while (*e && *e != '}' && *e != ':')
            e++;                // In {...}
      else if (isalpha(*e))     // Broken
         while (isalnum(*e))
            e++;
      p = e;
      // Index
      int index = 0;
      if (*p == '[')
      {
         p++;
         if (!isdigit(*p))
            return fail("Bad [n] suffix");
         while (isdigit(*p))
            index = index * 10 + *p++ - '0';
	 if(!index)return fail("[0] not valid");
         if (*p != ']')
            return fail("Unclosed [...");
         p++;
      }
      // Suffix
      const char *suffix = p;
      while (*p == ':' && isalpha(p[1]))
         p += 2;
      // End
      if (curly && *p++ != '}')
         return fail("Unclosed ${...");

      if (s == e)
         return fail("$ without no variable name");

      char *name = strndup(s, (int) (e - s));

      // Get value
      char *value = NULL;
      if (!strcmp(name, "+"))
      {
         if (!(malloced = malloc(UUID_STR_LEN + 1)))
            errx(1, "malloc");
         uuid_t u;
         uuid_generate(u);
         uuid_unparse(u, malloced);
         value = malloced;
      } else if (!strcmp(name, "$"))
      {
         if (!(flags & SQLEXPANDPPID))
            return fail("$$ not allowed");
         if (asprintf(&malloced, "%d", getppid()) < 0)
            err(1, "malloc");
         value = malloced;
      } else if (!strcmp(name, "-"))
      {
         if (!(flags & SQLEXPANDSTDIN))
            return fail("$- not allowed");
         errx(1, "Not doing stdin yet - TODO");
      } else if (!strcmp(name, "/"))
      {                         // Literal '
         if (q == '\'')
            q = 0;
         else if (!q)
            q = '\'';
         fputc('\'', f);
         value = "";
      } else if (!strcmp(name, "\\"))
      {                         // Literal `
         if (q == '`')
            q = 0;
         else if (!q)
            q = '`';
         fputc('`', f);
         value = "";
      } else
         value = getvar(name);

      if (at && value)
      {                         // File fetch
         if (!(flags & SQLEXPANDFILE))
            return fail("$@ not allowed");
         errx(1, "No $@ yet TODO");
      }

      if(index)
      {
	      errx(1,"No index yet - TODO");
      }

      while (*suffix == ':' && isalpha(suffix[1]))
      {
         switch (suffix[1])
         {
         case 'h':
         case 't':
         case 'e':
         case 'r':
         default:
            return fail("Unknown : suffix");
         }
         suffix += 2;
      }
      if (!value && !q && (flags & SQLEXPANDZERO))
         value = "0";
      if (!value && (flags & SQLEXPANDBLANK))
         value = "";
      if (!value)
         warn = "Missing variable";
      else if (percent)
      {                         // Output value (literal)
         if (hash)
            return fail("$% used with %#");
         if (comma)
            return fail("$% used with %,");
         while (*value)
            fputc(*value++, f);
      } else
      {                         // Output value (processed)
         if (!q && (comma || hash))
         {
            fputc(q = '"', f);
            hash = 1;           // Ensures we close it
         }
         while (*value)
         {                      // Processed
            if (q && comma && (*value == ',' || *value == '\t'))
            {
               fputc(q, f);
               fputc(',', f);
               fputc(q, f);
               value++;
               continue;
            }
            if (*value == '\\')
            {                   // backslash is literal
               fputc(*value++, f);
               if (!*value)
                  fputc('\\', f);
               else
                  fputc(*value++, f);
               continue;
            }
            if (q && *value == q)
            {                   // Quoted
               fputc(q, f);
               fputc(q, f);
               value++;
               continue;
            }
            fputc(*value++, f);
         }
         if (q && hash)
         {                      // Close
            fputc(q, f);
            q = 0;
         }
      }
      free(name);
      free(malloced);
   }
   if (q)
      return fail("Mismatched quotes");
   fclose(f);
   if (warn && errp)
      *errp = warn;
   return expanded;
}

#ifndef LIB
int main(int argc, const char *argv[])
{                               // Command line tool
   int dostdin = 0;
   int dofile = 0;
   int dosafe = 0;
   int dozero = 0;
   int doblank = 0;
   const char *query = NULL;
   {                            // POPT
      poptContext optCon;       // context for parsing command-line options
      const struct poptOption optionsTable[] = {
         { "stdin", 0, POPT_ARG_NONE, &dostdin, 0, "Allow stdin ($-)" },
         { "file", 0, POPT_ARG_NONE, &dofile, 0, "Allow file ($@)" },
         { "safe", 0, POPT_ARG_NONE, &dosafe, 0, "Do not allow ($%)" },
         { "zero", 0, POPT_ARG_NONE, &dozero, 0, "Do 0 for missing unquoted expansion" },
         { "blank", 0, POPT_ARG_NONE, &doblank, 0, "Allow blank for missing expansion" },
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

   unsigned int flags = 0;
   if (dostdin)
      flags |= SQLEXPANDSTDIN;
   if (dofile)
      flags |= SQLEXPANDFILE;
   if (dozero)
      flags |= SQLEXPANDZERO;
   if (doblank)
      flags |= SQLEXPANDBLANK;
   if (!dosafe)
      flags |= SQLEXPANDUNSAFE;
   const char *e = NULL;
   char *expanded = sqlexpand(query, getenv, &e, 0);
   if (!expanded)
      errx(1, "Failed SQL expand: %s\n[%s]\n", e, query);
   printf("%s", expanded);
   if (e)
      warnx("Warning SQL expansion: %s\n[%s]\n[%s]\n", e, query, expanded);
   free(expanded);
   return 0;
}
#endif
