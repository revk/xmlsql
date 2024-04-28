// Copyright (c) 2004 Adrian Kennard
// This software is provided under the terms of the GPL v2 or later.

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <popt.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <openssl/md5.h>
#include <openssl/sha.h>
#include <dirent.h>
#include <arpa/inet.h>
#include <glob.h>
#include <sqllib.h>
#include "xmlparse.h"
#include "punycode.h"
#include "sqlexpand.h"
#include <stringdecimaleval.h>
#include <sys/mman.h>

const char BASE64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

#define	BODGEEVAL               // TODO remove some time soon

#ifndef SECURITYTAG             // Env/tag fro security
#define SECURITYTAG     *       // Default tag
#endif

#define	FLAG_RAW	1       // RAW output
#define	FLAG_MARKUP	2       // Handle normal markup
#define	FLAG_URL	4       // Link things that look like web pages
#define	FLAG_SMILE	8       // IMG things that look like smilies
#define	FLAG_SAFE	16      // Allow additional markup
#define	FLAG_JSON	32      // Escape as JSON
#define	FLAG_XML	128     // Escape as XML object
#define	FLAG_TEXTAREA	256     // Escape as input for textarea

#define	MAXTEMP 50000

#define Q(x) #x                 // Trick to quote defined fields
#define QUOTE(x) Q(x)

struct markup_s
{
   int len;
   const char *match;
};

#define m(s) {sizeof(#s)-1,#s}
const struct markup_s markup[] = {
   m (br),                      // break, self closing
   m (hr),                      // hr, self closing
   m (i),                       // italic
   m (p),                       // paragraph
   m (s),                       // strikeout
   m (b),                       // bold
   m (u),                       // underline
   m (h1),                      // heading
   m (ul),                      // unnumbered list
   m (li),                      // list item
   m (blink),                   // blink
};

#undef m

typedef struct smiley_s
{
   struct smiley_s *next;
   int base;
   char file[];
} smiley_t;
smiley_t *smiley = 0;

int safe = 0;
int debug = 0;
int comment = 0;
int noform = 0;
int showhidden = 0;
int maxinputsize = 80;
int dataurifold = 0;
const char *sqlconf = NULL;
const char *sqlhost = NULL;
unsigned int sqlport = 0;
const char *sqluser = NULL;
const char *sqlpass = NULL;
const char *sqldatabase = NULL;
const char *smileydir = NULL;
const char *security = NULL;
FILE *of = 0;
int isxml = 0;
int allowexec = 0;

#define MAXLEVEL 10
int level = 0;
MYSQL sql;
MYSQL_RES *res[MAXLEVEL];
MYSQL_ROW row[MAXLEVEL];
MYSQL_FIELD *field[MAXLEVEL];
int fields[MAXLEVEL];
char sqlconnected = { 0 };
char sqlactive[MAXLEVEL] = { 0 };

typedef struct
{
   int selectmultiple:1;
   int selectedoption:1;
   char *selectvalue;
} process_t;

// Misc
xmltoken *loadfile (char *fn);

char *
eval (char *e)
{                               // evaluate e as simple decimal add/substract, ret 0 if not valid
   static char temp[100];
   char *p;
   int sign = 0;
   int places = 0,
      digits = 0,
      l;
   long long t = 0;
   p = e;
   while (*p)
   {
      while (*p == '+' || *p == '-')
      {
         p++;
         sign = 1;
      }
//      if (!isdigit (*p)) return 0;
      int l = 0;
      while (*p == '0')
         p++;
      while (isdigit (*p) || *p == ',')
      {
         p++;
         l++;
      }
      if (l > digits)
         digits = l;
      if (*p == '.')
      {
         l = 0;
         p++;
         while (isdigit (*p))
         {
            p++;
            l++;
         }
         if (l > places)
            places = l;
      }
      if (*p && *p != '-' && *p != '+')
         return 0;              // not valid
   }
   if (!sign)
      return 0;                 // numbers only, no sum
   if (digits > 18)
      return 0;                 // Too big
   if (digits + places > 18)
      places = 18 - digits;
   p = e;
   while (*p)
   {
      long long v = 0;
      l = places;
      sign = 1;
      while (*p == '-' || *p == '+')
      {
         if (*p == '-')
            sign = -sign;
         p++;
      }
      char last = '0';
      while (*p == '0')
         p++;
      while (isdigit (*p) || *p == ',')
      {
         if (isdigit (*p))
            v = v * 10 + *p - '0';
         last = *p++;
      }
      if (*p == '.')
      {
         p++;
         while (isdigit (*p) && l > 0)
         {
            v = v * 10 + *p - '0';
            l--;
            last = *p++;
         }
         if (isdigit (*p) && *p > '5')
            v++;
         else if (*p == '5')
         {
            p++;
            while (*p == '0')
               p++;
            if (isdigit (*p) || (last & 1))
               v++;             // bankers rounding
         }
         while (isdigit (*p))
            p++;
      }
      while (l--)
         v *= 10;
      t += v * sign;
   }
   sign = 1;
   if (t < 0)
   {
      sign = -1;
      t = 0 - t;
   };
   if (!places)
      sprintf (temp + 1, "%lld", t);
   else
   {
      l = sprintf (temp + 2, "%lld", t);
      if (l <= places)
      {
         sprintf (temp + 1, "0.%0*lld", places, t);
      } else
      {
         memmove (temp + 1, temp + 2, l - places);
         temp[1 + l - places] = '.';
      }
   }
   *temp = '-';
   if (sign < 0)
      return temp;
   return temp + 1;
}

char *
readtime (char *val, time_t * when)
{
   char *fmt = 0;
   struct tm t = { 0 };
   t.tm_isdst = -1;

   if (!val)
      return 0;
   if (strlen (val) == 19)
   {                            /* YYYY-MM-DD HH:MM:SS */
      t.tm_year = 1000 * (val[0] - '0') + 100 * (val[1] - '0') + 10 * (val[2] - '0') + val[3] - '0';
      t.tm_mon = 10 * (val[5] - '0') + val[6] - '0';
      t.tm_mday = 10 * (val[8] - '0') + val[9] - '0';
      t.tm_hour = 10 * (val[11] - '0') + val[12] - '0';
      t.tm_min = 10 * (val[14] - '0') + val[15] - '0';
      t.tm_sec = 10 * (val[17] - '0') + val[18] - '0';
      fmt = "%Y-%m-%d %H:%M:%S";
   } else if (strlen (val) == 10)
   {                            /* YYYY-MM-DD */
      t.tm_year = 1000 * (val[0] - '0') + 100 * (val[1] - '0') + 10 * (val[2] - '0') + val[3] - '0';
      t.tm_mon = 10 * (val[5] - '0') + val[6] - '0';
      t.tm_mday = 10 * (val[8] - '0') + val[9] - '0';
      fmt = "%Y-%m-%d";
   } else if (strlen (val) == 8 && val[2] == ':' && val[5] == ':')
   {                            /* HH:MM:SS */
      t.tm_year = 2000;         // 2000-01-01
      t.tm_mon = 1;
      t.tm_mday = 1;
      t.tm_hour = 10 * (val[0] - '0') + val[1] - '0';
      t.tm_min = 10 * (val[3] - '0') + val[4] - '0';
      t.tm_sec = 10 * (val[6] - '0') + val[7] - '0';
      fmt = "%H:%M:%S";
   } else if (strlen (val) == 8)
   {                            /* YYYYMMDD */
      t.tm_year = 1000 * (val[0] - '0') + 100 * (val[1] - '0') + 10 * (val[2] - '0') + val[3] - '0';
      t.tm_mon = 10 * (val[4] - '0') + val[5] - '0';
      t.tm_mday = 10 * (val[6] - '0') + val[7] - '0';
      fmt = "%Y%m%d";
   } else if (strlen (val) == 14)
   {                            /* YYYYMMDDHHMMSS */
      t.tm_year = 1000 * (val[0] - '0') + 100 * (val[1] - '0') + 10 * (val[2] - '0') + val[3] - '0';
      t.tm_mon = 10 * (val[4] - '0') + val[5] - '0';
      t.tm_mday = 10 * (val[6] - '0') + val[7] - '0';
      t.tm_hour = 10 * (val[8] - '0') + val[9] - '0';
      t.tm_min = 10 * (val[10] - '0') + val[11] - '0';
      t.tm_sec = 10 * (val[12] - '0') + val[13] - '0';
      fmt = "%Y%m%d%H%M%S";
   }
   if (fmt && when && t.tm_year >= 1900 && t.tm_mon)
   {
      t.tm_year -= 1900;
      t.tm_mon--;
      *when = mktime (&t);
      if (!*when)
         fmt = 0;
   } else
      fmt = 0;
   return fmt;
}

char
matchvalue (char *tabbed, char *val)
{                               // is val in a tabbed list
   if (!val && !tabbed)
      return 1;
   if (!val || !tabbed)
      return 0;
   while (*tabbed)
   {
      char *v = val;
      while (*v && *tabbed == *v)
      {
         tabbed++;
         v++;
      }
      if (!*v && (!*tabbed || *tabbed == 9 || *tabbed == ','))
         return 1;              // match
      while (*tabbed && *tabbed != 9 && *tabbed != ',')
         tabbed++;
      if (*tabbed == 9 || *tabbed == ',')
         tabbed++;
   }
   return 0;
}

// Variable management functions:-
// Top level is SQL fields, working through nesting
// Then our set fields
// Then actual environment fields

int
fieldlen (MYSQL_FIELD * f)
{
   if (f->charsetnr == 45)
      return f->length / 4;
   if (f->charsetnr == 33)
      return f->length / 3;
   return f->length;
}

char *
getvar (const char *n, int *lenp)
{                               // Return a variable content
   int l = level;
   if (lenp)
      *lenp = 0;
   if (!n)
      return 0;
   if (*n == '$')
      n = getvar (n + 1, NULL); // Nested get, e.g. ${$X}
   // check SQL
   while (l)
   {
      int f,
        max;
      l--;
      max = fields[l];
      if (sqlactive[l])
         for (f = 0; f < max; f++)
            if (!strcmp (field[l][f].name, n))  // TODO - what of database.field
            {
               char *v = NULL;
               if (lenp)
                  *lenp = fieldlen (&field[l][f]);
               if (sqlactive[l] == 2)
               {
                  v = getenv (n);
                  if (!v)
                     v = field[l][f].def;
               } else if (row[l][f])
                  v = (char *) row[l][f];
               if (v && !strncmp (v, "0000", 4)
                   && (field[l][f].type == FIELD_TYPE_DATE || field[l][f].type == FIELD_TYPE_DATETIME
                       || field[l][f].type == FIELD_TYPE_TIMESTAMP))
                  v = "";
               return v;
            }
   }
   // last resort, environment
   return getenv (n);
}

char *
getvarexpand (const char *n)
{
   return getvar (n, NULL);
}


char *
expandd (char *buf, int len, const char *i, char sum)
{                               // expand a string (sum set if allow variables without $ and default variables to zero, for maths in eval, etc)
   char *o = buf,
      *x = buf + len - 1;
   if (!i)
      return NULL;
   {
// Does it need expanding?
      const char *p = i;
      while (*p)
      {
         if ((*p == '$' && (isalpha (p[1]) || strchr (SQLEXPANDPREFIX, p[1])))
#ifndef  BODGEEVAL
             || (sum && isalpha (*p) && (p == i || !isalnum (p[-1])))
#endif
            )
            break;
         if (*p == '\\' && p[1])
         {
            p += 2;
            continue;
         }
         p++;
      }
      if (!*p)
         return (char *) i;     // Unchanged 
   }
   char q = 0;
   // Expand
#ifndef  BODGEEVAL
   char *base = i;
#endif
   while (*i && o < x)
   {
      if ((*i == '$' && (isalpha (i[1]) || strchr (SQLEXPANDPREFIX, i[1])))
#ifndef  BODGEEVAL
          || (sum && isalpha (*i) && (i == base || !isalnum (i[-1])))
#endif
         )
      {
         const char *e;
         dollar_expand_t *d = NULL;
         char *fail (const char *e)
         {
            warnx ("Expand failed: %s\n[%s]", e, i);
            dollar_expand_free (&d);
            return NULL;
         }
         if (*i == '$')
            i++;
         d = dollar_expand_parse (&i, &e);
         if (!d)
            return fail (e);
         if (e)
            warnx ("Expand: %s\n[%s]", e, i);
         const char *name = dollar_expand_name (d);
         if (!name)
            warnx ("Unexpected no name: %s", i);
         else if (!strcmp (name, "$"))
         {
            o += sprintf (o, "%d", getppid ());
            continue;
         } else if (!name[1] && *name == '@')
         {                      // Cache feature id
            struct stat s = { };
            time_t when = 0;
            if (!stat (".", &s))
               when = s.st_mtime;
            else
               when = time (0);
            o += sprintf (o, "%ld", when);
         } else
         {
            //char literal = dollar_expand_literal(d); // TODO use to control safe expansion...
            char query = dollar_expand_query (d);
            char *v = getvar (name, 0);
            if (!v && query)
            {
               dollar_expand_free (&d);
               return NULL;     // expand fails as variable does not exist
            }
            if ((!v || !*v) && sum)
               v = "0";

            if (v && !query)
            {
               v = dollar_expand_process (d, v, &e, 0);
               if (e)
                  warnx ("Expand: %s\n[%s]", e, i);
               if (v)
               {
                  char safe = dollar_expand_underscore (d);
                  char list = dollar_expand_list (d);
                  char quote = dollar_expand_quote (d);
                  if (q)
                     quote = 0;
                  else if (quote)
                  {
                     if (o < x)
                        *o++ = (q = '"');
                  }
                  if (safe)
                  {
                     while (*v && o < x)
                     {
                        if (*v == '\'' || *v == '"')
                           *o = '_';
                        else
                           *o = *v;
                        o++;
                        v++;
                     }
                  } else if (q)
                  {
                     while (*v && o < x)
                     {
                        if (list && (*v == '\t' || *v == ','))
                        {       // List comma
                           if (q && o < x)
                              *o++ = q;
                           if (o < x)
                              *o++ = ',';
                           if (q && o < x)
                              *o++ = q;
                        } else
                        {
                           if (q && *v == q && o < x)
                              *o++ = *v;
                           if (o < x)
                              *o++ = *v;
                        }
                        v++;
                     }
                  } else
                     while (*v && o < x)
                        *o++ = *v++;    // simple expansion
                  if (quote)
                  {
                     if (o < x)
                        *o++ = q;
                     q = 0;
                  }
               }
            }
         }
         dollar_expand_free (&d);
      } else if (*i == '\\' && i[1])
      {
         *o++ = *i++;
         *o++ = *i++;
      } else                    // normal character
      {
         if (q && *i == q)
            q = 0;
         else if (*i == '\'' || *i == '"' || *i == '`')
            q = *i;
         *o++ = *i++;
      }
   }
   *o = 0;
   xmlutf8 (buf);
   return buf;
}

char *
expand (char *buf, int len, char *i)
{
   return expandd (buf, len, i, 0);
}

char *
expandz (char *buf, int len, char *i)
{
   return expandd (buf, len, i, 1);
}

static void
xputc (unsigned char c, FILE * f, int flags)
{
   if (flags & FLAG_XML)
   {
      if (c == '<')
         fprintf (f, "%s", "&lt;");
      else if (c == '>')
         fprintf (f, "%s", "&gt;");
      else if (c == '&' && !(flags & FLAG_MARKUP))
         fprintf (f, "%s", "&amp;");
      else if (c < ' ')
         fprintf (f, "&#%u;", c);
      else
         fputc (c, f);
   } else if (flags & FLAG_JSON)
   {
      if (c == '"')
         fprintf (f, "%s", "\\\"");
      else if (c == '\\')
         fprintf (f, "%s", "\\\\");
      else if (c == '\r')
         fprintf (f, "%s", "\\r");
      else if (c == '\n')
         fprintf (f, "%s", "\\n");
      else if (c == '\t')
         fprintf (f, "%s", "\\t");
      else if (c == '/')
         fprintf (f, "%s", "\\/");
      else
         fputc (c, f);
   }

   else
      fputc (c, f);
}

static inline void
xputs (char *s, FILE * f, int flags)
{
   while (*s)
      xputc (*s++, f, flags);
}

// alternative writeattr for tagwrite to use.
static void
expandwriteattr (FILE * f, char *tag, char *value)
{
   if (tag)
   {
      if (!value)
         fprintf (f, " %s", tag);
      else
      {
         char *p;
         if (!*value)
            fprintf (f, " %s=\"\"", tag);
         else
         {
            char temp[MAXTEMP];
            value = expand (temp, sizeof (temp), value);
            if (value)
            {
               fprintf (f, " %s=\"", tag);
               for (p = value; *p; p++)
               {
                  if (*p == '"')
                     fprintf (f, "&quot;");
                  else if (isxml)
                  {
                     if (*p == '<')
                        fprintf (f, "&lt;");
                     else if (*p == '>')
                        fprintf (f, "&gt;");
                     else if (*p == '&')
                        fprintf (f, "&amp;");
                     else if (*p & 0x80)
                     {          // possible UTF8
                        int n = 0;
                        if ((*p & 0xE0) == 0xC0)
                           n = 1;
                        else if ((*p & 0xF0) == 0xE0)
                           n = 2;
                        else if ((*p & 0xF8) == 0xF0)
                           n = 3;
                        else if ((*p & 0xFC) == 0xF8)
                           n = 4;
                        else if ((*p & 0xFE) == 0xFC)
                           n = 5;
                        if (n)
                        {
                           int q;
                           for (q = 0; q < n && (p[q + 1] & 0xC0) == 0x80; q++);
                           if (q < n)
                              n = 0;
                        }
                        if (n)
                        {       // valid UTF8
                           while (n--)
                              fputc (*p++, f);
                           fputc (*p, f);
                        } else
                        {       // take a guess that is ISO8859-1 to escape it
                           fputc (0xC0 + (*p >> 6), f);
                           fputc (0x80 + (*p & 0x3F), f);
                        }
                     } else     // normal ASCII
                        fputc (*p, f);
                  } else
                     fputc (*p, f);
               }
               fputc ('"', f);
            }
         }
      }
   }
}

// Alternative xmlwrite that expands all attributes
void
tagwrite (FILE * f, xmltoken * t, ...)
{                               // write token to file
   if (!t)
      return;
   if (!t->type)
   {                            // write text
      fprintf (f, "%s", t->content);
   } else if (t->type & XML_COMMENT)
   {                            // comment
      fprintf (f, "<!--%s-->", t->content);
   } else if (t->type & (XML_START | XML_END))
   {                            // write token
      fputc ('<', f);
      if (!(t->type & XML_START))
         fputc ('/', f);
      fprintf (f, "%s", t->content);
      if (t->type & XML_START)
      {                         // attributes only if start type...
         char *hide = 0;
         int a;
         if (t->attrs)
         {
            hide = malloc (t->attrs);
            if (!hide)
               errx (1, "malloc at line %d", __LINE__);
            memset (hide, 0, t->attrs);
         }
         {                      // custom attributes
            va_list ap;
            va_start (ap, t);
            while (1)
            {
               char *tag = va_arg (ap, char *),
                *val;
               if (!tag)
                  break;
               val = va_arg (ap, char *);
               for (a = 0; a < t->attrs; a++)
                  if (t->attr[a].attribute && !strcasecmp (t->attr[a].attribute, tag))
                     hide[a] = 1;
               if (val != XMLATTREMOVE)
                  xmlwriteattr (f, tag, val);
            }
            va_end (ap);
         }
         if (t->attrs)
         {                      // attributes
            for (a = 0; a < t->attrs; a++)
               if (!hide[a])
                  expandwriteattr (f, t->attr[a].attribute, t->attr[a].value);
            free (hide);
         }
      }
      if ((t->type & XML_START) && (t->type & XML_END))
      {
         fputc (' ', f);
         fputc ('/', f);
      }
      fputc ('>', f);
   }
}

// Processing functions
void
warning (xmltoken * x, char *w, ...)
{
   va_list ap;
   if (comment)
   {
      va_start (ap, w);
      fprintf (of, "<!-- \n");
      vfprintf (of, w, ap);
      fprintf (of, " -->");
      va_end (ap);
   }
   if (debug)
   {
      va_start (ap, w);
      if (x)
         fprintf (stderr, "%s:%d ", x->filename, x->line);
      vfprintf (stderr, w, ap);
      va_end (ap);
      fputc ('\n', stderr);
   }
}

void
info (xmltoken * x, char *w, ...)
{
   va_list ap;
   if (comment)
   {
      va_start (ap, w);
      fprintf (of, "<!-- ");
      vfprintf (of, w, ap);
      fprintf (of, " -->");
      va_end (ap);
   }
   if (debug)
   {
      va_start (ap, w);
      if (x)
         fprintf (stderr, "%s:%d ", x->filename, x->line);
      vfprintf (stderr, w, ap);
      va_end (ap);
      fputc ('\n', stderr);
   }
}

xmltoken *processxml (xmltoken * x, xmltoken * e, process_t * state);

char *
getattbp (xmltoken * x, char *n, char **breakpoint)
{                               // get an attribute (if it has a value)
   xmlattr *a = xmlfindattrbp (x, n, breakpoint);
   if (a && a->value)
      return a->value;
   return 0;
}

char *
checkmarkup (char *v, char *e, char *m, int l, int flags)
{                               // check balanced markup contained from just after <m> and return start of </m> at end, else return 0
   //fprintf (stderr, "Check (%.*s) [%.*s]\n", l, m, e - v, v);
   while (v < e)
   {
      if (*v == '<')
      {
         char *q = v + 1;
         if (*q == '/')
            q++;
         char *p = q;
         while (isalnum (*p) && p < e)
            p++;
         char *z = p;
         if (flags & FLAG_SAFE)
         {                      // skip attributes
            while (z < e && *z != '>')
               z++;
            if (z < e && z > v && z[-1] == '/')
               z--;
         } else
            while (isspace (*z) && z < e)
               z++;
         if (v[1] != '/' && z < e && *z == '/' && z + 1 < e && z[1] == '>')
         {                      // self ending markup
            {                   // allowed self ending markup
               v = z + 2;
               continue;
            }
         } else                 // if (*p == '>' && p < e)
         {
            if (v[1] == '/' && p - q == l && !strncasecmp (q, m, l))
            {
               //fprintf (stderr, "Found end at %.*s\n", e - v, v);
               return v;        // found end
            }
            {                   // one we consider maybe valid
               if (v[1] == '/')
                  return 0;     // unbalanced
               char *E = checkmarkup (z + 1, e, q, p - q, flags);
               if (!E)
                  return 0;     // this is not balanced
               v = E + 3 + (p - q);
               continue;        // that was balanced, carry on
            }
         }
      }
      v++;
   }
   //fprintf (stderr, "Not balanced\n");
   return 0;
}

#define getatt(x,t) getattbp(x,t,0)

void
writeoutput (xmltoken * x, char *v, char *e, int hasreplace, int flags, int ps, int maxsize, int *count)
{
   //fprintf (stderr, "Write out [%.*s]\n", e - v, v);
   if ((*count) < 0)
      return;
   int space = 1;
   while (v < e)
   {
      if (maxsize && ((*count) < 0 || (*count) >= maxsize))
      {
         (*count) = -1;
         break;
      }
      if (space && smiley && (flags & FLAG_SMILE))
      {                         // markup and xml markup smiley
         smiley_t *s;
         for (s = smiley; s; s = s->next)
            if (e - v >= s->base && !strncmp (v, s->file, s->base) && (v + s->base == e || !isalnum (v[s->base])))
               break;
         if (s)
         {
            xputc ('<', of, flags);
            fprintf (of, "img class='smiley' alt='smiley' src='%s/", smileydir);
            char *p;
            for (p = s->file; *p; p++)
            {
               if (isalpha (*p) || *p == '.')
                  fputc (*p, of);
               else
                  fprintf (of, "%%%02X", *p);
            }
            fprintf (of, "' /");
            xputc ('>', of, flags);
            v += s->base;
            (*count)++;
            continue;
         }
      }
      if (hasreplace)
      {
         char match = 2;
         int a;
         for (a = hasreplace; a < x->attrs; a++)
         {
            if (!x->attr[a].value && !strcasecmp (x->attr[a].attribute, "MATCH"))
            {                   // expand for all tag matches
               match = 1;
               continue;
            }
            if (!x->attr[a].value && !strcasecmp (x->attr[a].attribute, "REPLACE"))
            {                   // expanded later
               match = 2;
               continue;
            }
            if (match == 2 && x->attr[a].attribute)
            {
               char temptag[1000];
               char *t = expand (temptag, sizeof (temptag), x->attr[a].attribute);
               if (t)
               {
                  int l = strlen (t);
                  if (l && e - v >= l && !strncmp (v, t, l))
                  {
                     char tempval[1000];
                     char *e = expand (tempval, sizeof (tempval), x->attr[a].value);
                     if (e)
                     {
                        (*count) += strlen (e);
                        if (!maxsize || (*count) <= maxsize)
                           fprintf (of, "%s", e);
                     }
                     v += l;
                     break;
                  }
               }
            }
         }
         if (a < x->attrs)
         {
            space = 1;
            continue;           // matched and replaced
         }
      }
      if (flags & (FLAG_MARKUP | FLAG_SAFE))
      {                         // markup and xml markup
         if ((flags & FLAG_URL) && space && e - v > 4 && !strncasecmp (v, "www.", 4))
         {
            char *p = v + 4;
            while (p < e)
            {
               if (!isalnum (*p))
                  break;
               if (p < e && isalnum (*p))
                  p++;
               while (p < e && (isalnum (*p) || *p == '-'))
                  p++;
               if (p[-1] == '-')
                  break;
               if (isspace (*p) || p == e)
               {                // possible www.domain
                  (*count) += (p - v);
                  if (maxsize && (*count) > maxsize)
                     (*count) = -1;
                  else
                  {
                     xputc ('<', of, flags);
                     fprintf (of, "a href='http://%.*s/' target='_blank'", (int) (p - v), v);
                     xputc ('>', of, flags);
                     fprintf (of, "%.*s", (int) (p - v), v);
                     xputs ("</a>", of, flags);
                  }
                  v = p;
                  break;
               }
               if (*p == '.')
                  p++;
               else
                  break;
            }
         }
         if ((flags & FLAG_URL) && space
             && ((e - v > 7 && !strncasecmp (v, "http://", 7)) || (e - v > 8 && !strncasecmp (v, "https://", 8))))
         {
            char *p = v + 4;
            if (*p == 's')
               p++;
            p += 3;
            while (p < e)
            {
               if (!isalnum (*p))
                  break;
               if (p < e && isalnum (*p))
                  p++;
               while (p < e && (isalnum (*p) || *p == '-'))
                  p++;
               if (p[-1] == '-')
                  break;
               if (p == e || isspace (*p) || *p == '/')
               {                // possible www.domain
                  if (*p == '/')
                     while (p < e && !isspace (*p) && *p != '\'' && *p != '<' && *p != '>')
                        p++;    // rest of URL
                  {
                     if (!strncasecmp (p - 4, ".jpg", 4) || !strncasecmp (p - 4, ".bmp", 4) || !strncasecmp (p - 4, ".png", 4)
                         || !strncasecmp (p - 4, ".gif", 4) || !strncasecmp (p - 5, ".jpeg", 5))
                     {
                        (*count)++;
                        xputc ('<', of, flags);
                        fprintf (of, "a href='%.*s' target='_blank'", (int) (p - v), v);
                        xputs ("><", of, flags);
                        fprintf (of, "img class='pic' src='%.*s'", (int) (p - v), v);
                        xputs ("/></a>", of, flags);
                     } else if (!strncasecmp (p - 4, ".flv", 4))
                     {
                        static int n = 0;
                        n++;
                        (*count)++;
                        xputc ('<', of, flags);
                        fprintf (of, "div id=\"vp%u\" class=\"videocontainer\"\n", n);
                        xputs ("></div><script type=\"text/javascript\">", of, flags);
                        fprintf (of, "playvideo('%.*s','vp%u');", (int) (p - v), v, n);
                        xputs ("</script>", of, flags);
                     } else
                     {
                        (*count) += (p - v);
                        if (maxsize && (*count) > maxsize)
                           (*count) = -1;
                        else
                        {
                           xputc ('<', of, flags);
                           fprintf (of, "a href='%.*s' target='_blank'", (int) (p - v), v);
                           xputc ('>', of, flags);
                           fprintf (of, "%.*s", (int) (p - v), v);
                           xputs ("</a>", of, flags);
                        }
                     }
                  }
                  v = p;
                  break;
               }
               if (*p == '.')
                  p++;
               else
                  break;
            }
         }
         if (v < e && *v == '<')
         {
            char *q = v + 1;
            if (*q == '/')
               q++;
            char *p = q;
            if (isalpha (*p) && p < e)
            {
               while (isalnum (*p) && p < e)
                  p++;
               char *z = p;
               if (flags & FLAG_SAFE)
               {                // skip attributes
                  while (z < e && *z != '>')
                     z++;
                  if (z < e && z > v && z[-1] == '/')
                     z--;
               } else
                  while (isspace (*z) && z < e)
                     z++;
               if (v[1] != '/' && *z == '/' && z + 1 < e && z[1] == '>')
               {                // self ending markup
                  int n = 0;
                  if (!(flags & FLAG_SAFE))
                     for (n = 0;
                          n < sizeof (markup) / sizeof (*markup) && (markup[n].len != p - q
                                                                     || strncasecmp (markup[n].match, q, p - q)); n++);
                  if (n < sizeof (markup) / sizeof (*markup))
                  {             // allowed self ending markup
                     while (v < z + 2)
                        xputc (*v++, of, flags);
                     space = 1;
                     continue;
                  }
               } else if (*z == '>' && z < e)
               {
                  int n = 0;
                  if (!(flags & FLAG_SAFE))
                     for (n = 0;
                          n < sizeof (markup) / sizeof (*markup) && (markup[n].len != p - q
                                                                     || strncasecmp (markup[n].match, q, p - q)); n++);
                  if (n < sizeof (markup) / sizeof (*markup) && (p - q != 5 || strncasecmp ("SCRIPT", q, p - q)))
                  {             // allowed markup - maybe..
                     char *E = checkmarkup (z + 1, e, q, p - q, flags);
                     if (E)
                     {
                        if (flags & FLAG_SAFE)
                        {
                           while (v < z + 1)
                           {
                              if (tolower (*v) == 'o' && tolower (v[1] == 'n') && isalpha (v[2]))
                                 xputc ('x', of, flags);
                              if (*v == '\'')
                              {
                                 xputc (*v++, of, flags);
                                 while (v < z + 1 && *v != '\'')
                                    xputc (*v++, of, flags);
                                 if (v < z + 1)
                                    xputc (*v++, of, flags);
                              } else if (*v == '"')
                              {
                                 xputc (*v++, of, flags);
                                 while (v < z + 1 && *v != '"')
                                    xputc (*v++, of, flags);
                                 if (v < z + 1)
                                    xputc (*v++, of, flags);
                              } else
                              {
                                 while (v < z + 1 && !isspace (*v) && *v != '=' && *v != '\'' && *v != '"')
                                    xputc (*v++, of, flags);
                                 if (*v == '=')
                                    xputc (*v++, of, flags);
                              }
                              while (isspace (*v))
                                 xputc (*v++, of, flags);
                           }
                        } else
                           while (v < z + 1)
                              xputc (*v++, of, flags);
                        int flags2 = flags;
                        if (p - q == 1 && !strncasecmp ("a", q, p - q))
                           flags2 &= ~(FLAG_URL | FLAG_SMILE);
                        writeoutput (x, v, E, hasreplace, flags2, ps, maxsize, count);
                        v = E;
                        while (v < e && *v != '>')
                           xputc (*v++, of, flags);
                        if (v < e)
                           xputc (*v++, of, flags);
                        space = 1;
                        continue;
                     } else
                     {          // Not balanced
                        v = z + 1;
                        continue;
                     }
                  } else
                  {             // some other unknown markup
                     char *E = checkmarkup (z + 1, e, q, p - q, flags);
                     if (E)
                     {
                        xputs ("<span class=\"", of, flags);
                        v++;
                        while (v < p)
                           xputc (*v++, of, flags);
                        while (v <= z && v < e)
                           v++;
                        xputs ("\">", of, flags);
                        writeoutput (x, v, E, hasreplace, flags, ps, maxsize, count);
                        xputs ("</span>", of, flags);
                        v = E;
                        while (v < e && *v != '>')
                           v++;
                        if (v < e)
                           v++;
                        continue;
                     }
                  }
               }
            }
         }
         if (v < e && *v == '&' && v + 1 < e && isalpha (v[1]))
         {
            char *q = v + 1;
            while (isalnum (*q) && q < e)
               q++;
            if (*q == ';' && q < e)
            {
               (*count)++;
               if (flags & (FLAG_SAFE | FLAG_MARKUP) && strncasecmp ("gt", v + 1, q - v - 1) && strncasecmp ("lt", v + 1, q - v - 1)
                   && strncasecmp ("amp", v + 1, q - v - 1) && strncasecmp ("nbsp", v + 1, q - v - 1)
                   && strncasecmp ("minus", v + 1, q - v - 1) && strncasecmp ("deg", v + 1, q - v - 1)
                   && strncasecmp ("le", v + 1, q - v - 1) && strncasecmp ("theta", v + 1, q - v - 1)
                   && strncasecmp ("pound", v + 1, q - v - 1) && strncasecmp ("ndash", v + 1, q - v - 1))
               {                // original is html
                  v++;
                  fprintf (of, "&amp;");
               }
               while (v <= q)
                  xputc (*v++, of, flags);
               continue;
            }
         }
      }
      if (v >= e)
         break;
      (*count)++;
      space = isspace (*v);
      if (flags & (FLAG_RAW | FLAG_SAFE))
         xputc (*v, of, flags);
      else if (ps)
      {
         unsigned long C = *v++;
         if ((C & 0xE0) == 0xC0 && (v[0] & 0xC0) == 0x80)
         {
            C = (((C & 0x1F) << 6) | (v[0] & 0x3F));
            v += 1;
         } else if ((C & 0xF0) == 0xE0 && (v[0] & 0xC0) == 0x80 && (v[1] & 0xC0) == 0x80)
         {
            C = (((C & 0xF) << 12) | ((v[0] & 0x3F) << 6) | (v[1] & 0x3F));
            v += 2;
         } else if ((C & 0xF8) == 0xF0 && (v[0] & 0xC0) == 0x80 && (v[1] & 0xC0) == 0x80 && (v[2] & 0xC0) == 0x80)
         {
            C = (((C & 0x7) << 18) | ((v[0] & 0x3F) << 12) | ((v[1] & 0x3F) << 6) | (v[2] & 0x3F));
            v += 3;
         } else if ((C & 0xFC) == 0xF8 && (v[0] & 0xC0) == 0x80 && (v[1] & 0xC0) == 0x80 && (v[2] & 0xC0) == 0x80
                    && (v[3] & 0xC0) == 0x80)
         {
            C = (((C & 0x3) << 24) | ((v[0] & 0x3F) << 18) | ((v[1] & 0x3F) << 12) | ((v[2] & 0x3F) << 6) | (v[3] & 0x3F));
            v += 4;
         } else if ((C & 0xFE) == 0xFC && (v[0] & 0xC0) == 0x80 && (v[1] & 0xC0) == 0x80 && (v[2] & 0xC0) == 0x80
                    && (v[3] & 0xC0) == 0x80 && (v[4] & 0xC0) == 0x80)
         {
            C = (((C & 0x1) << 30) | ((v[0] & 0x3F) << 24) | ((v[1] & 0x3F) << 18) | ((v[2] & 0x3F) << 12) | ((v[3] & 0x3F) << 6) |
                 (v[4] & 0x3F));
            v += 5;
         }

         if (C == '\n')
            fprintf (of, "\\n");
         else if (C == '(' || C == ')' || C == '\\')
            fprintf (of, "\\%c", (char) C);
         else if (C == 8364)
            fprintf (of, "\\200");
         else if (C >= 256)
            fprintf (of, "_");
         else if (C < ' ' || C >= 127)
            fprintf (of, "\\%03o", (unsigned int) (255 & C));
         else
            fputc (C, of);
         v--;
      } else
      {
         if (*v == '\n' || (*v == '\r' && v[1] != '\n'))
         {
            if (maxsize)
               fprintf (of, " ");
            else if ((flags & FLAG_TEXTAREA) || isxml)
               fputc ('\n', of);
            else
               xputs ("<br>", of, flags);
         } else if (*v == '\f')
         {
            if (maxsize)
               fprintf (of, " ");
            else if (flags & FLAG_TEXTAREA)
               fputc ('\f', of);
            else
               xputs ("<br><hr>", of, flags);
         }
         //else if (*v == (char) 160) fprintf (of, "&nbsp;");
         else if (*v == '\'')
            fprintf (of, "&#39;");      //apos does not work in IE Except xml
         else if (*v == '&')
            fprintf (of, "&amp;");
         else if (*v == '<')
            fprintf (of, "&lt;");
         else if (*v == '>')
            fprintf (of, "&gt;");
         else if (*v & 0x80)
         {                      // utf handling
            int n = 0;
            if ((*v & 0xE0) == 0xC0)
               n = 1;
            else if ((*v & 0xF0) == 0xE0)
               n = 2;
            else if ((*v & 0xF8) == 0xF0)
               n = 3;
            else if ((*v & 0xFC) == 0xF8)
               n = 4;
            else if ((*v & 0xFE) == 0xFC)
               n = 5;
            if (n)
            {
               int q;
               for (q = 0; q < n && (v[q + 1] & 0xC0) == 0x80; q++);
               if (q < n)
                  n = 0;
            }
            if (n)
            {                   // valid UTF8
               while (n--)
                  fputc (*v++, of);
               fputc (*v, of);
            } else
            {                   // take a guess that is ISO8859-1 to escape it
               fputc (0xC0 + (*(unsigned char *) v >> 6), of);
               fputc (0x80 + (*(unsigned char *) v & 0x3F), of);
            }
         } else if (*v != '\r')
            fputc (*v, of);
      }
      v++;
   }
}


xmltoken *
dooutput (xmltoken * x, process_t * state)
{                               // do output function
   static char *breakpoint[] = { "MATCH", "REPLACE", 0 };
   char ps = 0;
   int flags = 0;
   char temp[65536];
   char tempval[65536];
   char tempname[256];
   char temptype[100];
   char *v = 0;
   char *file = getattbp (x, "FILE", breakpoint);
   char *name = getattbp (x, "NAME", breakpoint);
   char *blank = getattbp (x, "BLANK", breakpoint);
   char *missing = getattbp (x, "MISSING", breakpoint);
   char *value = getattbp (x, "VALUE", breakpoint);
   char *type = getattbp (x, "TYPE", breakpoint);
   char *format = getattbp (x, "FORMAT", breakpoint);
   char *href = getattbp (x, "HREF", breakpoint);
   char *target = getattbp (x, "TARGET", breakpoint);
   char *style = getattbp (x, "STYLE", breakpoint);
   char *class = getattbp (x, "CLASS", breakpoint);
   char *size = getattbp (x, "SIZE", breakpoint);
   xmlattr *right = xmlfindattrbp (x, "RIGHT", breakpoint);
   int hasreplace = 0;
   int maxsize = 0;

   if (xmlfindattrbp (x, "XML", breakpoint))
      flags |= FLAG_XML;

   if (file)
   {
      value = NULL;
      char *v = expand (tempval, sizeof (tempval), file);
      if (v)
      {
         FILE *i = fopen (v, "r");
         if (!i)
            err (1, "Cannot open %s (%s)", v, file);
         char buf[1024];
         size_t len = 0,
            l;
         FILE *o = open_memstream (&value, &len);
         while ((l = fread (buf, 1, sizeof (buf), i)) > 0)
            fwrite (buf, l, 1, o);
         fclose (o);
         fclose (i);
         flags |= FLAG_TEXTAREA;
      }
   }

   if (size)
   {
      char *v = expand (tempval, sizeof (tempval), size);
      if (v)
         maxsize = atoi (v);
   }
   if (!name && !value)
   {
      warning (x, "No NAME/VALUE in OUTPUT");
      return x->next;
   }
   if (name && value)
      warning (x, "NAME & VALUE in OUTPUT");
   if (!href && target)
      warning (x, "TARGET with no HREF in OUTPUT");

   if (name)
      v = getvar (expand (tempname, sizeof (tempname), name), 0);
   if (!v && value)
      v = expand (tempval, sizeof (tempval), value);

   if (type && v)
   {                            // special format controls
      type = expand (temptype, sizeof (temptype), type);
   }
   if (type && v)
   {
      char modifier = 0;
      if (strchr ("+-", *type))
         modifier = *type++;
      if ((modifier == '-' && *v != '-') || (modifier == '+' && *v == '-'))
         v = 0;
      else if (modifier == '-' && *v == '-')
         v++;
   }
   if (!format)
      format = type;            // Default, as TYPE= used to be used for these
   if (format)
   {                            // Output formatting controls
      if (!strcasecmp (format, "MARKUP"))
         flags |= FLAG_MARKUP | FLAG_URL | FLAG_SMILE;
      else if (!strcasecmp (format, "RAW"))
         flags |= FLAG_RAW;
      else if (!strcasecmp (format, "SAFE"))
         flags |= FLAG_SAFE;
      else if (!strcasecmp (format, "SAFEMARKUP"))
         flags |= FLAG_SAFE | FLAG_MARKUP | FLAG_URL | FLAG_SMILE;
      else if (!strcasecmp (format, "PS"))
         ps = 1;
      else if (!strcasecmp (format, "JSON"))
         flags |= FLAG_JSON | FLAG_RAW;
      else if (!strcasecmp (format, "TEXTAREA"))
         flags |= FLAG_TEXTAREA;
   }
   if (type && v)
   {                            // Types that change the content in various ways
      char *skiptitle (char *v)
      {                         // Skip a title on a name
         char *e = strchr (v, ' ');
         if (!e)
            e = v + strlen (v);
         if (((e - v) == 2 && !strncasecmp (v, "mr", e - v)) || //
             ((e - v) == 2 && !strncasecmp (v, "dr", e - v)) || //
             ((e - v) == 2 && !strncasecmp (v, "ms", e - v)) || //
             ((e - v) == 3 && !strncasecmp (v, "mrs", e - v)) ||        //
             ((e - v) == 3 && !strncasecmp (v, "rev", e - v)) ||        //
             ((e - v) == 4 && !strncasecmp (v, "miss", e - v)) ||       //
             ((e - v) == 6 && !strncasecmp (v, "master", e - v)))
         {
            while (*e && isspace (*e))
               e++;
            return e;
         }
         return v;
      }
      void initialise (char *v)
      {                         // Initialise
         while (*v)
         {
            if (*v == 'M' && v[1] == 'c' && isupper (v[2]))
            {
               v += 2;
               continue;
            }
            if (*v == 'M' && v[1] == 'a' && v[2] == 'c' && isupper (v[3]))
            {
               v += 3;
               continue;
            }
            *v = toupper (*v);
            v++;
            while (*v && isalpha (*v))
            {
               *v = tolower (*v);
               v++;
            }
            while (*v && !isalpha (*v))
               v++;
         }
      }
      char addtz = 0;
      if (!strcasecmp (type, "TIMESTAMP"))
         type = "%d %b %Y %H:%M:%S";
      else if (!strcasecmp (type, "DATE"))
         type = ((flags & FLAG_XML) ? "%F" : "%d %b %Y");
      else if (!strcasecmp (type, "DATETIME"))
      {
         type = ((flags & FLAG_XML) ? "%FT%T" : "%d %b %Y %H:%M:%S");
         if (flags & FLAG_XML)
            addtz = 1;
      }
      if (strchr (type, '%') && (strlen (v) == 19 || strlen (v) == 14 || strlen (v) == 8 || strlen (v) == 10))
      {                         /* time stamp print */
         time_t when;
         if (!readtime (v, &when))
            v = "";
         else
         {
            struct tm t = *localtime (&when);
            strftime ((v = temp), sizeof (temp), type, &t);
            if (addtz)
            {                   // time zone suffix RFC3339 format
#ifdef __CYGWIN__
               // tm_gmtoff is a BSD extension which Linux has but Cygwin doesn't
               struct tm g = *gmtime (&when);
               int o = ((t.tm_hour * 60) + t.tm_min) * 60 + t.tm_sec;
               o -= (((g.tm_hour * 60) + g.tm_min) * 60 + g.tm_sec);
               if (t.tm_wday != g.tm_wday)
               {
                  if (t.tm_wday - g.tm_wday == 1 || t.tm_wday - g.tm_wday == -6)
                     o += 86400;
                  else
                     o -= 86400;
               }
#else
               int o = t.tm_gmtoff;
#endif
               if (!o)
                  strcat (temp, "Z");
               else
               {
                  if (o < 0)
                  {
                     strcat (temp, "-");
                     o = 0 - o;
                  } else
                     strcat (temp, "+");
                  sprintf (temp + strlen (temp), "%02u:%02u", o / 60 / 60, o / 60 % 60);
               }
            }
         }
      } else if (!strcasecmp (type, "INTERVAL"))
      {
         int i = atoi (v);

         if (i >= 86400)
            sprintf (temp, "%d:%02d:%02d:%02d", i / 86400, i / 3600 % 24, i / 60 % 60, i % 60);
         else if (i >= 3600)
            sprintf (temp, "%d:%02d:%02d", i / 3600, i / 60 % 60, i % 60);
         else if (i >= 60)
            sprintf (temp, "%d:%02d", i / 60, i % 60);
         else
            sprintf (temp, "%d", i);
         v = temp;
      } else if (!strcasecmp (type, "RECENT"))
      {
         time_t when;

         if (!readtime (v, &when))
         {
            if (blank)
               printf ("%s", expand (tempval, sizeof (tempval), blank));
         } else
         {
            time_t now = time (0);
            struct tm w = *localtime (&when);
            struct tm n = *localtime (&now);
            if (w.tm_year != n.tm_year)
               type = "%d %b %Y %H:%M:%S";
            else if (n.tm_yday == w.tm_yday)
               type = "Today %H:%M:%S";
            else if (n.tm_yday == w.tm_yday + 1)
               type = "Yesterday %H:%M:%S";
            else if (n.tm_yday + 1 == w.tm_yday)
               type = "Tomorrow %H:%M:%S";
            else if (n.tm_yday == w.tm_yday + 2 || n.tm_yday + 2 == w.tm_yday)
               type = "%A %H:%M:%S";
            else
               type = "%_d %b %H:%M:%S";
            strftime (temp, sizeof (temp), type, &w);
            if (strlen (temp) > 9 && !strcmp (temp + strlen (temp) - 9, " 00:00:00"))
               temp[strlen (temp) - 9] = 0;
            v = temp;
         }
      } else if (!strcasecmp (type, "MEGA") && strlen (v) < 100)
      {                         // E/P/T/G/M/k suffix, typically used with BIGINT
         int l = 0,
            s = 0;
         char *p = v,
            *o = temp;
         while (*p == ' ')
            p++;
         if (*p == '-')
            *o++ = *p++;
         while (isdigit (p[l]))
            l++;
         do
         {
            *o++ = *p++;
            l--;
            s++;
         }
         while (l && (l % 3 || l > 18));
         if (l && s < 3)
         {
            *o++ = '.';
            while (s < 3)
            {
               *o++ = *p++;
               s++;
            }
         }
         if (l == 3)
         {
            if (xmlfindattrbp (x, "KELVIN", breakpoint))
               *o++ = 'K';      // bodge
            else
               *o++ = 'k';      // Kilo is lower case k, else would be Kelvin
         }
         if (l == 6)
            *o++ = 'M';
         if (l == 9)
            *o++ = 'G';
         if (l == 12)
            *o++ = 'T';
         if (l == 15)
            *o++ = 'P';
         if (l == 18)
            *o++ = 'E';
         *o = 0;
         v = temp;
      } else if (!strcasecmp (type, "MEBI") && strlen (v) < 100)
      {                         // Ei/Pi/Ti/Gi/Mi/ki suffix, typically used with BIGINT
         char *p = v,
            *o = temp;
         while (*p == ' ')
            p++;
         if (*p == '-')
            *o++ = *p++;
         unsigned long long int n = strtoull (p, NULL, 10);
         char suffix = 0;
         if (n >= 1000ULL * (1ULL << 40ULL))
         {
            suffix = 'P';
            n = ((n * 100ULL) >> 50ULL);
         } else if (n >= 1000ULL * (1ULL << 30ULL))
         {
            suffix = 'T';
            n = ((n * 100ULL) >> 40ULL);
         } else if (n >= 1000ULL * (1ULL << 20ULL))
         {
            suffix = 'G';
            n = ((n * 100ULL) >> 30ULL);
         } else if (n >= 1000ULL * (1ULL << 10ULL))
         {
            suffix = 'M';
            n = ((n * 100ULL) >> 20ULL);
         } else if (n >= 1000ULL)
         {
            if (!xmlfindattrbp (x, "FAKESI", breakpoint) || xmlfindattrbp (x, "KELVIN", breakpoint))
               suffix = 'K';    // Kibi is Ki, somewhat inconsitently with k for Kilo.
            else
               suffix = 'k';
            n = ((n * 100ULL) >> 10ULL);
         } else
            n *= 100ULL;
         if (n >= 10000)
            o += sprintf (o, "%llu", n / 100ULL);
         else if (n >= 1000)
            o += sprintf (o, "%llu.%llu", n / 100ULL, n / 10ULL % 10ULL);
         else if (suffix)
            o += sprintf (o, "%llu.%02llu", n / 100ULL, n % 100ULL);
         else
            o += sprintf (o, "%llu", n / 100ULL);
         if (suffix)
         {
            *o++ = suffix;
            if (!xmlfindattrbp (x, "FAKESI", breakpoint))
               *o++ = 'i';
         }
         *o = 0;
         v = temp;
      } else if ((!strcasecmp (type, "COMMA") || !strncasecmp (type, "CASH", 4)) && strlen (v) < 100 && *v)
      {                         // comma separated number (typically for use with BIGINT or money)
         int l;
         char *p = v,
            *o = temp,
            *mal = NULL;
         while (*p == ' ')
            p++;
         if (!strncasecmp (type, "CASH", 4))
         {
            flags |= FLAG_RAW;
            if (*p == '-')
            {
               if (style)
                  o += sprintf (o, "<span style=\"color:#C00\">");
               o += sprintf (o, "-");
               p++;
            }
            if (!type[4] || !strcasecmp (type + 4, "GBP"))
               o += sprintf (o, isxml ? "£" : "&pound;");
            else if (!strcasecmp (type + 4, "USD"))
               o += sprintf (o, "$");
            else if (!strcasecmp (type + 4, "EUR"))
               o += sprintf (o, isxml ? "€" : "&euro;");
            else if (!strcasecmp (type + 4, "AUD"))
               o += sprintf (o, "$");
            else if (!strcasecmp (type + 4, "NZD"))
               o += sprintf (o, "$");
            else if (!strcasecmp (type + 4, "AED"))
               o += sprintf (o, "<small>&#x62f;&#x2e;&#x625;</small>");
            else
               o += sprintf (o, "<small>%s</small>", type + 4);
         } else
         {
            if (*p == '-')
               *o++ = *p++;
         }
         for (l = 0; isdigit (p[l]); l++);
         while (l)
         {
            do
            {
               *o++ = *p++;
               l--;
            }
            while (l && l % 3);
            if (l)
               *o++ = ',';
         }
         if (!strncasecmp (type, "CASH", 4) && (!*p || *p == '.'))
         {                      // cash
            l = 0;
            while (*p && l < 3)
            {
               *o++ = *p++;
               l++;
            }
            if (!l)
            {
               *o++ = '.';
               l++;
            }
            while (l < 3)
            {
               *o++ = '0';
               l++;
            }
            if (*v == '-')
            {
               if (style)
                  o += sprintf (o, "</span>");
               else
                  style = "color:#C00";
            }
         } else
         {                      // not cash
            while (*p)
               *o++ = *p++;
         }
         *o = 0;
         v = temp;
         if (mal)
            free (mal);
      } else if (!strcasecmp (type, "FLOOR") && v && strlen (v) < 100)
      {
         char *i = v,
            *o = temp;
         while (*i && *i != '.')
            *o++ = *i++;
         *o = 0;
         v = temp;
      } else if (!strcasecmp (type, "TRIM") && v && strlen (v) < 100)
      {
         char *p = v + strlen (v),
            *d = strchr (v, '.'),
            *o = temp;
         if (d)
         {
            while (p > d && p[-1] == '0')
               p--;
            if (p == d + 1)
               p--;
         }
         while (v < p)
            *o++ = *v++;
         *o = 0;
         v = temp;
      } else if (!strcasecmp (type, "PENCE") && v && strlen (v) < 100)
      {
         char *p = v + strlen (v),
            *d = strchr (v, '.'),
            *s = 0,
            *o = temp;
         if (d)
         {
            while (p > d && p[-1] == '0')
               p--;
            if (p == d + 1)
               p--;
         }
         if (d && p == d + 2 && d[1] == '5')
            p = d, s = "½";
         if (d && p == d + 3 && d[1] == '2' && d[2] == '5')
            p = d, s = "¼";
         if (d && p == d + 3 && d[1] == '7' && d[2] == '5')
            p = d, s = "¾";
         if (s && *v == '-' && v[1] == '0' && v[2] == '.')
            *o++ = '-';
         else if (!(s && *v == '0' && v[1] == '.'))
            while (v < p)
               *o++ = *v++;
         if (s)
            while (*s)
               *o++ = *s++;
         *o = 0;
         v = temp;
         flags |= FLAG_RAW;
      } else if (!strcasecmp (type, "UKTEL"))
      {
         if ((*v == '+' && v[1] == '4' && v[2] == '4') || (v[0] == '0' && v[1] > '0'))
         {                      // looks like may be a UK phone number...
            char *o = temp;
            *o++ = '0';
            if (*v == '+')
               v += 3;
            else
               v++;
            if (v[0] == '2' && v[2] > '1')
            {
               *o++ = *v++;
               *o++ = *v++;
               *o++ = ' ';
            } else if (v[0] == '1' && (v[1] == '1' || v[2] == '1') && v[3] > '1')
            {
               *o++ = *v++;
               *o++ = *v++;
               *o++ = *v++;
               *o++ = ' ';
            } else if ((v[0] == '1' && v[4] > '1') || v[0] == '3' || v[0] == '5' || v[0] == '8' || v[0] == '7')
            {
               *o++ = *v++;
               *o++ = *v++;
               *o++ = *v++;
               *o++ = *v++;
               *o++ = ' ';
            }
            int n = 0;
            for (n = 0; n < 10 && isdigit (v[n]); n++);
            if (n == 7 || (n == 6 && v[0] == v[3] && v[1] == v[4] && v[2] == v[5]) || (n == 6 && v[1] == '0' && v[2] == '0'))
            {
               *o++ = *v++;
               *o++ = *v++;
               *o++ = *v++;
               *o++ = ' ';
            } else if (n == 8)
            {
               *o++ = *v++;
               *o++ = *v++;
               *o++ = *v++;
               *o++ = *v++;
               *o++ = ' ';
            } else if (n == 6 && v[0] == v[2] && v[2] == v[4] && v[1] == v[3] && v[3] == v[5])
            {
               *o++ = *v++;
               *o++ = *v++;
               *o++ = ' ';
               *o++ = *v++;
               *o++ = *v++;
               *o++ = ' ';
            }

            while (*v && o < temp + sizeof (temp) - 1)
               *o++ = *v++;
            *o = 0;
            v = temp;
         }
      } else if (!strcasecmp (type, "MASK"))
      {
         unsigned int ip = atoi (v);
         if (!ip)
            ip = 32;
         ip = ~((1 << (32 - ip)) - 1);
         sprintf ((v = temp), "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255);
      } else if (!strcasecmp (type, "IP"))
      {
         char b[16],
           s[40] = "?";
         if (inet_pton (AF_INET6, v, b) > 0)
            inet_ntop (AF_INET6, b, s, sizeof (s));
         else if (inet_pton (AF_INET, v, b) > 0)
            inet_ntop (AF_INET, b, s, sizeof (s));
         else if (isdigit (*v))
         {
            unsigned int ip = atoi (v);
            sprintf (s, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 0xFF, ip >> 8 & 0xFF, ip & 0xFF);
         }
         sprintf ((v = temp), "%s", s);
      } else if (!strcasecmp (type, "HEX"))
      {
         unsigned long long x = strtoull (v, NULL, 10);
         sprintf ((v = temp), "%llX", x);
      } else if (!strcasecmp (type, "YEARS"))
      {
         time_t now = time (0);
         time_t when = 0;
         char *p;
         for (p = v; isdigit (*p); p++);
         if (!*p)
            when = now - atoi (v);
         else if (!readtime (v, &when))
            when = 0;
         if (!when)
         {
            if (blank)
               printf ("%s", expand (tempval, sizeof (tempval), blank));
         } else
         {
            struct tm w = *localtime (&when);
            struct tm n = *localtime (&now);
            int Y = n.tm_year - w.tm_year;
            if (n.tm_mon < w.tm_mon || (n.tm_mon == w.tm_mon && n.tm_mday < w.tm_mday))
               Y--;
            sprintf ((v = temp), "%u", Y);
         }
      } else if (!strcasecmp (type, "AGE"))
      {                         // simple relative age
         time_t now = time (0);
         time_t when = 0;
         char *p;
         for (p = v; isdigit (*p); p++);
         if (!*p)
            when = now - atoi (v);
         else if (!readtime (v, &when))
            when = 0;
         if (!when)
         {
            if (blank)
               printf ("%s", expand (tempval, sizeof (tempval), blank));
         } else
         {
            const char *frac[] = { "", "¼", "½", "¾" };
            if (when > now)
            {
               time_t temp = when;
               when = now;
               now = temp;
            }
            time_t d = now - when;
            if (d < 60)
               sprintf ((v = temp), "%u second%s", (int) d, (d == 1) ? "" : "s");
            else if (d < 3600)
            {
               d /= 15;
               sprintf ((v = temp), "%u%s minute%s", (int) (d / 4), frac[d % 4], d == 4 ? "" : "s");
            } else if (d < 86400)
            {
               d /= 900;
               sprintf ((v = temp), "%u%s hour%s", (int) (d / 4), frac[d % 4], d == 4 ? "" : "s");
            } else if (d < 86400 * 31)
            {
               d /= 21600;
               sprintf ((v = temp), "%u%s day%s", (int) (d / 4), frac[d % 4], d == 4 ? "" : "s");
            } else
            {
               now -= (d % 86400);
               struct tm w = *localtime (&when);
               struct tm n = *localtime (&now);
               int Y = n.tm_year - w.tm_year;
               int M = n.tm_mon - w.tm_mon;
               int D = n.tm_mday - w.tm_mday;
               if (D < 0)
               {
                  D += 30;
                  M--;
               }
               if (M < 0)
               {
                  M += 12;
                  Y--;
               }
               if (Y)
               {
                  d = Y * 4 + M / 3;
                  sprintf ((v = temp), "%u%s year%s", (int) (d / 4), frac[d % 4], d == 4 ? "" : "s");
               } else
               {
                  d = M * 4 + D / 7;
                  sprintf ((v = temp), "%u%s month%s", (int) (d / 4), frac[d % 4], d == 4 ? "" : "s");
               }
            }
            flags |= FLAG_RAW;
         }
      } else if (!strcasecmp (type, "IDN"))
      {                         // IDN to UTF convert
         char *o = temp;
         char *i = v;
         while (*i)
         {
            char *q = i;
            if (!strncasecmp (q, "xn--", 4))
            {
               while (*q && *q != '.')
                  q++;
               if (!*q || *q == '.')
               {                // convert
                  punycode_uint len = 63,
                     p;
                  punycode_uint out[len];
                  if (!punycode_decode (q - i - 4, i + 4, &len, out, NULL))
                  {
                     for (p = 0; p < len; p++)
                     {
                        int u = out[p];
                        if (u >= 0x4000000)
                        {
                           *o++ = 0xfC + (u >> 30);
                           *o++ = 0x80 + ((u >> 24) & 0x3F);
                           *o++ = 0x80 + ((u >> 18) & 0x3F);
                           *o++ = 0x80 + ((u >> 12) & 0x3F);
                           *o++ = 0x80 + ((u >> 6) & 0x3F);
                           *o++ = 0x80 + (u & 0x3F);
                        } else if (u >= 0x200000)
                        {
                           *o++ = 0xf8 + (u >> 24);
                           *o++ = 0x80 + ((u >> 18) & 0x3F);
                           *o++ = 0x80 + ((u >> 12) & 0x3F);
                           *o++ = 0x80 + ((u >> 6) & 0x3F);
                           *o++ = 0x80 + (u & 0x3F);
                        } else if (u >= 0x10000)
                        {
                           *o++ = 0xF0 + (u >> 18);
                           *o++ = 0x80 + ((u >> 12) & 0x3F);
                           *o++ = 0x80 + ((u >> 6) & 0x3F);
                           *o++ = 0x80 + (u & 0x3F);
                        } else if (u >= 0x800)
                        {
                           *o++ = 0xE0 + (u >> 12);
                           *o++ = 0x80 + ((u >> 6) & 0x3F);
                           *o++ = 0x80 + (u & 0x3F);
                        } else if (u >= 0x80 || u == '<' || u == '>' || u == '&')
                        {
                           *o++ = 0xC0 + (u >> 6);
                           *o++ = 0x80 + (u & 0x3F);
                        } else
                           *o++ = u;
                     }
                     i = q;
                  } else        // not decoded
                     while (*i && *i != '.')
                        *o++ = *i++;
               }
            } else
               while (*i && *i != '.')
                  *o++ = *i++;
            if (*i == '.')
               *o++ = *i++;
         }
         *o = 0;
         v = temp;
      } else if (!strcasecmp (type, "RFC2047"))
      {
         flags |= FLAG_SAFE;
         char *i;
         for (i = v; *i && *i >= ' ' && *i <= 126; i++);
         if (*i)
         {                      // translate
            char *o = temp,
               *b = temp;
            for (i = v; *i; i++)
            {
               if (o == b || o - b >= 70)
               {
                  b = o;
                  if (o != temp)
                     o += strlen (strcpy (o, "?=\r\n "));
                  o += strlen (strcpy (o, "=?utf-8?q?"));

               }
               if (*i == ' ')
                  *o++ = '_';
               else if (*i == '_' || *i == '?' || *i == '=' || *i < ' ' || *i > 126)
                  o += sprintf (o, "=%02X", (unsigned char) *i);
               else
                  *o++ = *i;
            }
            strcpy (o, "?=");
            v = temp;
         }
      } else if (!strcasecmp (type, "SURNAME"))
      {
         strncpy (temp, v, sizeof (temp));
         v = skiptitle (temp);
         char *s = strrchr (v, ' ');
         if (s)
            v = s + 1;
         initialise (v);
      } else if (!strcasecmp (type, "FORENAME"))
      {
         strncpy (temp, v, sizeof (temp));
         v = skiptitle (temp);
         char *s = strchr (v, ' ');
         if (s)
            *s = 0;
         initialise (v);
      } else if (!strcasecmp (type, "FORENAMES"))
      {
         strncpy (temp, v, sizeof (temp));
         v = skiptitle (temp);
         char *s = strrchr (v, ' ');
         if (s)
            *s = 0;
         initialise (v);
      } else if (!strcasecmp (type, "TITLE"))
      {
         strncpy (temp, v, sizeof (temp));
         v = temp;
         char *e = skiptitle (v);
         if (v == e)
            v = "";
         else
            *e = 0;
         initialise (v);
      }
   }

   if (v)
   {                            // Match strings
      int a;
      char match = 0;
      for (a = 0; a < x->attrs; a++)
         if (x->attr[a].attribute)
         {
            if (!x->attr[a].value && !strcasecmp (x->attr[a].attribute, "MATCH"))
            {                   // expand for all tag matches
               match = 1;
               continue;
            }
            if (!x->attr[a].value && !strcasecmp (x->attr[a].attribute, "REPLACE"))
            {                   // expanded later
               if (!hasreplace)
                  hasreplace = a + 1;
               match = 2;
               continue;
            }
            if (!match && !strcmp (x->attr[a].attribute, v) &&
                (strcasecmp (x->attr[a].attribute, "HREF")
                 && strcasecmp (x->attr[a].attribute, "TARGET")
                 && strcasecmp (x->attr[a].attribute, "NAME")
                 && strcasecmp (x->attr[a].attribute, "BLANK") && strcasecmp (x->attr[a].attribute, "MISSING")
                 && strcasecmp (x->attr[a].attribute, "value") && strcasecmp (x->attr[a].attribute, "TYPE")
                 && strcasecmp (x->attr[a].attribute, "STYLE") && strcasecmp (x->attr[a].attribute, "CLASS")))
            {                   // legacy
               v = expand (tempval, sizeof (tempval), x->attr[a].value);
               break;
            }
            if (match == 1 && !strcmp (x->attr[a].attribute, v))
            {                   // match, case specific exact match
               v = expand (tempval, sizeof (tempval), x->attr[a].value);
               break;
            }

         }
   }
   // Defaults
   if (!v && missing)
      v = expand (tempval, sizeof (tempval), missing);
   else if (v && !*v && blank)
      v = expand (tempval, sizeof (tempval), blank);

   if (v && *v)
   {                            // output
      char *b = v;
      if (href)
      {
         char tempatt[MAXTEMP];
         char *ta = expand (tempatt, sizeof (tempatt), href);
         if (*ta)
         {
            fprintf (of, "<a");
            xmlwriteattr (of, "href", ta);
            if (class)
               xmlwriteattr (of, "class", expand (tempatt, sizeof (tempatt), class));
            if (style)
               xmlwriteattr (of, "style", expand (tempatt, sizeof (tempatt), style));
            if (target)
               xmlwriteattr (of, "target", expand (tempatt, sizeof (tempatt), target));
            fprintf (of, ">");
         } else
            href = 0;
      } else if (class || style)
      {
         char tempatt[MAXTEMP];
         fprintf (of, "<span");
         if (class)
            xmlwriteattr (of, "class", expand (tempatt, sizeof (tempatt), class));
         if (style)
            xmlwriteattr (of, "style", expand (tempatt, sizeof (tempatt), style));
         fprintf (of, ">");
      }
      if (right && maxsize)
      {
         int n = 0;
         unsigned char *c;
         for (c = (unsigned char *) v; *c; c++)
            if (*c < 0x80 || *c >= 0xC0)
               n++;
         if (n < maxsize)
         {
            n = maxsize - n;
            while (n--)
               fputc (' ', of);
         } else if (n > maxsize)
         {
            fprintf (of, ps ? "..." : "…");
            n -= maxsize;
            for (c = (unsigned char *) v; n && *c; c++)
               if (*c < 0x80 || *c >= 0xC0)
                  n--;
            v = (char *) c;
         }
      }
      int count = 0;
      writeoutput (x, v, v + strlen (v), hasreplace, flags, ps, maxsize, &count);
      if (count < 0)
         fprintf (of, ps ? "..." : "…");

      if (type && !strcasecmp (type, "NTH"))
      {
         int n = atoi (b);
         if (n % 10 == 1 && n % 100 != 11)
            fprintf (of, "st");
         else if (n % 10 == 2 && n % 100 != 12)
            fprintf (of, "nd");
         else if (n % 10 == 3 && n % 100 != 13)
            fprintf (of, "rd");
         else
            fprintf (of, "th");
      }
      if (href)
         fprintf (of, "</a>");
      else if (class || style)
         fprintf (of, "</span>");
   }
   if (file && value)
      free (value);
   return x->next;
}

xmltoken *
dodir (xmltoken * x, process_t * state)
{
   if (!x->end)
   {
      warning (x, "Unclosed %s tag", x->content);
      return x->next;
   }
   xmlattr *all = xmlfindattr (x, "ALL");
   char *path = getatt (x, "PATH");
   if (!path)
      path = ".";
   char temp[MAXTEMP];
   path = expand (temp, sizeof (temp), path);
   struct stat s;
   void found (char *fn, int statres)
   {                            // expects stat done
      setenv ("FILENAME", fn, 1);
      char *leaf = strrchr (fn, '/');
      if (leaf)
         leaf++;
      else
         leaf = fn;
      setenv ("FILELEAF", leaf, 1);

      char *ext = strrchr (fn, '.');
      if (ext)
         setenv ("FILEEXT", ext + 1, 1);

      if (!statres)
      {
         if (S_ISREG (s.st_mode))
            setenv ("FILETYPE", "FILE", 1);
         else if (S_ISDIR (s.st_mode))
            setenv ("FILETYPE", "DIR", 1);
         else if (S_ISCHR (s.st_mode))
            setenv ("FILETYPE", "CHR", 1);
         else if (S_ISBLK (s.st_mode))
            setenv ("FILETYPE", "BLK", 1);
         else if (S_ISFIFO (s.st_mode))
            setenv ("FILETYPE", "FIFO", 1);
         else if (S_ISLNK (s.st_mode))
            setenv ("FILETYPE", "LINK", 1);
         else if (S_ISSOCK (s.st_mode))
            setenv ("FILETYPE", "SOCK", 1);
         else
            setenv ("FILETYPE", "UNKNOWN", 1);
         char temp[100];
         sprintf (temp, "%o", s.st_mode & 0777);
         setenv ("FILEMODE", temp, 1);
         sprintf (temp, "%ld", s.st_size);
         setenv ("FILESIZE", temp, 1);
         strftime (temp, sizeof (temp), "%F %T", localtime (&s.st_mtime));
         setenv ("FILEMTIME", temp, 1);
         strftime (temp, sizeof (temp), "%F %T", localtime (&s.st_ctime));
         setenv ("FILECTIME", temp, 1);
         strftime (temp, sizeof (temp), "%F %T", localtime (&s.st_atime));
         setenv ("FILEATIME", temp, 1);
      }
      processxml (x->next, x->end, state);
      if (statres)
      {
         unsetenv ("FILETYPE");
         unsetenv ("FILEMODE");
         unsetenv ("FILESIZE");
         unsetenv ("FILEMTIME");
         unsetenv ("FILECTIME");
         unsetenv ("FILEATIME");
      }
      unsetenv ("FILENAME");
      unsetenv ("FILELEAF");
      unsetenv ("FILEEXT");
   }
   if (!stat (path, &s) && S_ISDIR (s.st_mode))
   {                            // Dir scan
      struct dirent *e;
      DIR *d = opendir (path);
      if (!d)
      {
         warning (x, "Cannot directory list %s", path);
         return x->next;
      }
      int dirfd = open (path, O_RDONLY);
      if (dirfd < 0)
      {
         warning (x, "Cannot open list %s", path);
         close (dirfd);
         return x->next;
      }
      while ((e = readdir (d)))
         if (all || *e->d_name != '.')
            found (e->d_name, fstatat (dirfd, e->d_name, &s, AT_SYMLINK_NOFOLLOW));
      close (dirfd);
   } else
   {                            // Glob scan
      glob_t pglob = { };
      if (glob (path, GLOB_TILDE_CHECK + (all ? GLOB_PERIOD : 0), NULL, &pglob))
      {
         warning (x, "Cannot match %s", path);
         return x->end->next;
      }
      //  fprintf (stderr, "Glob %ld\n", pglob.gl_pathc);
      int n = 0;
      for (n = 0; n < pglob.gl_pathc; n++)
         found (pglob.gl_pathv[n], lstat (pglob.gl_pathv[n], &s));
      globfree (&pglob);
   }
   return x->end->next;
}

xmltoken *
dofor (xmltoken * x, process_t * state)
{
   if (!x->end)
   {
      warning (x, "Unclosed %s tag", x->content);
      return x->next;
   }
   if (x->attrs < 1)
   {
      warning (x, "%s takes at least one attribute", x->content);
      return x->next;
   }
   char delim = '\t';
   char period = 0;
   int a;
   for (a = 0; a < x->attrs; a++)
   {
      char *n = x->attr[a].attribute;
      char *v = x->attr[a].value;
      if (v)
      {
         char temp[MAXTEMP];
         v = expand (temp, sizeof (temp), v);
         if (period)
         {                      // loop
            char *p = v;
            while (*v && *v != delim)
               v++;
            if (*v)
            {
               *v++ = 0;
               // loop p to v
               if (period == 'U')
               {                // integer up
                  int f = atoi (p),
                     t = atoi (v);
                  while (f <= t)
                  {
                     snprintf (temp, sizeof (temp), "%d", f);
                     setenv (n, temp, 1);
                     processxml (x->next, x->end, state);
                     unsetenv (n);
                     f++;
                  }
               } else if (period == 'D')
               {                // integer down
                  int f = atoi (p),
                     t = atoi (v);
                  while (f >= t)
                  {
                     snprintf (temp, sizeof (temp), "%d", f);
                     setenv (n, temp, 1);
                     processxml (x->next, x->end, state);
                     unsetenv (n);
                     f--;
                  }
               } else
               {                // date
                  void getdate (struct tm *t, char *p)
                  {
                     int H = 0,
                        M = 0,
                        S = 0,
                        d = 0,
                        m = 0,
                        y = 0;
                     sscanf (p, "%d-%d-%d %d:%d:%d", &y, &m, &d, &H, &M, &S);
                     t->tm_year = y - 1900;
                     t->tm_mon = m - 1;
                     t->tm_mday = d;
                     t->tm_hour = H;
                     t->tm_min = M;
                     t->tm_sec = S;
                     t->tm_isdst = -1;
                  }
                  struct tm f = { };
                  struct tm t = { };
                  getdate (&f, p);
                  getdate (&t, v);
                  while (mktime (&f) <= mktime (&t))
                  {
                     snprintf (temp, sizeof (temp), "%04d-%02d-%02d %02d:%02d:%02d", f.tm_year + 1900, f.tm_mon + 1, f.tm_mday,
                               f.tm_hour, f.tm_min, f.tm_sec);
                     setenv (n, temp, 1);
                     processxml (x->next, x->end, state);
                     unsetenv (n);
                     switch (period)
                     {
                     case 'S':
                        f.tm_sec++;
                        break;
                     case 'M':
                        f.tm_min++;
                        break;
                     case 'H':
                        f.tm_hour++;
                        break;
                     case 'd':
                        f.tm_mday++;
                        break;
                     case 'w':
                        f.tm_mday += 7;
                        break;
                     case 'm':
                        f.tm_mon++;
                        break;
                     case 'y':
                        f.tm_year++;
                        break;
                     }
                     f.tm_isdst = -1;
                     t.tm_isdst = -1;
                  }
               }
               v[-1] = delim;
            }
            period = 0;
         } else
            while (*v)          // simple iteration
            {
               char *p = v;
               while (*v && *v != delim)
                  v++;
               char c = *v;
               if (c)
                  *v = 0;
               setenv (n, p, 1);
               if (c)
                  *v++ = c;
               processxml (x->next, x->end, state);
               unsetenv (n);
            }
      } else
      {
         if (!strcasecmp (n, "SPACE"))
            delim = ' ';
         else if (!strcasecmp (n, "LF"))
            delim = '\n';
         else if (!strcasecmp (n, "CR"))
            delim = '\r';
         else if (!strcasecmp (n, "COMMA"))
            delim = ',';
         else if (!strcasecmp (n, "HASH"))
            delim = '#';
         else if (!strcasecmp (n, "SECOND"))
            period = 'S';
         else if (!strcasecmp (n, "MINUTE"))
            period = 'M';
         else if (!strcasecmp (n, "HOUR"))
            period = 'H';
         else if (!strcasecmp (n, "DAY"))
            period = 'd';
         else if (!strcasecmp (n, "WEEK"))
            period = 'w';
         else if (!strcasecmp (n, "MONTH"))
            period = 'm';
         else if (!strcasecmp (n, "YEAR"))
            period = 'y';
         else if (!strcasecmp (n, "UP"))
            period = 'U';
         else if (!strcasecmp (n, "DOWN"))
            period = 'D';
      }
   }

   return x->end->next;
}

xmltoken *
doif (xmltoken * x, process_t * state)
{                               // do if function
   int a = 0;
   char neg = 0;
   char istrue = 1;
   static char lastif = 0;
   if (!x->end)
   {
      warning (x, "Unclosed %s tag", x->content);
      return x->next;
   }
   char *debuginfo = NULL;
   int debuglen = 0,
      debugptr = 0;
   void adddebug (char *f, ...)
   {
      if (!comment && !debug)
         return;
      if (debugptr + 100 >= debuglen)
         debuginfo = realloc (debuginfo, debuglen = debugptr + 100);
      va_list ap;
      va_start (ap, f);
      int l = vsnprintf (debuginfo + debugptr, debuglen - debugptr, f, ap);
      va_end (ap);
      if (l >= debuglen - debugptr)
      {
         if (debugptr + l + 100 < debuglen)
            debuginfo = realloc (debuginfo, debuglen = debugptr + l + 100);
         va_start (ap, f);
         l = vsnprintf (debuginfo + debugptr, debuglen - debugptr, f, ap);
         va_end (ap);
      }
      if (l >= 0)
         debugptr += l;
   }
   while (a < x->attrs)
   {
      char temp[MAXTEMP];
      char *n = x->attr[a].attribute;
      char *v = x->attr[a].value;
      adddebug (" %s", n);
      if (n)
      {
         char *r = expand (temp, sizeof (temp), n);
         if (strcmp (r, n))
            adddebug ("[%s]", r);
         n = r;
      }
      if (!n)
      {
         a++;
         continue;
      }
      if (!v && !strcasecmp (n, "NOT"))
      {
         if (neg)
            warning (x, "NOT NOT in %s", x->content);
         neg = (!neg);
      } else if (!v && !strcasecmp (n, "AND"))
      {
         if (neg)
            warning (x, "NOT AND in %s", x->content);
      } else if (!v && !strcasecmp (n, "OR"))
      {
         if (neg)
            warning (x, "NOT OR in %s", x->content);
         istrue = 1;
         break;                 // done
      } else if (!v && !strcasecmp (n, "ELSE"))
      {
         istrue = ((!lastif) ? !neg : neg);
      } else if (v && !strcasecmp (n, "EXISTS"))
      {                         // file exists
         char *t = expand (temp, sizeof (temp), v);
         adddebug ("[%s]", t);
         istrue = (access (t, R_OK) ? neg : !neg);
      } else if (v)
      {                         // NAME=X
         char temp[MAXTEMP];
         char *z = getvar (n, 0);
         char *e = v;
         char *t;
         if (!z)
         {
            adddebug ("[null]=%s", e);
            istrue = neg;
         } else
         {
            adddebug ("[%s]='%s'", z, v);
            if (*e == '#')
               e++;             // numeric prefix
            if (strchr ("+-=&*", *e))
               e++;
            t = expand (temp, sizeof (temp), e);
            if (strcmp (t, e))
               adddebug ("[%s]", t ? : "null");
            if (*v == '+')      // string >
               istrue = ((strcmp (z, t) >= 0) ? !neg : neg);
            else if (*v == '-') // string <
               istrue = ((strcmp (z, t) <= 0) ? !neg : neg);
            else if (*v == '#' && v[1] == '+')  // numeric >
               istrue = (stringdecimal_cmp (z, t) >= 0 ? !neg : neg);
            else if (*v == '#' && v[1] == '-')  // numeric -
               istrue = (stringdecimal_cmp (z, t) <= 0 ? !neg : neg);
            else if (*v == '#' && v[1] == '=')  // numeric =
               istrue = (!stringdecimal_cmp (z, t) ? !neg : neg);
            else if (*v == '&' || (*v == '#' && v[1] == '&'))   // numeric &
               istrue = ((atoll (z) & atoll (t)) ? !neg : neg);
            else if (*v == '*' && v[1]) // string substring
               istrue = ((strstr (z, t) || (*z && strstr (t, z))) ? !neg : neg);
            else if (*v == '=') // string compare
               istrue = (!strcmp (z, t) ? !neg : neg);
            else if (!strcmp (t, "0"))
            {                   // special case zero compare, true if target has 0 in it and only 0, -, ., :, or space or is empty string
               if (!*z)
                  istrue = !neg;
               else
               {
                  char *q;
                  for (q = z; *q && *q != '0'; q++);
                  if (!*q)
                     istrue = neg;      // no zero in string
                  else
                  {
                     for (q = z; *q == '0' || *q == '-' || *q == '.' || *q == ':' || *q == ' '; q++);
                     istrue = (*q ? neg : !neg);
                  }
               }
            } else              // simple text compare
               istrue = ((!strcmp (z, t)) ? !neg : neg);
         }
         neg = 0;
      } else
      {                         // NAME (exists)
         char *z = getvar (n, 0);
         adddebug ("[%s]", z ? : "null");
         istrue = (z ? !neg : neg);
         neg = 0;
      }
      if (istrue)
         a++;                   // next test
      else
      {                         // skip to next OR
         while (a < x->attrs && (x->attr[a].value || !x->attr[a].attribute || strcasecmp (x->attr[a].attribute, "OR")))
            a++;
         if (a < x->attrs)
         {
            a++;
            istrue = 1;
         }
      }
   }
   {
      static int iflevel = 0;
      if (istrue)
      {
         if (debug > 1)
            info (x, "%s%d: begin%s", x->content, iflevel, debuginfo ? : "");
         iflevel++;
         processxml (x->next, x->end, state);
         iflevel--;
         if (debug > 1)
            info (x, "%s%d: end", x->content, iflevel);
         if (!strcasecmp (x->content, "WHILE"))
         {
            if (debuginfo)
               free (debuginfo);
            return x;           // repeat
         }
      } else
      {
         if (debug > 1)
            info (x, "%s%d: skip %s", x->content, iflevel, debuginfo ? : "");
      }
   }
   x = x->end;
   lastif = istrue;
   if (debuginfo)
      free (debuginfo);
   return x->next;
}

xmltoken *
dolater (xmltoken * x, process_t * state)
{                               // do later function
   xmltoken *e = x->end;
   x = x->next;
   if (!e)
      warning (x, "Unclosed LATER tag");
   else
      while (x && x != e)
      {
         xmlwrite (of, x, (void *) 0);  // write as is - no expansion
         x = x->next;
      }
   return x->next;
}

xmltoken *
doset (xmltoken * x, process_t * state)
{                               // do set function
   int a;
   for (a = 0; a < x->attrs; a++)
      if (x->attr[a].attribute)
      {
         char tempa[MAXTEMP];
         char *va = expand (tempa, sizeof (tempa), x->attr[a].attribute);
         if (x->attr[a].value)
         {
            char temp[MAXTEMP];
            char *v = expand (temp, sizeof (temp), x->attr[a].value);
            if (!v)
               warnx ("Failed to expand: %s", x->attr[a].value);
            else
            {
               setenv (va, v, 1);
               if (comment || debug)
                  info (x, "%s=%s", va, v);
            }
         } else
         {
            unsetenv (va);
            if (comment || debug)
               info (x, "unset %s", va);
         }
      }
   return x->next;
}

xmltoken *
doeval (xmltoken * x, process_t * state)
{                               // do eval function
   int a;
   char format = '*';
   char places = 0;
   char round = 0;
   char *def = NULL;
   for (a = 0; a < x->attrs; a++)
      if (x->attr[a].attribute)
      {
         if (!strcasecmp (x->attr[a].attribute, "!"))
         {
            def = x->attr[a].value;
         } else if (!strcasecmp (x->attr[a].attribute, "#"))
         {
            char *p = x->attr[a].value;
            if (p && isalpha (*p))
               round = toupper (*p++);
            if (p && *p)
               places = atoi (p);
            format = '=';
         } else if (!strcasecmp (x->attr[a].attribute, "/"))
         {
            char *p = x->attr[a].value;
            if (p && isalpha (*p))
               round = toupper (*p++);
            if (p && *p)
               places = atoi (x->attr[a].value);
            format = '+';
         } else if (!strcasecmp (x->attr[a].attribute, "."))
         {
            char *p = x->attr[a].value;
            if (p)
               format = *p;
         } else if (x->attr[a].value)
         {
            char tempa[MAXTEMP];
            char *va = expand (tempa, sizeof (tempa), x->attr[a].attribute);
            char temp[MAXTEMP];
            char *v = expandz (temp, sizeof (temp), x->attr[a].value);
            if (!va)
               warnx ("Failed to expand: %s", x->attr[a].attribute);
            else if (!v)
               warnx ("Failed to expand: %s", x->attr[a].value);
            else
            {
             char *e = stringdecimal_eval (v, format: format, places: places, round:round);
               if (!debug && !comment && (!e || *e == '!') && !def)
                  fprintf (stderr, "%s:%d Eval %s = %s = %s = %s format=%c round=%c places=%d\n", x->filename, x->line, va,
                           x->attr[a].value, v, e, format ? : '?', round ? : '?', places);
               if (!e || *e == '!')
               {
                  if (e)
                     free (e);
                  e = NULL;
                  if (def)
                     e = strdup (def);
#ifdef	BODGEEVAL
                  else
                     e = strdup (v);    // Bodge to use unchanged value
#endif
               }
               if (e)
               {
                  if (comment || debug)
                     info (x, "Eval %s = %s = %s = %s format=%c round=%c places=%d\n", va, x->attr[a].value, v, e, format ? : '?',
                           round ? : '?', places);
                  setenv (va, e, 1);
                  free (e);
               } else
               {
                  if (comment || debug)
                     info (x, "Unset %s", va);
                  unsetenv (va);
               }
            }
         } else
         {
            unsetenv (x->attr[a].attribute);
            if (comment || debug)
               info (x, "unset %s", x->attr[a].attribute);
         }
      }
   return x->next;
}

xmltoken *
dosql (xmltoken * x, process_t * state)
{                               // do sql function
   size_t outsize = 0;
   char *outdata = NULL;
   FILE *out = of;
   if (!x->end)
   {
      warning (x, "Unclosed SQL tag");
      return x->next;
   }
   if (level + 1 == MAXLEVEL)
   {
      warning (x, "SQL nested too deep");
      return x->next;
   }
   if (!sqlconnected)
   {
      sql_real_connect (&sql, sqlhost, sqluser, sqlpass, sqldatabase, sqlport, 0, 0, 1, sqlconf);
      sqlconnected = 1;
   }
   if (sqlconnected)
   {
      {                         // construct query
         char *litquery = getatt (x, "QUERY");
         xmlattr *key = xmlfindattr (x, "KEY");
         char *select = getatt (x, "SELECT");
         char *table = getatt (x, "TABLE");
         char *where = getatt (x, "WHERE");
         char *order = getatt (x, "ORDER");
         char *having = getatt (x, "HAVING");
         char *group = getatt (x, "GROUP");
         char *limit = getatt (x, "LIMIT");
         xmlattr *csv = xmlfindattr (x, "CSV");
         xmlattr *csvhead = xmlfindattr (x, "CSVHEAD");
         xmlattr *xml = xmlfindattr (x, "XML");
         xmlattr *json = xmlfindattr (x, "JSON");
         xmlattr *jsarray = xmlfindattr (x, "JSARRAY");
         xmlattr *jsarrayhead = xmlfindattr (x, "JSARRAYHEAD");
         xmlattr *tablehead = xmlfindattr (x, "TABLEHEAD");
         xmlattr *tablerow = xmlfindattr (x, "TABLEROW");
         if ((json && json->value) || (jsarray && jsarray->value) || (tablerow && tablerow->value))
            out = open_memstream (&outdata, &outsize);
         char temp[MAXTEMP];
         char *v;
         char *query = NULL;
         xmlattr *desc = xmlfindattr (x, "DESC");
         xmlattr *asc = xmlfindattr (x, "ASC");
         xmlattr *distinct = xmlfindattr (x, "DISTINCT");
         if (!table)
            table = getatt (x, "FROM");
         if (!group)
            group = getatt (x, "GROUPBY");
         if (!order)
            order = getatt (x, "ORDERBY");
         if (litquery)
         {                      // literal query
            if (key)
               warning (x, "QUERY and KEY in SQL");
            if (select)
               warning (x, "QUERY and SELECT in SQL");
            if (table)
               warning (x, "QUERY and TABLE in SQL");
            if (where)
               warning (x, "QUERY and WHERE in SQL");
            if (order)
               warning (x, "QUERY and ORDER in SQL");
            if (having)
               warning (x, "QUERY and HAVING in SQL");
            if (group)
               warning (x, "QUERY and GROUP in SQL");
            if (limit)
               warning (x, "QUERY and LIMIT in SQL");
            if (desc)
               warning (x, "QUERY and DESC in SQL");
            if (asc)
               warning (x, "QUERY and ASC in SQL");
            v = expand (temp, sizeof (temp), litquery);
            if (!v)
               warnx ("Failed to expand: %s", litquery);
            query = strdup (v);
         } else
         {                      // construct query
            if (key && key->value && where && *where)
            {
               warning (x, "KEY and WHERE in SQL");
               return x->end->next;
            };
            if (desc && (!order || !*order))
            {
               warning (x, "DESC with no ORDER in SQL");
               return x->end->next;
            };
            char *exp (char *p)
            {
               if (!p)
                  return NULL;
               p = expand (temp, sizeof (temp), p);
               if (!*p)
                  return NULL;
               return p;
            }
#define ex(n) (n=exp(n))
            // Construct then expand
            size_t l;
            FILE *o = open_memstream (&query, &l);
            fprintf (o, "SELECT ");
            if (distinct)
               fprintf (o, "DISTINCT ");
            fprintf (o, "%s", ex (select) ? : "*");
            if (ex (table))
               fprintf (o, " FROM %s", table);
            if (ex (where))
               fprintf (o, " WHERE %s", where);
            else if (key && key->value)
            {
               char *v = strrchr (key->value, '.');
               v = getvar (v ? : key->value, 0);
               fprintf (o, " WHERE %s='%s'", key->value, v ? : "");
            }
            if (ex (group))
               fprintf (o, " GROUP BY %s", group);
            if (ex (having))
               fprintf (o, " HAVING %s", having);
            if (ex (order))
            {
               fprintf (o, " ORDER BY %s", order);
               if (desc)
                  fprintf (o, " DESC");
               if (asc)
                  fprintf (o, " ASC");
            }
            if (ex (limit))
               fprintf (o, " LIMIT %s", limit);
            fclose (o);
         }
#undef qadd
         {                      // Sanity check
            char q = 0;
            char *p;
            for (p = query; *p; p++)
            {
               if (*p == '\\' && p[1])
               {
                  if (!p[1])
                     errx (1, "%s:%d Trailing \\ in SQL query: %s", x->filename, x->line, query);
                  p++;
                  continue;
               }
               if (q && *p == q)
                  q = 0;
               else if (!q && (*p == '`' || *p == '\'' || *p == '"'))
                  q = *p;
               if (!q)
               {
                  if ((*p == '-' && p[1] == '-' && (!p[2] || isspace (p[2]))) || *p == '#' || (*p == '/' && p[1] == '*'))
                     errx (1, "%s:%d Comment in SQL query: %s", x->filename, x->line, query);
                  if (*p == ';')
                  {
                     if (!p[1])
                     {
                        *p = 0;
                        warnx ("%s:%d Trailing ; on sql query, ignored: %s", x->filename, x->line, query);
                     } else
                        errx (1, "%s:%d Multiple query attempt in SQL query: %s", x->filename, x->line, query);
                  }
               }
            }
            if (q)
               errx (1, "%s:%d Unclosed (%c) in query: %s", x->filename, x->line, q, query);
         }
         if (sql_query (&sql, query))
         {
            warning (x, "SQL%d: %s\nError:%s", level, query, (char *) sql_error (&sql));
            free (query);
            return x->end->next;
         } else
            info (x, "SQL%d: %s", level, query);
         //res[level] = sql_use_result (&sql);
         res[level] = sql_store_result (&sql);
         free (query);
         if (res[level])
         {                      // query done, result
            fields[level] = sql_num_fields (res[level]);
            field[level] = sql_fetch_field (res[level]);
            void xmlout (char *c)
            {
               if (!c)
                  return;
               while (*c)
               {
                  if (*c == '\n' || (*c == '\r' && v[1] != '\n'))
                     fprintf (out, "<br>");
                  else if (*c == '\f')
                     fprintf (out, "<br><hr>");
                  //else if (*c == (char) 160) fprintf (out, "&nbsp;");
                  else if (*c == '\'')
                     fprintf (out, "&#39;");    //apos does not work in IE Except xml
                  else if (*c == '&')
                     fprintf (out, "&amp;");
                  else if (*c == '<')
                     fprintf (out, "&lt;");
                  else if (*c == '>')
                     fprintf (out, "&gt;");
                  else if (*c != '\r')
                     fputc (*c, out);
                  c++;
               }
            }
            void csvout (const char *p, char q)
            {                   // JSON string
               while (*p)
               {
                  if (*p >= ' ')
                  {
                     if ((q && *p == q) || *p == '\\')
                        fputc ('\\', out);
                     fputc (*p, out);
                  }
                  p++;
               }
            }
            void jsonout (const char *p)
            {                   // JSON string
               if (!p)
               {
                  fprintf (out, "null");
                  return;
               }
               fputc ('"', out);
               while (*p)
               {
                  unsigned char c = *p;
                  if (c == '\n')
                     fprintf (out, "\\n");
                  else if (c == '\r')
                     fprintf (out, "\\r");
                  else if (c == '\t')
                     fprintf (out, "\\t");
                  else if (c == '\b')
                     fprintf (out, "\\b");
                  else if (c == '\\' || c == '"' || c == '/')   // Note escaping / is optional but done to avoid </script> issues
                     fprintf (out, "\\%c", (char) c);
                  else if (c < ' ')
                     fprintf (out, "\\u%04X", c);
                  else
                     fputc (c, out);
                  p++;
               }
               fputc ('"', out);
            }
            if (tablehead)
            {
               fprintf (out, "<tr class='sqlhead'>");
               for (int f = 0; f < fields[level]; f++)
               {
                  fprintf (out, "<th>");
                  xmlout (field[level][f].name);
                  fprintf (out, "</th>");
               }
               fprintf (out, "</tr>\n");
            }
            if (csvhead)
            {
               for (int f = 0; f < fields[level]; f++)
               {
                  if (f)
                     fputc (',', out);
                  fputc ('"', out);
                  csvout (field[level][f].name, '"');
                  fputc ('"', out);
               }
               fputc ('\n', out);
            }
            if (x->type & XML_END)
            {                   // command has results, and we have no way to format it - special cases for direct formatted output
               if (csv)
               {                // Direct CSV output
                  while ((row[level] = sql_fetch_row (res[level])))
                  {
                     for (int f = 0; f < fields[level]; f++)
                     {
                        char *c = row[level][f];
                        char q = 0;
                        if (!(field[level][f].flags & NUM_FLAG))
                           q = '"';
                        if (f)
                           fputc (',', out);
                        if (c)
                        {
                           if (q)
                              fputc (q, out);
                           csvout (row[level][f], q);
                           if (q)
                              fputc (q, out);
                        }
                     }
                     fputc ('\n', out);
                  }
               } else if (xml)
               {                // Direct XML output
                  while ((row[level] = sql_fetch_row (res[level])))
                  {
                     fprintf (out, "<%s>", xml->value ? : "Row");
                     for (int f = 0; f < fields[level]; f++)
                     {
                        char *c = row[level][f];
                        if (c)
                        {
                           if (xml->value)
                              fprintf (out, "<%s>", field[level][f].name);
                           else
                           {
                              int t = field[level][f].type == FIELD_TYPE_DATE;
                              char *style = XMLATTREMOVE;
                              char *type = "String";
                              if (t == FIELD_TYPE_DATE || t == FIELD_TYPE_DATETIME || t == FIELD_TYPE_TIMESTAMP)
                                 style = type = "DateTime";
                              if (IS_NUM (field[level][f].type))
                                 type = "Number";
                              if (t == FIELD_TYPE_DECIMAL && field[level][f].decimals == 2)
                                 style = "Money";
                              xmlwrite (out, 0, "Cell", "ss:StyleID", style, 0);
                              xmlwrite (out, 0, "Data", "ss:Type", type, "FieldName", field[level][f].name, 0);
                           }
                           xmlout (c);
                           if (xml->value)
                              fprintf (out, "</%s>", field[level][f].name);
                           else
                              fprintf (out, "</Data></Cell>");
                        } else
                        {
                           if (xml->value)
                              fprintf (out, "<%s/>", field[level][f].name);
                           else
                              fprintf (out, "<Cell/>");
                        }
                     }
                     fprintf (out, "</%s>\n", xml->value ? : "Row");
                  }
               } else if (json || jsarray || jsarrayhead)
               {
                  int found = 0;
                  fprintf (out, "[");   // top level array
                  if (jsarrayhead)
                  {             // heading row
                     found++;
                     fprintf (out, "[");
                     for (int f = 0; f < fields[level]; f++)
                     {
                        if (f)
                           fprintf (out, ",");
                        jsonout (field[level][f].name);
                     }
                     fprintf (out, "]");
                  }
                  while ((row[level] = sql_fetch_row (res[level])))
                  {
                     if (found++)
                        fprintf (out, ",");
                     if (json)
                     {          // object
                        int found = 0;
                        fprintf (out, "{");
                        for (int f = 0; f < fields[level]; f++)
                        {
                           char *c = row[level][f];
                           if (!c)
                              continue;
                           if (found++)
                              fprintf (out, ",");
                           jsonout (field[level][f].name);
                           fprintf (out, ":");
                           if (c && IS_NUM (field[level][f].type))
                              fprintf (out, "%s", c);
                           else
                              jsonout (c);
                        }
                        fprintf (out, "}");
                     } else
                     {          // array
                        fprintf (out, "[");
                        for (int f = 0; f < fields[level]; f++)
                        {
                           if (f)
                              fprintf (out, ",");
                           char *c = row[level][f];
                           if (c && IS_NUM (field[level][f].type))
                              fprintf (out, "%s", c);
                           else
                              jsonout (c);
                        }
                        fprintf (out, "]");
                     }
                  }
                  fprintf (out, "]");
               } else if (tablerow)
               {                // Simple table rows
                  while ((row[level] = sql_fetch_row (res[level])))
                  {
                     fprintf (out, "<tr class='sqlresult'>");
                     for (int f = 0; f < fields[level]; f++)
                     {
                        fprintf (out, "<td");
                        if (field[level][f].type == FIELD_TYPE_DATE)
                           fprintf (out, " class='sqldate'");
                        else if (field[level][f].type == FIELD_TYPE_YEAR)
                           fprintf (out, " class='sqlyear'");
                        else if (field[level][f].type == FIELD_TYPE_TIME)
                           fprintf (out, " class='sqltime'");
                        else if (field[level][f].type == FIELD_TYPE_DATETIME)
                           fprintf (out, " class='sqldatetime'");
                        else if (field[level][f].type == FIELD_TYPE_TIMESTAMP)
                           fprintf (out, " class='sqltimestamp'");
                        else if (field[level][f].type == MYSQL_TYPE_VAR_STRING || field[level][f].type == FIELD_TYPE_STRING)
                           fprintf (out, " class='sqlstring'");
                        else if (field[level][f].type == FIELD_TYPE_SET)
                           fprintf (out, " class='sqlset'");
                        else if (field[level][f].type == FIELD_TYPE_ENUM)
                           fprintf (out, " class='sqlenum'");
                        else if (field[level][f].flags & NUM_FLAG)
                           fprintf (out, " class='sqlnum'");
                        fprintf (out, ">");
                        xmlout (row[level][f]);
                        fprintf (out, "</td>");
                     }
                     fprintf (out, "</tr>\n");
                  }
               } else
               {
                  sql_free_result (res[level]);
                  warning (x, "SQL result with no formatting");
                  return x->end->next;
               }
            } else
            {                   // command has results, and we have content to output using that
               if (csv)
               {
                  sql_free_result (res[level]);
                  warning (x, "SQL CSV - use self closing SQL tag");
                  return x->end->next;
               }
               if (xml)
               {
                  sql_free_result (res[level]);
                  warning (x, "SQL XML - use self closing SQL tag");
                  return x->end->next;
               }
               if (json || jsarray)
               {
                  sql_free_result (res[level]);
                  warning (x, "SQL JSON - use self closing SQL tag");
                  return x->end->next;
               }
               if (tablerow)
               {
                  sql_free_result (res[level]);
                  warning (x, "SQL TABLE - use self closing SQL tag");
                  return x->end->next;
               }
               row[level] = sql_fetch_row (res[level]);
               if (row[level])
               {
                  sqlactive[level] = 1;
                  do
                  {
                     level++;
                     processxml (x->next, x->end, state);
                     level--;
                     row[level] = sql_fetch_row (res[level]);
                  }
                  while (row[level]);
                  info (x, "SQL%d: done", level);
               } else
               {                // no rows
                  if (key && table)
                  {             // template
                     sql_free_result (res[level]);
                     res[level] = sql_list_fields (&sql, table, 0);
                     if (!res[level])
                     {
                        sql_free_result (res[level]);
                        warning (x, "SQL query with no content - use self closing SQL tag");
                        return x->end->next;
                     }
                     field[level] = sql_fetch_field (res[level]);
                     fields[level] = sql_num_fields (res[level]);
                     sqlactive[level] = 2;
                     level++;
                     processxml (x->next, x->end, state);
                     level--;
                     info (x, "SQL%d: template", level);
                  } else
                     info (x, "No rows");
               }
               sqlactive[level] = 0;
            }
            sql_free_result (res[level]);
         } else
         {                      // command done, no result
            if (!(x->type & XML_END))
               warning (x, "SQL command with content produced no results - use self closing SQL tag");
            return x->end->next;
         }
         if ((json && json->value) || (jsarray && jsarray->value) || (tablerow && tablerow->value))
         {
            fputc (0, out);
            fclose (out);
            char *tag = NULL;
            if (json && json->value)
               tag = expand (temp, sizeof (temp), json->value);
            else if (jsarray && jsarray->value)
               tag = expand (temp, sizeof (temp), jsarray->value);
            else if (tablerow && tablerow->value)
               tag = expand (temp, sizeof (temp), tablerow->value);
            if (tag && *tag)
               setenv (tag, outdata, 1);
            free (outdata);
         }
      }
   }
   return x->end->next;
}

xmltoken *
doinput (xmltoken * x, process_t * state)
{                               // do input function
   char *name = getatt (x, "name");
   char *value = getatt (x, "value");
   char *set = getatt (x, "set");
   char *checkval = value;
   char *type = getatt (x, "type");
   char *size = getatt (x, "size");
   char *style = getatt (x, "style");
   char *class = getatt (x, "class");
   char *maxlength = getatt (x, "maxlength");
   char *checked = getatt (x, "checked");
   xmlattr *trim = xmlfindattr (x, "TRIM");
   char *v = 0;
   char tsize[50];
   char tmaxlength[30];
   char tempvar[100];
   int len = 0;
   if (value)
   {                            // Allow $ in explicit value
      char temp[1000];
      value = strdupa (expand (temp, sizeof (temp), value));
   }
   if (style)
   {                            // Allow $ in explicit style
      char temp[1000];
      style = strdupa (expand (temp, sizeof (temp), style));
   }
   if (!type)
      type = XMLATTREMOVE;
   else
   {
      char temp[100];
      type = strdupa (expand (temp, sizeof (temp), type));
   }
   if (!checked)
      checked = XMLATTREMOVE;
   if (!name)
   {
      if (noform)
      {
         if (value)
         {
            if (class)
               xmlwrite (of, 0, "span", "class", class, (char *) 0);
            fprintf (of, "%s", value);
            if (class)
               fprintf (of, "</span>");
         }

      } else
         tagwrite (of, x, (void *) 0);  // no change
      return x->next;
   }
   if (type && !strcasecmp (type, "submit"))
   {
      if (!noform)
         tagwrite (of, x, (void *) 0);
      return x->next;
   }
   if (!strncasecmp (name, "FILE:", 5))
      name += 5;
   if (set)
      len = strlen (v = expand (tempvar, sizeof (tempvar), set));
   else
      v = getvar (expand (tempvar, sizeof (tempvar), name), &len);
   if (len)
   {
      if (!size && maxinputsize)
      {
#if 0
         if (maxinputsize > 0 && len > maxinputsize)
            sprintf ((size = tsize), "%d", maxinputsize);
         else
            sprintf ((size = tsize), "%d", len + 1);
#else
         if (!style)
         {
            if (maxinputsize > 0 && len > maxinputsize)
               sprintf ((style = tsize), "width:%dex;", maxinputsize);
            else
               sprintf ((style = tsize), "width:%dex;", len + 1);
         }
#endif
      }
      if (!maxlength)
         sprintf ((maxlength = tmaxlength), "%d", len);
   }
#if 0
   if (!size)
      size = XMLATTREMOVE;
#else
   if (!style)
      style = XMLATTREMOVE;
#endif
   if (!maxlength)
      maxlength = XMLATTREMOVE;
   if (v)
   {
      if (type && !strcasecmp (type, "radio"))
      {
         char temp[MAXTEMP];
         checked = size = maxlength = XMLATTREMOVE;
         if (value && !strcmp (v, expand (temp, sizeof (temp), value)))
            checked = "checked";
      } else if (type && !strcasecmp (type, "checkbox"))
      {
         char temp[MAXTEMP];
         checkval = value;
         if (!checkval)
            checkval = "on";
         checked = size = maxlength = XMLATTREMOVE;
         if (*v && matchvalue (v, expand (temp, sizeof (temp), checkval)))
            checked = "checked";
      } else
         checkval = value = v;
   } else if (checked)
   {
      char temp[1000];
      if (!expand (temp, sizeof (temp), checked))
         checked = XMLATTREMOVE;
   }
   if (trim && value)
   {
      value = strdupa (value);
      char *d = strrchr (value, '.');
      if (d)
      {
         char *e = value + strlen (value);
         while (e > d && e[-1] == '0')
            e--;
         if (e == d + 1)
            e--;
         *e = 0;
      }
   }
   if (!value)
      value = XMLATTREMOVE;
   if (type && !strcasecmp (type, "HIDDEN"))
   {
      size = maxlength = XMLATTREMOVE;
      if (showhidden)
         type = XMLATTREMOVE;
   }
   if (noform)
   {
      if (type && (!strcasecmp (type, "checkbox") || !strcasecmp (type, "radio")) && checked == XMLATTREMOVE)
         checkval = 0;
      if (checkval)
      {
         char *class = getatt (x, "class");
         if (class)
            xmlwrite (of, 0, "span", "class", class, (char *) 0);
         fprintf (of, "%s", checkval);
         if (class)
            fprintf (of, "</span>");
      }
   } else if (type && !strcasecmp (type, "datetime-local") && strlen (value) == 19 && value[10] == ' ')
   {
      value = strdupa (value);
      value[10] = 'T';
      tagwrite (of, x, "value", value, "type", type, (void *) 0);
   } else if (type && (!strcasecmp (type, "checkbox") || !strcasecmp (type, "radio")))
      tagwrite (of, x, "value", value, "checked", checked, "type", type, "trim", XMLATTREMOVE, (void *) 0);
   else
      tagwrite (of, x, "style", style, "maxlength", maxlength, "value", value, "checked", checked, "type", type, "trim",
                XMLATTREMOVE, (void *) 0);
   return x->next;
}

xmltoken *
doform (xmltoken * x, process_t * state)
{                               // do form function (just removes form tags)
   if (!noform)
   {
      tagwrite (of, x, (void *) 0);
      return x->next;
   }
   if (!x->end)
   {
      warning (x, "Unclosed FORM tag");
      return x->next;
   }
   char *class = getatt (x, "class");
   if (class)
      xmlwrite (of, 0, "div", "class", class, (char *) 0);
   processxml (x->next, x->end, state);
   if (class)
      fprintf (of, "</div>");
   return x->end->next;
}

xmltoken *
doscript (xmltoken * x, process_t * state)
{
   char *object = getatt (x, "object");
   tagwrite (of, x, "var", XMLATTREMOVE, "object", XMLATTREMOVE, (void *) 0);
   int a;
   if (object)
      fprintf (of, "var %s={", object);
   int count = 0;
   for (a = 0; a < x->attrs; a++)
      if (x->attr[a].attribute && x->attr[a].value && !strcasecmp (x->attr[a].attribute, "VAR"))
      {
         char *n = x->attr[a].value;
         if (!strcmp (n, "*"))
         {                      // all in current sql
            int l = level;
            if (!l)
            {
               fprintf (of, "// no sql open\n");
               continue;
            }
            l--;
            if (sqlactive[l])
            {
               int max = fields[l],
                  f;
               for (f = 0; f < max; f++)
                  if (row[l][f])
                  {
                     if (count++ && object)
                        fprintf (of, ",");
                     if (object)
                        fprintf (of, "%s:'", field[l][f].name);
                     else
                        fprintf (of, "var %s='", field[l][f].name);
                     char *p = row[l][f];
                     while (*p)
                     {
                        if (*p == '\n')
                           fprintf (of, "\\n");
                        else if (*p == '\\' || *p == '\'')
                           fprintf (of, "\\%c", *p);
                        else
                           fputc (*p, of);
                        p++;
                     }
                     fprintf (of, "'");
                     if (!object)
                        fprintf (of, ";");
                  } else if (!object)
                  {
                     if (count++ && object)
                        fprintf (of, ",");
                     fprintf (of, "var %s=undefined\n", field[l][f].name);
                     if (!object)
                        fprintf (of, ";");
                  }
            }
            continue;
         }
         int raw = 0;
         int file = 0;
         while (*n)
         {
            if (*n == '*')
               raw++;
            else if (*n == '@')
               file++;
            else
               break;
            n++;
         }
         char *v = getvar (n, 0);
         if (v || !object)
         {
            if (count++ && object)
               fprintf (of, ",");
            if (object)
               fprintf (of, "%s:", n);
            else
               fprintf (of, "var %s=", n);
            size_t l = 0;
            if (v)
            {
               l = strlen (v);
               if (l && file)
               {
                  if (!strncmp (v, "/etc/", 5))
                  {
                     warnx ("Nice try %s", v);
                     v = NULL;
                  } else
                  {
                     int fd = open (v, O_RDONLY);
                     if (fd < 0)
                     {
                        warn ("Open failed %s", v);
                        v = NULL;
                     } else
                     {
                        struct stat s;
                        if (fstat (fd, &s))
                        {
                           warn ("Stat failed %s", v);
                           v = NULL;
                        } else
                        {
                           l = s.st_size;
                           void *a = mmap (NULL, l, PROT_READ, MAP_SHARED, fd, 0);
                           if (a == MAP_FAILED)
                           {
                              warn ("Map failed %s", v);
                              v = NULL;
                           } else
                              v = a;
                        }
                        close (fd);
                     }
                  }
               }
            }
            if (!v)
            {
               l = strlen (v = "undefined");
               raw = 1;
               file = 0;
            }
            if (raw)
               fprintf (of, "%.*s", (int) l, v);
            else
            {
               fprintf (of, "'");
               char *p = v,
                  *e = v + l;
               while (p < e)
               {
                  if (*p == '\n')
                     fprintf (of, "\\n");
                  else if (*p == '\\' || *p == '\'')
                     fprintf (of, "\\%c", *p);
                  else
                     fputc (*p, of);
                  p++;
               }
               fprintf (of, "'");
            }
            if (!object)
               fprintf (of, ";");
            if (file)
               munmap (v, l);
         }
      }
   if (object)
      fprintf (of, "};\n");
   return x->next;
}

xmltoken *
doimg (xmltoken * x, process_t * state)
{
   char tempatt[MAXTEMP];
   char tempalt[MAXTEMP];
   char *alt = getatt (x, "alt");
   if (!alt)
      alt = getatt (x, "title");
   if (!alt)
      alt = getatt (x, "src");
   if (!alt)
      alt = "";                 // Force an alt tag of some sort
   alt = expand (tempalt, sizeof (tempalt), alt);
   char *file = getatt (x, "base64");
   if (!file)
   {                            // Not interested
      tagwrite (of, x, "base64", XMLATTREMOVE, "alt", alt, (void *) 0);
      return x->next;
   }
   char *ta = expand (tempatt, sizeof (tempatt), file);
   int f = open (ta, O_RDONLY);
   if (f < 0)
   {
      fprintf (stderr, "Unable to open file %s\n", ta);
      tagwrite (of, x, "base64", XMLATTREMOVE, "alt", alt, (void *) 0);
      return x->next;
   }
   const char *type = NULL;
   unsigned char magic[4] = { };
   ssize_t l = read (f, magic, sizeof (magic));
   lseek (f, 0, SEEK_SET);
   if (l == sizeof (magic))
   {                            // work out type - we support only a few
      if (magic[0] == 0x89 && magic[1] == 0x50 && magic[2] == 0x4E && magic[3] == 0x47)
         type = "image/png";
      if (magic[0] == 0x47 && magic[1] == 0x49 && magic[2] == 0x46 && magic[3] == 0x38)
         type = "image/gif";
      if (magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF && magic[3] == 0xDB)
         type = "image/jpeg";
      if (magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF && magic[3] == 0xE0)
         type = "image/jpeg";
      if (magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF && magic[3] == 0xE1)
         type = "image/jpeg";
   }
   if (!type)
   {
      close (f);
      if (l != sizeof (magic))
         fprintf (stderr, "Unknown file type for %s (%d)\n", ta, (int) l);
      else
         fprintf (stderr, "Unknown file type for %s (%02X%02X%02X%02X)\n", ta, magic[0], magic[1], magic[2], magic[3]);
      tagwrite (of, x, "base64", XMLATTREMOVE, "alt", alt, (void *) 0);
      return x->next;
   }
   fprintf (of, "<img");
   int a;
   for (a = 0; a < x->attrs; a++)
      if (strcasecmp (x->attr[a].attribute, "base64") && strcasecmp (x->attr[a].attribute, "src"))
         expandwriteattr (of, x->attr[a].attribute, x->attr[a].value);
   fprintf (of, " src=\"data:%s;base64,", type);
   char buf[1024];
   int v = 0,
      ll = 0,
      b = 0;
   while ((l = read (f, buf, sizeof (buf))) > 0)
   {
      ssize_t p;
      for (p = 0; p < l; p++)
      {
         b += 8;
         v = (v << 8) + buf[p];
         while (b >= 6)
         {
            if( dataurifold && ll == dataurifold ) { fputc('\n',of); ll=0; }
            b -= 6;
            fputc (BASE64[(v >> b) & ((1 << 6) - 1)], of);
            ll++;
         }
      }
   }
   if (b)
   {                            // final bits
      b += 8;
      v <<= 8;
      b -= 6;
      fputc (BASE64[(v >> b) & ((1 << 6) - 1)], of);
      while (b)
      {                         // padding
         while (b >= 6)
         {
            b -= 6;
            fputc ('=', of);
         }
         if (b)
            b += 8;
      }
   }
   fputc ('"', of);
   if ((x->type & XML_END))
      fputc ('/', of);
   fputc ('>', of);
   close (f);
   return x->next;
}

xmltoken *
doselect (xmltoken * x, process_t * state)
{                               // do select function
   char *name = getatt (x, "NAME");
   char *set = getatt (x, "set");
   if (!noform)
      tagwrite (of, x, (void *) 0);
   if (!x->end)
   {
      warning (x, "Unclosed SELECT tag");
      return x->next;
   }
   if (!name || !*name)
   {
      warning (x, "SELECT with no NAME");
      processxml (x->next, x->end, 0);
      return x->end;
   }
   {
      process_t state = {
         0
      };
      char *v = 0;
      {
         char temp[100];
         if (set)
            v = expand (temp, sizeof (temp), set);
         else
            v = getvar (expand (temp, sizeof (temp), name), 0);
      }
      if (v)
         v = strdup (v);
      state.selectvalue = v;
      if (xmlfindattr (x, "MULTIPLE"))
         state.selectmultiple = 1;
      processxml (x->next, x->end, &state);
      if (v)
         free (v);
   }
   if (noform)
      return x->end->next;
   return x->end;
}

xmltoken *
dooption (xmltoken * x, process_t * state)
{
   if (state && state->selectvalue)
   {
      char temp[MAXTEMP];
      char *value = expand (temp, sizeof (temp), getatt (x, "value"));
      char *r = 0,
         t = 0;
      char *selected = XMLATTREMOVE;
      if (!value)
      {
         xmltoken *q = x->next;
         while (q && (q->type & XML_COMMENT))
            q = q->next;
         if (q && q->type == XML_TEXT)
            value = q->content;
         if (value)
         {
            for (r = value; *r >= ' '; r++);
            if (*r)
            {
               t = *r;
               *r = 0;
            }
         }
      }
      if (state->selectmultiple)
      {
         if (matchvalue (state->selectvalue, value))
            selected = "selected";
      } else
      {
         if (value && !strcmp (state->selectvalue, value))
            selected = "selected";
      }
      if (noform)
      {
         state->selectedoption = (selected != XMLATTREMOVE);
      } else
         tagwrite (of, x, "selected", selected, (void *) 0);
      if (r)
         *r = t;
   } else
      tagwrite (of, x, (void *) 0);
   return x->next;
}

xmltoken *
dotextarea (xmltoken * x, process_t * state)
{                               // do textarea function
   char *name = getatt (x, "name");
   char *file = getatt (x, "file");
   char tempvar[100];
   char *v;
   if (!x->end)
   {
      warning (x, "Unclosed TEXTAREA tag");
      if (!noform)
         tagwrite (of, x, "file", XMLATTREMOVE, (void *) 0);
      return x->next;
   }
   if (!name)
   {
      warning (x, "TEXTAREA with no NAME");
      if (!noform)
         tagwrite (of, x, "file", XMLATTREMOVE, (void *) 0);
      return x->next;
   }
   char type = x->type;
   x->type &= ~XML_END;
   if (!noform)
      tagwrite (of, x, "file", XMLATTREMOVE, (void *) 0);
   char *class = getatt (x, "class");
   if (!strncasecmp (name, "FILE:", 5))
      name += 5;
   v = getvar (expand (tempvar, sizeof (tempvar), name), 0);
   if (v)
   {
      if (noform && class)
         xmlwrite (of, 0, "span", "class", class, (char *) 0);
      while (*v)
      {
         if (*v == '<')
            fprintf (of, "&lt;");
         else if (*v == '>')
            fprintf (of, "&gt;");
         else if (*v == '&')
            fprintf (of, "&amp;");
         else
            fputc (*v, of);
         v++;
      }
      if (noform && class)
         fprintf (of, "</span>");
      x = x->end;
      type |= XML_END;
   } else if (file && !safe)
   {
      char temp[MAXTEMP];
      file = expand (temp, sizeof (temp), file);
      FILE *f = fopen (file, "r");
      if (f)
      {
         int c;
         if (noform && class)
            xmlwrite (of, 0, "span", "class", class, (char *) 0);
         while ((c = fgetc (f)) >= 0)
         {
            if (c == '<')
               fprintf (of, "&lt;");
            else if (c == '>')
               fprintf (of, "&gt;");
            else if (c == '&')
               fprintf (of, "&amp;");
            else
               fputc (c, of);
         }
         fclose (f);
         if (noform && class)
            fprintf (of, "</span>");
         x = x->end;
         type |= XML_END;
      }
   }
   if (noform)
      return x->end->next;
   if (!noform && (type & XML_END))
      fprintf (of, "</textarea>");
   return x->next;
}

xmltoken *
doinclude (xmltoken * x, process_t * state, char *value)
{                               // src overrides the src check
   xmlattr *a = xmlfindattr (x, "SRC");
   if (value || (a && a->value))
   {
      char temp[MAXTEMP];
      if (!value)
      {
         value = expand (temp, sizeof (temp), a->value);
         if (strcmp (a->value, value))
            info (x, "Include %s [%s]", a->value, value);
         else
            info (x, "Include %s", a->value);
      }
      xmltoken *i = loadfile (value);
      if (i)
      {                         // included
         xmltoken *l = x->next;
         x->next = i;
         while (i && i->next)
            i = (i->end && i->end != i ? i->end : i->next);
         i->next = l;
      }
   } else if ((a = xmlfindattr (x, "VAR")) && a->value)
   {                            // Include a variable directly
      value = getvar (a->value, NULL);
      if (value)
      {
         value = strdup (value);
         // Not freed as used as part of the parsed strings
         xmltoken *i = xmlparse ((char *) value, a->value);
         if (i)
         {                      // included
            xmltoken *l = x->next;
            x->next = i;
            while (i && i->next)
               i = (i->end && i->end != i ? i->end : i->next);
            i->next = l;
         }
      }
   }
   x->attrs = 0;                // Don't re-run
   return x->next;
}

xmltoken *
doexec (xmltoken * x, process_t * state)
{
   if (!x->attrs)
      return x->next;
   info (x, "Exec (%d)", x->attrs);
   fflush (of);
   char *include = NULL;
   int tempf = -1;
   char template[] = "/tmp/xmlsqlXXXXXX";
   if (x->attrs && !x->attr[0].value && !strcasecmp (x->attr[0].attribute, "include"))
      tempf = mkstemp (include = template);
   int pid = fork ();
   if (pid < 0)
      return x->next;
   if (pid)
   {
      if (include)
         close (tempf);
      waitpid (pid, NULL, 0);
   } else
   {                            // child
      char *args[x->attrs + 1];
      int arg = 0;
      for (int n = (include ? 1 : 0); n < x->attrs; n++)
      {
         args[arg] = NULL;
         size_t len;
         FILE *out = open_memstream (&args[arg], &len);
         xmlattr *a = &x->attr[n];
         char *v = a->attribute;
         if (v)
         {
            if (!strcasecmp (v, arg ? "arg" : "cmd"))
               v = a->value;
            if (v)
            {
               char temp[MAXTEMP];
               char *e = expand (temp, sizeof (temp), v);
               if (e)
               {
                  fprintf (out, "%s", e);
                  if (v != a->value && a->value)
                  {
                     char *v = expand (temp, sizeof (temp), a->value);
                     if (v)
                        fprintf (out, "=%s", v);
                  }
               }
            }
         }
         fclose (out);
         if (debug)
            fprintf (stderr, "Arg %d [%s]\n", arg, args[arg]);
         arg++;
      }
      args[arg] = NULL;
      if (include)
         dup2 (tempf, fileno (stdout));
      else
         dup2 (fileno (of), fileno (stdout));
      close (fileno (stdin));
      execvp (args[0], args);
      exit (0);
   }
   if (include)
   {
      x = doinclude (x, state, include);
      unlink (include);
      return x;
   }
   return x->next;
}

xmltoken *
processxml (xmltoken * x, xmltoken * e, process_t * state)
{
   while (x && x != e && !feof (of))
   {
      if (debug)
         fflush (of);
      if (x->type & XML_START)
      {
         if (!strcasecmp (x->content, "OUTPUT") || !strcasecmp (x->content, "xmlsql:OUTPUT"))
         {
            x = dooutput (x, state);
            continue;
         }
         if (!strcasecmp (x->content, "IF") || !strcasecmp (x->content, "xmlsql:IF") || !strcasecmp (x->content, "WHILE")
             || !strcasecmp (x->content, "xmlsql:WHILE"))
         {
            x = doif (x, state);
            continue;
         }
         if (!strcasecmp (x->content, "LATER") || !strcasecmp (x->content, "xmlsql:LATER"))
         {
            x = dolater (x, state);
            continue;
         }
         if (!strcasecmp (x->content, "FOR") || !strcasecmp (x->content, "xmlsql:FOR"))
         {
            x = dofor (x, state);
            continue;
         }
         if (!strcasecmp (x->content, "DIR") || !strcasecmp (x->content, "xmlsql:DIR"))
         {
            x = dodir (x, state);
            continue;
         }
         if (!strcasecmp (x->content, "SET") || !strcasecmp (x->content, "xmlsql:SET"))
         {
            x = doset (x, state);
            continue;
         }
         if (!strcasecmp (x->content, "EVAL") || !strcasecmp (x->content, "xmlsql:EVAL"))
         {
            x = doeval (x, state);
            continue;
         }
         if (!strcasecmp (x->content, "SQL") || !strcasecmp (x->content, "xmlsql:SQL"))
         {
            x = dosql (x, state);
            continue;
         }
         if (!strcasecmp (x->content, "INCLUDE") || !strcasecmp (x->content, "xmlsql:INCLUDE"))
         {
            x = doinclude (x, state, NULL);
            continue;
         }
         if ((!strcasecmp (x->content, "EXEC") || !strcasecmp (x->content, "xmlsql:EXEC")))
         {
            if (!allowexec)
               errx (1, "Use of <exec.../> without --exec");
            x = doexec (x, state);
            continue;
         }
         if (!strcasecmp (x->content, "INPUT"))
         {
            x = doinput (x, state);
            continue;
         }
         if (!strcasecmp (x->content, "SELECT"))
         {
            x = doselect (x, state);
            continue;
         }
         if (!strcasecmp (x->content, "OPTION"))
         {
            x = dooption (x, state);
            continue;
         }
         if (!strcasecmp (x->content, "TEXTAREA"))
         {
            x = dotextarea (x, state);
            continue;
         }
         if (!strcasecmp (x->content, "FORM"))
         {
            x = doform (x, state);
            continue;
         }
         if (!strcasecmp (x->content, "SCRIPT"))
         {
            x = doscript (x, state);
            continue;
         }
         if (!strcasecmp (x->content, "IMG"))
         {
            x = doimg (x, state);
            continue;
         }
      } else if ((x->type & XML_END) && security && !strcasecmp (x->content, "FORM"))
         fprintf (of, "<input type='hidden' name='" QUOTE (SECURITYTAG) "' value='%s'>", security);    // Security as last input item in any form
      if ((comment || !(x->type & XML_COMMENT)) && (!noform || !state || !state->selectvalue || state->selectedoption))
         tagwrite (of, x, (void *) 0);
      x = x->next;
   }
   fflush (of);
   return x;
}

xmltoken *
loadfile (char *fn)
{
   if (!fn || !*fn)
   {
      warn ("Empty file included in input list, ignored");
      return NULL;
   }
   unsigned char *buf = 0;
   xmltoken *n;
   unsigned long all = 0;
   unsigned long pos = 0;
   int f = -1;
   unsigned long len = 0;
   char *fntag = fn;
   if (!fn || !strcmp (fn, "-"))
   {
      f = fileno (stdin);
      fntag = getenv ("SCRIPT_NAME") ? : fntag;
   } else
   {
      f = open (fn, O_RDONLY);
      if (f < 0)
      {
         warn ("Loading file [%s]", fn);
         return NULL;
      }
   }
   {
      char *r = strrchr (fntag, '/');
      if (r)
         fntag = r + 1;
   }
   {                            // size
      struct stat s;
      if (!fstat (f, &s))
         len = s.st_size;
   }
   if (debug && len)
      fprintf (stderr, "Loading %s: %lu bytes\n", fntag, len);
   if (len)
   {
      buf = malloc ((all = len + 2));
      if (!buf)
         errx (1, "Malloc %lu", all);
   }
   while (1)
   {
      long l;
      if (pos + 1 >= all)
      {
         all += 10000;
         buf = realloc (buf, all);
         if (!buf)
            errx (1, "Malloc %lu", all);
      }
      l = read (f, buf + pos, all - pos - 1);
      if (l < 0)
         err (1, "Reading file [%s]", fntag);
      if (l == 0)
         break;
      pos += l;
   }
   buf = realloc (buf, pos + 1);
   if (!buf)
      errx (1, "malloc at line %d", __LINE__);
   buf[pos] = 0;
   if (debug && !len)
      fprintf (stderr, "Loaded %s: %lu bytes\n", fntag, pos);
   if (f != fileno (stdin))
      close (f);
   n = xmlparse ((char *) buf, fntag);
   if (!n)
      warnx ("Cannot parse %s\n", fn);
   xmlendmatch (n, "IF\tSQL\tWHILE\tFOR\tTEXTAREA\tSELECT\tLATER\tXMLSQL\tFORM\tDIR");
   return n;
}

int
main (int argc, const char *argv[])
{
   sd_max = 10000;
   xmltoken *x = 0;
   int c;
   char *infile = 0;
   char *outfile = 0;
   char *test = 0;
   int contenttype = 0;
   poptContext optCon;          // context for parsing command-line options
   const struct poptOption optionsTable[] = {
      {"sql-conf", 0, POPT_ARGFLAG_SHOW_DEFAULT | POPT_ARG_STRING, &sqlconf, 0, "Client config file ($SQL_CNF_FILE)", "filename"},
      {"sql-host", 'h', POPT_ARG_STRING, &sqlhost, 0, "SQL server host", "hostname/ip"},
      {"sql-port", 0, POPT_ARG_INT, &sqlport, 0, "SQL server port", "port"},
      {"sql-user", 'u', POPT_ARG_STRING, &sqluser, 0, "SQL username", "username"},
      {"sql-pass", 'p', POPT_ARG_STRING, &sqlpass, 0, "SQL password", "password"},
      {"sql-database", 'd', POPT_ARG_STRING, &sqldatabase, 0, "SQL database", "database"},
      {"in-file", 'i', POPT_ARG_STRING, &infile, 0, "Source file", "filename"},
      {"out-file", 'o', POPT_ARG_STRING, &outfile, 0, "Target file", "filename"},
      {"test", 0, POPT_ARG_STRING, &test, 0, "Test in-line", "script-text"},
      {"comment", 'c', POPT_ARG_NONE, &comment, 0, "Add comments"},
      {"content-type", 'C', POPT_ARG_NONE, &contenttype, 0, "Add content type text/html"},
      {"smiley", 0, POPT_ARG_STRING, &smileydir, 0, "Relative dir for smilies (used for markup format output)"},
      {"safe", 0, POPT_ARG_NONE, &safe, 0, "Restrict some operations such as file in textarea"},
      {"xml", 0, POPT_ARG_NONE, &isxml, 0, "Force extra escaping for xml output"},
      {"exec", 0, POPT_ARG_NONE, &allowexec, 0, "Allow <exec cmd='...' arg='...' arg='...' .../>"},
      {"no-form", 'f', POPT_ARG_NONE, &noform, 0, "Remove forms and change inputs to text"},
      {"security", 0, POPT_ARG_STRING, &security, 0, "Add hidden field to forms", "value"},
      {"show-hidden", 's', POPT_ARG_NONE, &showhidden, 0, "Remove type=hidden in input"},
      {"dataurifold", 0, POPT_ARG_INT, &dataurifold, 0, "fold datauri (70 is good for qprint)"},
      {"max-input-size", 'm', POPT_ARG_INT, &maxinputsize, 0,
       "When setting size from database field, limit to this max (0=dont set)"},
      {"debug", 'v', POPT_ARG_NONE, &debug, 0, "Debug"},        //
      POPT_AUTOHELP {NULL, 0, 0, NULL, 0}
   };
   if (getenv ("XMLSQLDEBUG"))
      comment = debug = 2;

   optCon = poptGetContext (NULL, argc, argv, optionsTable, 0);
   poptSetOtherOptionHelp (optCon, "<files>");

   if (sqldatabase && *sqldatabase == '$')
      sqldatabase = getenv (sqldatabase + 1);

   /* Now do options processing, get portname */
   if ((c = poptGetNextOpt (optCon)) < -1)
   {
      /* an error occurred during option processing */
      fprintf (stderr, "%s: %s\n", poptBadOption (optCon, POPT_BADOPTION_NOALIAS), poptStrerror (c));
      return 1;
   }
   if (!sqlconf)
      sqlconf = getenv ("SQL_CNF_FILE");

   if (contenttype)
      printf ("Content-Type: text/html\r\n\r\n");

   if (!security)
      security = getenv (QUOTE (SECURITYTAG));  // default

   if (smileydir)
   {                            // scan for smilies
      struct dirent *e;
      DIR *d = opendir (smileydir);
      if (!d)
         err (1, "Smiley dir [%s]", smileydir);
      while ((e = readdir (d)))
      {
         char *dot = strrchr (e->d_name, '.');
         if (dot && dot >= e->d_name + 2
             && (!strcasecmp (dot, ".gif") || !strcasecmp (dot, ".png") || !strcasecmp (dot, ".jpg") || !strcasecmp (dot, ".jpeg")
                 || !strcasecmp (dot, ".bmp") || !strcasecmp (dot, ".svg")))
         {
            int l = strlen (e->d_name);
            smiley_t *s = malloc (sizeof (smiley_t) + l + 1);
            if (!s)
               errx (1, "Malloc");
            s->base = dot - e->d_name;
            memmove (s->file, e->d_name, l + 1);
            s->next = smiley;
            smiley = s;
         }
      }
      closedir (d);
   }

   if (infile && poptPeekArg (optCon))
   {
      poptPrintUsage (optCon, stderr, 0);
      return 2;
   }
   if (!outfile || !strcmp (outfile, "-"))
      of = stdout;
   else
      of = fopen (outfile, "w");
   if (!of)
      err (1, "Opening output [%s]", outfile ? : "-");

   if (!infile && !poptPeekArg (optCon) && !test)
      infile = "-";             // stdin by default

   alarm (600);                 // be careful!

   if (test)
   {
      x = xmlparse ((char *) test, "test");
      if (!x)
         warnx ("Cannot parse test\n");
      xmlendmatch (x, "IF\tSQL\tWHILE\tFOR\tTEXTAREA\tSELECT\tLATER\tXMLSQL\tFORM\tDIR");
   }

   while (1)
   {                            // Load file(s)
      char *fn = infile;
      if (!fn)
         fn = (char *) poptGetArg (optCon);
      if (!fn)
         break;                 // end of files
      xmltoken *n = loadfile (fn);
      if (!x)
         x = n;
      else if (n)
      {                         // append
         xmltoken *e = x;
         while (e->next)
            e = e->end ? : e->next;
         if (e)
            e->next = n;
      }
      if (infile)
         break;                 // have done the one explicitly specified file
   }
   // Process file
   processxml (x, 0, 0);
   if (sqlconnected)
      sql_close (&sql);
   return 0;
}
