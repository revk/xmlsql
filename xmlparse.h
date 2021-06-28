// Copyright (c) 2003 Adrian Kennard Andrews & Arnold Ltd
// This software is provided under the terms of the GPL v2 or later.
// This software is provided free of charge with a full "Money back" guarantee.
// Use entirely at your own risk. We accept no liability. If you don't like that - don't use it.

typedef struct xmlattr_s
{
   char *attribute;             // pointer to attribute name, or null for deleted attribute
   char *value;                 // pointer to content, or null if just a word without =value
} xmlattr;

typedef struct xmltoken_s
{
   struct xmltoken_s *next;     // next token in file
   char *content;               // tag name for tags, or text start for text
   unsigned int type:8;
   unsigned int utf8:1;         // content is UTF8 encoded and needs escaping on output
   unsigned int attrs;          // number of attributes
   unsigned int styles;         // number of styles
   struct xmltoken_s *start;    // pointer to start token if this is end, or null if not found
   struct xmltoken_s *end;      // pointer to end token if this is start, or null if not found
   xmlattr *style;              // points to style array if xmlstyle has been called, else 0
   char *filename;              // filename
   int line;                    // line number
   int level;			// indent level
   xmlattr *attr;               // extends to number of attributes
} xmltoken;

extern char XMLATTREMOVE[];     // used in xmlwrite as attribute meaning remove it

#define XML_TEXT	0
#define	XML_START	1       // bit flag, valid for start and end in same token <TAG ... />
#define	XML_END	2               // bit flag, valid for start and end in same token <TAG ... />
#define	XML_COMMENT	4       // bit flag, valid on it's own


xmltoken *xmlparse (char *xml, char *filename); // parse XML and return token list - writes to and references memory image of source
void xmlfree (xmltoken * token);        // free token list
void xmlwrite (FILE *, xmltoken *, ...);        // write token to file, optional attr,value pairs to override attributes, null attr terminated. if token null, next is tag iiteral name
void xmlwriteattr (FILE *, char *, char *);     // write attribute as part of a tag
xmlattr *xmlfindattrbp (xmltoken *, char *, char **);   // find an attribute, null for not found, stop at breakpoint
#define xmlfindattr(x,t) xmlfindattrbp(x,t,0)
void xmldeescape (char *i);     // in situ de-escape of common xml & encoding
void xmlstyle (xmltoken * t);   // expand style attribute if present
void xmlstyleall (xmltoken * t);        // expand style on whole token chain
void xmlutf8 (char *c);         // in situ expact &xxx; to UTF8
void xmlutf8all (xmltoken * t); // expand all text in token chain
char *xmlloadfile (char *fn, size_t * len);     // load a file, if fn 0 then stdin, in to malloced memory - sets length if not null
void xmlendmatch (xmltoken * t, const char *tags);    // fill in ->end fields
void xmlutf8out (FILE *, unsigned char *);      // write UTF8 string as html with escapes
