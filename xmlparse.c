// Copyright (c) 2003 Adrian Kennard Andrews & Arnold Ltd
// This software is provided under the terms of the GPL v2 or later.
// This software is provided free of charge with a full "Money back" guarantee.
// Use entirely at your own risk. We accept no liability. If you don't like that - don't use it.
// Parse XML
// library functions to parse XML source, and to produce XML from parsed source
// Using expat may be better for real XML

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <err.h>
#include "xmlparse.h"

#define MAXATTR 255             // max attributes in any token
#define MAXLEVEL 1000           // nesting depth

char XMLATTREMOVE[] = "";

static xmltoken *gettoken(void)
{
   xmltoken *h = malloc(sizeof(*h));
   if (!h)
      errx(1, "malloc");
   memset(h, 0, sizeof(*h));
   return h;
}

xmltoken *xmlparse(char *h, char *filename)
{                               // parse XML and return token list
   xmltoken *n = 0,
       *p = (xmltoken *) & n,
       *t = NULL;
   int line = 1;

   while (*h)
   {
      if (*h == '<' && h[1] == '!' && h[2] == '-' && h[3] == '-')
      {                         // comment
         *h++ = 0;
         t = gettoken();
         t->filename = filename;
         t->line = line;
         t->type = XML_COMMENT;
         t->content = h + 3;
         while (*h && !(*h == '-' && h[1] == '-' && h[2] == '>'))
         {
            if (*h == '\n')
               line++;
            h++;
         }
         if (*h)
         {
            *h = 0;
            h += 3;
         }
      } else if (*h == '<' && ((h[1] == '/' && isalpha(h[2])) || isalpha(h[1])))
      {                         // token (note <!DOCTYPE...> is not recognised as a tag
         xmlattr a[MAXATTR];
         int n = 0;
         char *tag;
         unsigned char type = 0;
         *h++ = 0;              // close previous content
         if (*h == '/')
         {
            type = XML_END;
            h++;
         } else
            type = XML_START;
         tag = h;
         while (isalnum(*h) || *h == '-' || *h == '_' || *h == ':')
         {
            if (*h == '\n')
               line++;
            h++;
         }
         char *hwas = NULL;
         while (1)
         {                      // get attributes
            if (hwas && !*h)
            {                   // End of embedded variable
               h = hwas;
               hwas = NULL;
            }
            if (!*h)
               break;           // End of file?
            while (isspace(*h))
            {
               if (*h == '\n')
                  line++;
               *h++ = 0;
            }
#ifdef	DOLLAREXPAND
            if (*h == '$')
            {
               if (isalpha(h[1]) && strcasestr(DOLLAREXPAND, tag))
               {                // Expanded $variable as attributes - only in specified tags
                  char *s = h + 1,
                      *e = s;
                  while (isalnum(*e))
                     e++;
                  if (!*e || (*e == '/' && e[1] == '>') || *e == '>')
                  {             // Only actually allow at end
                     char *env = strndup(s, (int) (e - s));
                     char *val = getenv(env);
                     if (!val)
                        warnx("Not found $%s", env);
                     free(env);
                     h = e;     // Skip
                     if (val)
                     {
                        hwas = h;
                        h = val;
                     }
                     continue;
                  } else
                     warnx("Line %d use of $%.*s not at end of attributes", line, (int) (e - s), s);
               } else
                  warnx("Line %d use of $variable not allowed in %.20s", line, tag);
            }
#endif
            if (*h == '/' && h[1] == '>')
            {
               if (hwas)
               {
                  h = hwas;
                  hwas = NULL;
                  continue;
               }
               type |= XML_END;
               *h++ = 0;
               break;
            }
            if (*h == '>')
            {
               if (hwas)
               {
                  h = hwas;
                  hwas = NULL;
                  continue;
               }
               break;
            }
            a[n].value = 0;
            if (*h == '"' || *h == '\'')
            {
               char q = *h++;
               a[n].attribute = h;
               while (*h && *h != q && *h >= ' ')
                  h++;
               if (*h == q)
                  *h++ = 0;
            } else
            {
               a[n].attribute = h;
               while (*h && (*h != '/' || h[1] != '>') && *h != '=' && *h != '>' && !isspace(*h))
               {
                  if (*h == '\n')
                     line++;
                  h++;
               }
            }
            if (*h == '=')
            {                   // value
               char quote = 0;
               *h++ = 0;
               while (isspace(*h))
               {
                  if (*h == '\n')
                     line++;
                  h++;
               }
               if (*h == '\'' || *h == '"')
                  quote = *h++;
               a[n].value = h;
               if (quote)
               {
                  while (*h && *h != quote)
                  {
                     if (*h == '\n')
                        line++;
                     h++;
                  }
                  if (*h == quote)
                     *h++ = 0;
               } else
                  while (*h && (*h != '/' || h[1] != '>') && *h != '>' && !isspace(*h))
                  {
                     if (*h == '\n')
                        line++;
                     h++;
                  }
            }
            if (n < MAXATTR)
               n++;
            else
               fprintf(stderr, "Too many attributes for tag %s (%s:%d)\n", tag, filename, line);
         }
         if (*h == '>')
            *h++ = 0;
         t = gettoken();
         t->filename = filename;
         t->line = line;
         t->type = type;
         t->content = tag;
         if (n)
         {
            t->attrs = n;
            t->attr = malloc(n * sizeof(*a));
            if (!t->attr)
               errx(1, "Malloc");
            memmove(t->attr, a, n * sizeof(*a));
         }
         if ((type & XML_START) && !(type & XML_END) && !strcasecmp(tag, "SCRIPT"))
         {                      // special parsing of script content
            t->next = 0;
            p->next = t;
            p = t;
            t = gettoken();
            t->filename = filename;
            t->line = line;
            t->content = h;
            while (*h && strncasecmp(h, "</script>", 9))
            {
               if (*h == '\n')
                  line++;
               h++;
            }
         }
      } else
      {                         // text
         t = gettoken();
         t->filename = filename;
         t->line = line;
         t->content = h;
         while (*h)
         {
            if (*h == '<' && h[1] == '!' && h[2] == '-' && h[3] == '-')
               break;
            if (*h == '<' && ((h[1] == '/' && isalpha(h[2])) || isalpha(h[1])))
               break;
            if (*h == '\n')
               line++;
            h++;
         }
      }
      t->next = 0;
      p->next = t;
      p = t;
   }
   return n;
}

static int listmatch(const char *t, char *s)
{                               // match to one of the tab sep field names - xml name space can match or whole tag
   while (*t)
   {
      char *p = s;
      while (*p && toupper(*p) == *t)
      {
         p++;
         t++;
      }
      if ((!*p || *p == ':') && (!*t || *t == '\t' || *t == ','))
         return 1;              // match
      while (*t && *t != '\t' && *t != ',')
         t++;
      if (*t == '\t' || *t == ',')
         t++;
   }
   return 0;                    // no match
}

void xmlendmatch(xmltoken * n, const char *tags)
{
   xmltoken *h[MAXLEVEL],
   *t;
   int l = 0;
   for (t = n; t; t = t->next)
   {
      t->level = l;
      if (t->type && (!tags || listmatch(tags, t->content)))
      {
         if (t->type & XML_START)
         {                      // a start, stack
            h[l] = t;
            if (l < MAXLEVEL)
               l++;
         }
         if (t->type & XML_END)
         {                      // an end, unstack till match

            int q = l;
            while (q && strcasecmp(h[q - 1]->content, t->content))
            {
               if (!strcasecmp(h[q - 1]->content, "if"))
               {
                  q = 0;
                  break;
               }
               q--;
               if (tags)
               {                // May be a tad overkill for an error message, but this is often in apache log so avoid line splitting
                  char *e = NULL;
                  size_t elen = 0;
                  FILE *o = open_memstream(&e, &elen);
                  fprintf(o, "Force close %s for /%s (%s:%d %s:%d) ... ", h[q]->content, t->content, h[q]->filename, h[q]->line, t->filename, t->line);
                  int n = 10;
                  xmltoken *q;
                  for (q = t->next; q && n--; q = q->next)
                     if ((q->type & XML_START) && (q->type & XML_END))
                        fprintf(o, "<%s/>", q->content);
                     else if ((q->type & XML_START))
                        fprintf(o, "<%s>", q->content);
                     else if ((q->type & XML_END))
                        fprintf(o, "</%s>", q->content);
                     else
                     {          // don't show line breaks
                        char *z;
                        for (z = q->content; *z; z++)
                           if (*z >= ' ')
                              fputc(*z, o);
                     }
                  fclose(o);
                  fputs(e, stderr);
                  free(e);
               }
            }
            if (q)
            {
               l = q;
               --l;
               h[l]->end = t;
               t->start = h[l];
               t->level = l;
            }
         }
      }
   }
}

void xmlfree(xmltoken * t)
{                               // free token list
   while (t)
   {
      xmltoken *n = t->next;
      if (t->style)
         free(t->style);
      if (t->attr)
         free(t->attr);
      free(t);
      t = n;
   }
}

#if 0
static char *stolower(char *s)
{
   char *p = s;
   if (s)
      while (*s)
      {
         if (isupper(*s))
            *s = tolower(*s);
         s++;
      }
   return p;
}
#endif

int validescape(char *e)
{                               // if it a valid &xxx; or &#xxx; escale
   if (*e != '&')
      return 0;
   e++;
   if (*e == '#')
   {
      e++;
      if (!isxdigit(*e))
         return 0;
      while (isxdigit(*e))
         e++;
      if (*e != ';')
         return 0;
   } else
   {
      if (!isalpha(*e))
         return 0;
      while (isalnum(*e))
         e++;
      if (*e != ';')
         return 0;
   }
   return 1;
}

void xmlwriteattr(FILE * f, char *tag, char *value)
{
   if (tag)
   {
      fprintf(f, " %s", tag);
      if (value)
      {
         char *p;
         fputc('=', f);
         if (!*value)
         {
            fputc('"', f);
            fputc('"', f);
         } else
         {
//            for (p = value; *p && (isupper (*p) || isdigit (*p) || *p == '-'); p++);
//            if (*p)
            {
               fputc('"', f);
               for (p = value; *p; p++)
                  if (*p == '"')
                     fprintf(f, "&quot;");
                  else if (*p == '&' && !validescape(p))
                     fprintf(f, "&amp;");
                  else
                     fputc(*p, f);
               fputc('"', f);
            }
// else fprintf (f, "%s", value);        // unquoted
         }
      }
   }
}

xmlattr *xmlfindattrbp(xmltoken * t, char *tag, char **breakpoint)
{
   int a;
   if (!t || !tag)
      return 0;
   for (a = 0; a < t->attrs; a++)
      if (t->attr[a].attribute)
      {
         if (!strcasecmp(t->attr[a].attribute, tag))
            return &t->attr[a];
         if (breakpoint && !t->attr[a].value)
         {
            char **b = breakpoint;
            while (*b)
            {
               if (!strcasecmp(t->attr[a].attribute, *b))
                  break;
               b++;
            }
            if (*b)
               break;           // end of attribute scan
         }
      }
   for (a = 0; a < t->styles; a++)
      if (t->style[a].attribute)
      {
         if (!strcasecmp(t->style[a].attribute, tag))
            return &t->style[a];
      }
   return 0;
}

void xmlwrite(FILE * f, xmltoken * t, ...)
{                               // write token to file
   unsigned char type = 0;
   char *tag = 0;
   va_list a;
   va_start(a, t);
   if (!t)
   {                            // tag literal
      tag = va_arg(a, char *);
      type |= XML_START;
      if (tag && *tag == '/')
      {
         tag++;
         type |= XML_END;
      }
   } else
   {
      type = t->type;
      tag = t->content;
   }
   if (!tag)
      return;
   if (!type)
   {                            // write text
      if (!t || t->utf8)
         xmlutf8out(f, (unsigned char *) tag);
      else
         fprintf(f, "%s", tag);
   } else if (type & XML_COMMENT)
   {                            // comment
      fprintf(f, "<!--%s-->", tag);
   } else if (type & (XML_START | XML_END))
   {                            // write token
      fputc('<', f);
      if (!(type & XML_START))
         fputc('/', f);
      fprintf(f, "%s", tag);
      if (type & XML_START)
      {                         // attributes only if start type...
         while (1)
         {                      // custom attributes
            char *tag = va_arg(a, char *),
            *val;
            xmlattr *attr;
            if (!tag)
               break;
            val = va_arg(a, char *);
            if (t)
               while ((attr = xmlfindattr(t, tag)))
                  attr->attribute = 0;
            if (val != XMLATTREMOVE)
               xmlwriteattr(f, tag, val);
         }
         if (t && t->attrs)
         {                      // attributes
            int a;
            for (a = 0; a < t->attrs; a++)
               xmlwriteattr(f, t->attr[a].attribute, t->attr[a].value);
         }
         if (t && t->styles)
         {                      // styles
            int a;
            fprintf(f, " style=\"");
            for (a = 0; a < t->styles; a++)
            {
               if (a)
                  fprintf(f, ";");
               fprintf(f, "%s", t->style[a].attribute); // TODO escape
               if (t->style[a].value)
               {
                  fprintf(f, ":");
                  fprintf(f, "%s", t->style[a].value);  // TODO escape / quote
               }
            }
            fprintf(f, "\"");
         }
      }
      if (type & XML_START && type & XML_END)
      {
         fputc(' ', f);
         fputc('/', f);
      }
      fputc('>', f);
   }
   va_end(a);
}

void xmldeescape(char *i)
{
   char *o = i;
   while (*i)
   {
      if (*i == '&')
      {
         char *v;
         i++;
         v = i;
         while (*i && *i != ';')
            i++;
         if (*i == ';')
         {
            *i++ = 0;
            if (!strcasecmp(v, "lt"))
               *o++ = '<';
            else if (!strcasecmp(v, "gt"))
               *o++ = '>';
            else if (!strcasecmp(v, "amp"))
               *o++ = '&';
            else if (!strcasecmp(v, "quot"))
               *o++ = '"';
            else if (!strcasecmp(v, "apos"))
               *o++ = '\'';
            else if (!strcasecmp(v, "#x0a"))
               *o++ = '\n';
            else
            {
               *o++ = '&';
               i[-1] = ';';
               i = v;
            }
         } else
         {
            i = v;
            *o++ = '&';
         }
      } else
         *o++ = *i++;
   }
   *o = 0;
}

// See if there is a style attribute, and if so, expand it as tag=value for tag:value in the style
void xmlstyle(xmltoken * t)
{
   if (t->type & XML_START)
   {
      xmlattr *s = xmlfindattr(t, "style");
      if (s && s->value)
      {
         char *p = s->value;
         int n = 0;
         xmlattr a[MAXATTR];
         s->value = 0;          // deleted attribute
         if (t->style)
         {
            free(t->style);
            t->style = 0;
            t->styles = 0;
         }
         while (*p)
         {
            char *q = p,
                *v = 0;
            while (*q && *q != ':' && *q != ';')
               q++;
            if (*q == ':')
            {
               *q++ = 0;
               v = q;
               while (*q && *q != ';')
                  q++;
            }
            while (*q == ';')
               *q++ = 0;
            if (n >= MAXATTR)
               break;
            a[n].attribute = p;
            a[n].value = v;
            n++;
            p = q;
         }
         t->styles = n;
         if (n)
         {
            t->style = malloc(sizeof(xmlattr) * n);
            memmove(t->style, a, sizeof(xmlattr) * n);
         }
      }
   }
}

void xmlstyleall(xmltoken * t)
{
   for (; t; t = t->next)
      xmlstyle(t);
}

static struct {
   unsigned long u;
   char *t;
} utf[] = {
   {
    ' ', "sp" },
   {
    '!', "excl" },
   {
    '"', "quot" },
   {
    '#', "num" },
   {
    '$', "dollar" },
   {
    '%', "percnt" },
   {
    '&', "ampr" },
   {
    '\'', "apos" },
   {
    '(', "lpar" },
   {
    ')', "rpar" },
   {
    '*', "ast" },
   {
    '+', "plus" },
   {
    ',', "comma" },
   {
    '-', "hyphen" },
   {
    '-', "minus" },
   {
    '/', "sol" },
   {
    ':', "colon" },
   {
    ';', "semi" },
   {
    '<', "lt" },
   {
    '=', "equals" },
   {
    '>', "gt" },
   {
    '?', "quest" },
   {
    '@', "commat" },
   {
    '[', "lsqb" },
   {
    '\\', "bsol" },
   {
    ']', "rsqb" },
   {
    '^', "circ" },
   {
    '_', "lowbar" },
   {
    '_', "horbar" },
   {
    '`', "grave" },
   {
    '{', "lcub" },
   {
    '|', "verbar" },
   {
    '}', "rcub" },
   {
    '~', "tilde" },
   {
    0x82, "lsquor" },
   {
    0x83, "fnof" },
   {
    0x84, "ldquor" },
   {
    0x85, "hellip" },
   {
    0x85, "ldots" },
   {
    0x86, "dagger" },
   {
    0x87, "Dagger" },
   {
    0x89, "permil" },
   {
    0x8A, "Scaron" },
   {
    0x8B, "lsaquo" },
   {
    0x8C, "OElig" },
   {
    0x91, "lsquo" },
   {
    0x91, "rsquor" },
   {
    0x92, "rsquo" },
   {
    0x93, "ldquo" },
   {
    0x93, "rdquor" },
   {
    0x94, "rdquo" },
   {
    0x95, "bull" },
   {
    0x96, "ndash" },
   {
    0x96, "endash" },
   {
    0x97, "mdash" },
   {
    0x97, "emdash" },
   {
    0x98, "tilde" },
   {
    0x99, "trade" },
   {
    0x9A, "scaron" },
   {
    0x9B, "rsaquo" },
   {
    0x9C, "oelig" },
   {
    0x9DF, "Yuml" },
   {
    0xA0, "nbsp" },
   {
    0xA1, "iexcl" },
   {
    0xA2, "cent" },
   {
    0xA3, "pound" },
   {
    0xA4, "curren" },
   {
    0xA5, "yen" },
   {
    0xA6, "brvbar" },
   {
    0xA6, "brkbar" },
   {
    0xA7, "sect" },
   {
    0xA8, "uml" },
   {
    0xA8, "die" },
   {
    0xA9, "copy" },
   {
    0xAA, "ordf" },
   {
    0xAB, "laquo" },
   {
    0xAC, "not" },
   {
    0xAD, "shy" },
   {
    0xAE, "reg" },
   {
    0xAF, "macr" },
   {
    0xAF, "hibar" },
   {
    0xB0, "deg" },
   {
    0xB1, "plusmn" },
   {
    0xB2, "sup2" },
   {
    0xB3, "sup3" },
   {
    0xB4, "acute" },
   {
    0xB5, "micro" },
   {
    0xB6, "para" },
   {
    0xB7, "middot" },
   {
    0xB8, "cedil" },
   {
    0xB9, "sup1" },
   {
    0xBA, "ordm" },
   {
    0xBB, "raquo" },
   {
    0xBC, "frac14" },
   {
    0xBD, "frac12" },
   {
    0xBE, "frac34" },
   {
    0xBF, "iquest" },
   {
    0xC0, "Agrave" },
   {
    0xC1, "Aacute" },
   {
    0xC2, "Acirc" },
   {
    0xC3, "Atilde" },
   {
    0xC4, "Auml" },
   {
    0xC5, "Aring" },
   {
    0xC6, "AElig" },
   {
    0xC7, "Ccedil" },
   {
    0xC8, "Egrave" },
   {
    0xC9, "Eacute" },
   {
    0xCA, "Ecirc" },
   {
    0xCB, "Euml" },
   {
    0xCC, "Igrave" },
   {
    0xCD, "Iacute" },
   {
    0xCE, "Icirc" },
   {
    0xCF, "Iuml" },
   {
    0xD0, "ETH" },
   {
    0xD1, "Ntilde" },
   {
    0xD2, "Ograve" },
   {
    0xD3, "Oacute" },
   {
    0xD4, "Ocirc" },
   {
    0xD5, "Otilde" },
   {
    0xD6, "Ouml" },
   {
    0xD7, "times" },
   {
    0xD8, "Oslash" },
   {
    0xD9, "Ugrave" },
   {
    0xDA, "Uacute" },
   {
    0xDB, "Ucirc" },
   {
    0xDC, "Uuml" },
   {
    0xDD, "Yacute" },
   {
    0xDE, "THORN" },
   {
    0xDF, "szlig" },
   {
    0xE0, "agrave" },
   {
    0xE1, "aacute" },
   {
    0xE2, "acirc" },
   {
    0xE3, "atilde" },
   {
    0xE4, "auml" },
   {
    0xE5, "aring" },
   {
    0xE6, "aelig" },
   {
    0xE7, "ccedil" },
   {
    0xE8, "egrave" },
   {
    0xE9, "eacute" },
   {
    0xEA, "ecirc" },
   {
    0xEB, "euml" },
   {
    0xEC, "igrave" },
   {
    0xED, "iacute" },
   {
    0xEE, "icirc" },
   {
    0xEF, "iuml" },
   {
    0xF0, "eth" },
   {
    0xF1, "ntilde" },
   {
    0xF2, "ograve" },
   {
    0xF3, "oacute" },
   {
    0xF4, "ocirc" },
   {
    0xF5, "otilde" },
   {
    0xF6, "ouml" },
   {
    0xF7, "divide" },
   {
    0xF8, "oslash" },
   {
    0xF9, "ugrave" },
   {
    0xFA, "uacute" },
   {
    0xFB, "ucirc" },
   {
    0xFC, "uuml" },
   {
    0xFD, "yacute" },
   {
    0xFE, "thorn" },
   {
    0xFF, "yuml" },
};

void xmlutf8(char *i)
{                               // in situ page &xxx; in to UTF-8
   if (i)
   {
      char *o = i;
      while (*i)
      {
         if (*i == '&')
         {
            char *t = i + 1,
                *e = t;
            while (*e && *e != ';')
               e++;
            if (*e == ';')
            {
               unsigned long long u = 0;
               int n = e - t;
               if (*t == '#')
               {
                  t++;
                  while (isxdigit(*t))
                  {
                     u = (u << 4) + (*t & 0xF);
                     if (isalpha(*t))
                        u += 9;
                     t++;
                  }
               } else
               {
                  int p;
                  for (p = 0; p < sizeof(utf) / sizeof(*utf); p++)
                     if (strlen(utf[p].t) == n && !strncmp(t, utf[p].t, n))
                     {
                        u = utf[p].u;
                        break;
                     }
               }
               if (u)
               {                // UTF encode
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
                  i = e + 1;
               } else
                  *o++ = *i++;
            } else
               *o++ = *i++;
         } else
            *o++ = *i++;
      }
      *o = 0;
   }
}

void xmlutf8all(xmltoken * t)
{
   for (; t; t = t->next)
      if (!t->type)
      {
         xmlutf8(t->content);
         t->utf8 = 1;
      }
}

void xmlutf8out(FILE * f, unsigned char *p)
{                               // write UTF encoded content
   while (*p)
   {
      if (*p < 0x20)
         fprintf(f, "&#x%X;", *p);
      else if (*p == '&')
         fprintf(f, "&amp;");
      else if (*p == '<')
         fprintf(f, "&lt;");
      else if (*p == '>')
         fprintf(f, "&gt;");
      else if (*p >= 0x80)
      {                         // UTF8
         unsigned long v = 0;
         if (*p >= 0xF8)
            v = *p;             // silly
         if (*p >= 0xF0 && p[1] >= 0x80 && p[1] < 0xC0 && p[2] >= 0x80 && p[2] < 0xC0 && p[3] >= 0x80 && p[3] < 0xC0)
         {
            v = (((p[0] & 0x07) << 18) | ((p[1] & 0x3f) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3f));
            p += 3;
         } else if (*p >= 0xE0 && p[1] >= 0x80 && p[1] < 0xC0 && p[2] >= 0x80 && p[2] < 0xC0)
         {
            v = (((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F));
            p += 2;
         } else if (*p >= 0xC0 && p[1] >= 0x80 && p[1] < 0xC0)
         {
            v = (((p[0] & 0x1F) << 6) | (p[1] & 0x3F));
            p += 1;
         } else
            v = *p;             // silly
         int p;
         for (p = 0; p < sizeof(utf) / sizeof(*utf) && utf[p].u != v; p++);
         if (p < sizeof(utf) / sizeof(*utf))
            fprintf(f, "&%s;", utf[p].t);
         else
            fprintf(f, "&#x%lX;", v);
      } else
         fputc(*p, f);
      p++;
   }
}

char *xmlloadfile(char *fn, size_t *len)
{                               // load a file
   size_t l = 0,
       a = 0,
       p = 0,
       r = 0;
   char *m;
   struct stat s;
   FILE *f = (fn ? fopen(fn, "r") : stdin);
   if (!f)
      return 0;                 // not opened
   if (!fstat(fileno(f), &s))
      l = s.st_size;
   a = 10240;
   if (l)
      a = l;
   m = malloc(a + 1);
   if (!m)
   {
      if (fn)
         fclose(f);
      return 0;                 // no memory
   }
   while (1)
   {
      r = fread(m + p, 1, a - p, f);
      if (!r)
         break;
      if (r < 0)
      {
         if (fn)
            fclose(f);
         free(m);
         return 0;
      }
      p += r;
      if (l && p == l)
         break;                 // end of file
      if (p == a)
      {
         a += 10240;
         m = realloc(m, a + 1);
         if (!m)
         {
            if (fn)
               fclose(f);
            return 0;
         }
      }
   }
   m = realloc(m, p + 1);
   if (!m)
   {
      if (fn)
         fclose(f);
      return 0;
   }
   m[p] = 0;                    // termination byte.
   if (len)
      *len = p;
   if (fn)
      fclose(f);
   return m;
}


#ifndef LIB
int main(int argc, char *argv[])
{
   int a;
   for (a = 1; a < argc; a++)
   {
      char *m = xmlloadfile(strcmp(argv[a], "-") ? argv[a] : 0, 0);
      if (m)
      {
         xmltoken *t = xmlparse(m, argv[a]);
         xmlendmatch(t, 0);
         {
            xmltoken *n = t;
            while (n)
            {
               if (n->type)
               {
                  printf("%p %p", n, n->end);
                  xmlwrite(stdout, n, "test", "123", 0);
                  fputc('\n', stdout);
               }
               n = n->next;
            }
         }
         xmlfree(t);
         free(m);
      } else
         perror(argv[a]);
   }
   xmlwrite(stdout, 0, "test", "a", "B&C", (char *) 0);
   xmlwrite(stdout, 0, "/test", "a", "B&C", "B", (char *) 0, (char *) 0);
   xmlutf8out(stdout, (unsigned char *) "test&test£abc");
   return 0;
}
#endif
