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
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>
#include <err.h>
#include <uuid/uuid.h>
#include <openssl/md5.h>
#include <openssl/sha.h>
#include "sqlexpand.h"

// Low level dollar expansion parse
struct dollar_expand_s {
   unsigned char quote:1;       // Main processing flags
   unsigned char list:1;
   unsigned char file:1;
   unsigned char literal:1;
   unsigned char url:2;
   unsigned char hash:2;
   unsigned char base64:1;
   unsigned char underscore:1;
   unsigned int flags;
   int index;                   // [n] index
   const char *error;           // Error
   char *name;                  // Variable name (malloced copy)
   const char *suffix;          // Set if :x suffixes - points to first :
   char *malloced;              // Set if any malloced space has been used
};

const char *dollar_expand_name(dollar_expand_t * d)
{                               // The extracted variable name
   return d->name;
}

const char *dollar_expand_error(dollar_expand_t * d)
{                               // The current error
   return d->error;
}

unsigned char dollar_expand_literal(dollar_expand_t * d)
{                               // Flags
   return d->literal;
}

unsigned char dollar_expand_quote(dollar_expand_t * d)
{                               // Flags
   return d->quote;
}

unsigned char dollar_expand_list(dollar_expand_t * d)
{                               // Flags
   return d->list;
}

// Initialises dollar_expand_t. Passed pointer to character after the $. Returns next character after parsing $ expansion args
const char *dollar_expand_parse(dollar_expand_t * d, const char *p, unsigned int flags)
{
   memset(d, 0, sizeof(*d));
   d->flags = flags;
   char *fail(const char *e) {
      d->error = e;
      return NULL;
   }
   char curly = 0;
   if (*p == '{')
      curly = *p++;
   // Prefixes
   while (*p)
   {
      if (*p == '#')
         d->hash++;
      else if (*p == ',')
         d->list++;
      else if (*p == '*')
         d->file++;
      else if (*p == '%')
         d->literal++;
      else if (*p == '+')
         d->url++;
      else if (*p == '-')
         d->underscore++;
      else if (*p == '=')
         d->base64++;
      else
         break;
      p++;
   }
   {                            // Variable name
      const char *s = p,
          *e = p;               // The variable name
      if (strchr("$/\\@<", *e))
         e++;                   // Special one letter variable name
      else if (curly)
         while (*e && *e != '}' && *e != ':')
            e++;                // In {...}
      else if (isalpha(*e) || *e == '_')        // Simple
         while (isalnum(*e) || *e == '_')
            e++;
      p = e;
      if (e == s)
         return fail("No variable name");
      d->name = strndup(s, (int) (e - s));
   }
   // Index
   if (*p == '[')
   {
      p++;
      int index = 0;
      if (!isdigit(*p))
         return fail("Bad [n] suffix");
      while (isdigit(*p))
         index = index * 10 + *p++ - '0';
      if (!index)
         return fail("[0] not valid");
      if (*p != ']')
         return fail("Unclosed [...");
      p++;
      d->index = index;
   }
   // Suffix
   if (*p == ':' && isalpha(p[1]))
   {
      d->suffix = p;
      while (*p == ':' && isalpha(p[1]))
         p += 2;
   }
   // End
   if (curly && *p++ != '}')
      return fail("Unclosed ${...");
   return p;
}

// Passed the parsed dollar_expand_t, and a pointer to the value, returns processed value, e.g. after applying flags and suffixes, and so on
char *dollar_expand_process(dollar_expand_t * d, const char *value)
{
   if (!d)
      return NULL;
   if (d->error)
      return NULL;
   char *fail(const char *e) {
      d->error = e;
      return NULL;
   }

   if (d->file && value)
   {                            // File fetch
      if (!(d->flags & SQLEXPANDFILE))
         return fail("$@ not allowed");
      if (strstr(value, "/etc/"))
         return fail("Not playing that game, file is has /etc/");
      int i = open(value, O_RDONLY);
      if (i >= 0)
      {
         size_t len,
          got;
         FILE *o = open_memstream(&d->malloced, &len);
         char buf[16384];
         while ((got = read(i, buf, sizeof(buf))) > 0)
            fwrite(buf, got, 1, o);
         fclose(o);
         close(i);
         value = d->malloced;
      }
   }

   if (d->index && value)
   {
      int index = d->index;
      const char *p = value,
          *s = NULL;;
      while (p && --d->index)
      {
         s = strchr(p, '\t');
         if (s)
            p = s + 1;
         else
            p = NULL;
      }
      if (index || !p)
         value = "";
      else
      {
         s = strchr(p, '\t');
         if (s)
            value = strndup(p, (int) (s - p));
         else
            value = strdup(p);
         free(d->malloced);
         d->malloced = (void *) value;
      }
   }
   if (d->suffix)
   {
      const char *suffix = d->suffix;
      while (value && *suffix == ':' && isalpha(suffix[1]))
      {
         switch (suffix[1])
         {
         case 'h':             // head in path - remove all after last /
            {
               char *s = strrchr(value, '/');
               if (s)
               {
                  if (value == d->malloced)
                     *s = 0;
                  else
                  {
                     value = strndup(value, (int) (s - value));
                     free(d->malloced);
                     d->malloced = (void *) value;
                  }
               }
            }
            break;
         case 't':             // tail in path - everything from past last slash, or if no slash then unchanged
            {
               char *s = strrchr(value, '/');
               if (s)
               {
                  if (!d->malloced)
                     value = s + 1;
                  else
                  {
                     value = strdup(s + 1);
                     free(d->malloced);
                     d->malloced = (void *) value;
                  }
               }
            }
            break;
         case 'e':             // extension on file
            {
               char *s = strrchr(value, '/');
               if (!s)
                  s = (char *) value;
               s = strrchr(s, '.');
               if (s)
               {
                  if (!d->malloced)
                     value = s + 1;
                  else
                  {
                     value = strdup(s + 1);
                     free(d->malloced);
                     d->malloced = (void *) value;
                  }
               } else
                  value = "";
            }
            break;
         case 'r':             // remove extension on file
            {
               char *s = strrchr(value, '/');
               if (!s)
                  s = (char *) value;
               s = strrchr(s, '.');
               if (s)
               {
                  if (value == d->malloced)
                     *(char *) s = 0;
                  else
                  {
                     value = strndup(value, (int) (s - value));
                     free(d->malloced);
                     d->malloced = (void *) value;
                  }
               }
            }
            break;
         default:
            return fail("Unknown $...: suffix");
         }
         suffix += 2;
      }
   }

   if (d->underscore)
   {
      if (!d->malloced)
         value = d->malloced = strdup(value);
      for (char *p = (char *)value; *p; p++)
         if (*p == '\'' || *p == '"' || *p == '`')
            *p = '_';
   }

   if (d->url)
   {                            // URL encode
      char *new;
      size_t l;
      FILE *o = open_memstream(&new, &l);
      const char *v = value;
      while (*v)
      {
         if (*v == ' ' && d->url == 1)
            fputc('+', o);
         else if (*v <= ' ' || strchr("+=%\"'&<>?#!", *v) || (d->url > 1 && *v == '/'))
         {
            fputc('%', o);
            int u = d->url;
            while (u-- > 1)
               fprintf(o, "25");
            fprintf(o, "%02X", *v);
         } else
            fputc(*v, o);
         v++;
      }
      fclose(o);
      free(d->malloced);
      value = new;
      d->malloced = new;
   }

   void dobinary(const void *buf, int len) {    // Do a base64 or hex
      char *new;
      size_t l;
      FILE *o = open_memstream(&new, &l);
      const unsigned char *p = buf,
          *e = p + len;
      if (d->base64)
      {
         const char BASE64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
         unsigned int b = 0,
             v = 0;
         while (p < e)
         {
            b += 8;
            v = (v << 8) + *p++;
            while (b >= 6)
            {
               b -= 6;
               fputc(BASE64[(v >> b) & ((1 << 6) - 1)], o);
            }
         }
         if (b)
         {                      // final bits
            b += 8;
            v <<= 8;
            b -= 6;
            fputc(BASE64[(v >> b) & ((1 << 6) - 1)], o);
            while (b)
            {                   // padding
               while (b >= 6)
               {
                  b -= 6;
                  fputc('=', o);
               }
               if (b)
                  b += 8;
            }
         }
      } else
         while (p < e)
            fprintf(o, "%02x", *p++);
      fclose(o);
      free(d->malloced);
      value = new;
      d->malloced = new;
   }

   if (d->hash)
   {                            // Make a hash (hex or base64)
      if (d->hash == 1)
      {                         // MD5
         unsigned char md5buf[16];
         MD5_CTX c;
         MD5_Init(&c);
         MD5_Update(&c, value, strlen(value));
         MD5_Final(md5buf, &c);
         dobinary(md5buf, sizeof(md5buf));
      } else if (d->hash == 2)
      {
         unsigned char sha1buf[20];
         SHA_CTX c;
         SHA1_Init(&c);
         SHA1_Update(&c, value, strlen(value));
         SHA1_Final(sha1buf, &c);
         dobinary(sha1buf, sizeof(sha1buf));
      } else
         return fail("Unknown hash to use");
   } else if (d->base64)
      dobinary(value, strlen(value));   // Base64
   return (char *) value;
}

// Frees space created (including any used for return from dollar_expand_process)
void dollar_expand_free(dollar_expand_t * d)
{
   free(d->name);
   free(d->malloced);
   memset(d, 0, sizeof(*d));
}

// SQL parse
char *sqlexpand(const char *query, sqlexpandgetvar_t * getvar, const char **errp, unsigned int flags)
{
   if (errp)
      *errp = NULL;
   if (!getvar)
      getvar = getenv;          // Default
   if (!query)
      return NULL;              // Uh?
   dollar_expand_t d;
   const char *warn = NULL;
   char *expanded = NULL;
   char *malloced = NULL;       // For when variable is malloced
   size_t len;
   FILE *f = open_memstream(&expanded, &len);
   char *fail(const char *e) {  // For direct exit with error
      fclose(f);
      free(expanded);
      free(malloced);
      if (errp)
         *errp = e;
      dollar_expand_free(&d);
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
      p++;
      p = dollar_expand_parse(&d, p, flags);
      if (!p)
         return fail(dollar_expand_error(&d));

      const char *name = dollar_expand_name(&d);
      char *value = NULL;
      if (!name[1] && *name == '$')
      {
         if (!(flags & SQLEXPANDPPID))
            return fail("$$ not allowed");
         if (asprintf(&malloced, "%d", getppid()) < 0)
            err(1, "malloc");
         value = malloced;
      } else if (!name[1] && *name == '@')
      {                         // Cache feature id
         struct stat s = { };
         time_t when = 0;
         if (!stat(".", &s))
            when = s.st_mtime;
         else
            when = time(0);
         if (asprintf(&malloced, "%ld", when) < 0)
            value = malloced;
      } else if (!name[1] && *name == '<')
      {
         if (!(flags & SQLEXPANDSTDIN))
            return fail("$- not allowed");
         size_t len,
          got;
         FILE *o = open_memstream(&malloced, &len);
         char buf[16384];
         while ((got = read(fileno(stdin), buf, sizeof(buf))) > 0)
            fwrite(buf, got, 1, o);
         fclose(o);
         value = malloced;
      } else if (!name[1] && *name == '/')
      {                         // Literal '
         if (q == '\'')
            q = 0;
         else if (!q)
            q = '\'';
         fputc('\'', f);
         value = "";
      } else if (!name[1] && *name == '\\')
      {                         // Literal `
         if (q == '`')
            q = 0;
         else if (!q)
            q = '`';
         fputc('`', f);
         value = "";
      } else
         value = getvar(name);

      if (value)
      {
         value = dollar_expand_process(&d, value);
         if (!value)
            fail(dollar_expand_error(&d));
      }

      if (!value && !q && (flags & SQLEXPANDZERO))
         value = "0";
      if (!value && (flags & SQLEXPANDBLANK))
         value = "";
      if (!value)
      {
         warn = "Missing variable";
         value = "";
      }

      unsigned char literal = dollar_expand_literal(&d);
      unsigned char quote = dollar_expand_quote(&d);
      unsigned char list = dollar_expand_list(&d);
      if (literal)
      {                         // Output value (literal)
         if (quote)
            return fail("$% used with quote prefix");
         if (list)
            return fail("$% used with list prefix");
         while (*value)
         {
            if (*value == '\\')
            {
               fputc(*value++, f);
               if (*value)
                  fputc(*value++, f);
               else
               {
                  fputc('\\', f);
                  warn = "Trailing \\ in expansion";
               }
               continue;
            }
            if (q && *value == q)
               q = 0;
            else if (!q && (*value == '\'' || *value == '"' || *value == '`'))
               q = *value;
            fputc(*value++, f);
         }
      } else
      {                         // Output value (processed)
         if (!q && (list || quote))
         {
            fputc(q = '"', f);
            quote = 1;          // Ensures we close it
         }
         if (!q)
         {                      // Only allow numeric expansion
            const char *v = value;
            if (*v == '-')
               v++;
            if (!isdigit(*v))
               v = NULL;
            else
            {
               while (isdigit(*v))
                  v++;
               if (*v == '.')
               {
                  v++;
                  while (isdigit(*v))
                     v++;
               }
               if (*v == 'e' || *v == 'E')
               {
                  v++;
                  if (*v == '+' || *v == '-')
                     v++;
                  if (!isdigit(*v))
                     v = NULL;
                  else
                     while (isdigit(*v))
                        v++;
               }
            }
            if (!v || *v)
            {
               warn = "Invalid number in $ expansion";
               value = (flags & SQLEXPANDZERO) ? "0" : "";
            }
         }
         while (*value)
         {                      // Processed
            if (q && list && (*value == ',' || *value == '\t'))
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
               if (*value)
                  fputc(*value++, f);
               else
               {
                  fputc('\\', f);
                  warn = "Trailing \\ in expansion";
               }
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
         if (q && quote)
         {                      // Close
            fputc(q, f);
            q = 0;
         }
      }
      dollar_expand_free(&d);
   }
   if (q)
      return fail("Mismatched quotes");
   fclose(f);
   // Check final query
   p = expanded;
   while (*p)
   {
      if (*p == '\\')
      {
         p++;
         if (!*p)
            warn = "Trailing \\ in expanded query";
         else
            p++;
         continue;
      }
      if (q && *p == q)
         q = 0;
      else if (!q && (*p == '\'' || *p == '"' || *p == '`'))
         q = *p;
      else if (!q && (*p == '#' || (*p == '/' && p[1] == '*') || (*p == '-' && p[1] == '-' && (!p[2] || isspace(p[2])))))
      {
         free(expanded);
         if (errp)
            *errp = "Comment found in expanded query";
         return NULL;
      } else if (!q && *p == ';')
      {
         free(expanded);
         if (errp)
            *errp = "Semi colon found in expanded query";
         return NULL;
      }
      p++;
   }
   if (q)
   {
      free(expanded);
      if (errp)
         *errp = "Unclosed quoting in expanded query";
      return NULL;
   }
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
   char *expanded = sqlexpand(query, getenv, &e, flags);
   if (!expanded)
      errx(1, "Failed SQL expand: %s\n[%s]\n", e, query);
   printf("%s", expanded);
   if (e)
      warnx("Warning SQL expansion: %s\n[%s]\n[%s]\n", e, query, expanded);
   free(expanded);
   return 0;
}
#endif
