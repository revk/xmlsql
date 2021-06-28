/*
punycode.c from RFC 3492
http://www.nicemice.net/idn/
Adam M. Costello
http://www.nicemice.net/amc/

This is ANSI C code (C89) implementing Punycode (RFC 3492).

*/

#include <string.h>
#include "punycode.h"

/*** Bootstring parameters for Punycode ***/

enum
{ base = 36, tmin = 1, tmax = 26, skew = 38, damp = 700,
   initial_bias = 72, initial_n = 0x80, delimiter = 0x2D
};

/* basic(cp) tests whether cp is a basic code point: */
#define basic(cp) ((punycode_uint)(cp) < 0x80)

/* delim(cp) tests whether cp is a delimiter: */
#define delim(cp) ((cp) == delimiter)

/* decode_digit(cp) returns the numeric value of a basic code */
/* point (for use in representing integers) in the range 0 to */
/* base-1, or base if cp is does not represent a value.       */

static punycode_uint
decode_digit (punycode_uint cp)
{
   return cp - 48 < 10 ? cp - 22 : cp - 65 < 26 ? cp - 65 : cp - 97 < 26 ? cp - 97 : base;
}

/* encode_digit(d,flag) returns the basic code point whose value      */
/* (when used for representing integers) is d, which needs to be in   */
/* the range 0 to base-1.  The lowercase form is used unless flag is  */
/* nonzero, in which case the uppercase form is used.  The behavior   */
/* is undefined if flag is nonzero and digit d has no uppercase form. */

static char
encode_digit (punycode_uint d, int flag)
{
   return d + 22 + 75 * (d < 26) - ((flag != 0) << 5);
   /*  0..25 map to ASCII a..z or A..Z */
   /* 26..35 map to ASCII 0..9         */
}

/* flagged(bcp) tests whether a basic code point is flagged */
/* (uppercase).  The behavior is undefined if bcp is not a  */
/* basic code point.                                        */

#define flagged(bcp) ((punycode_uint)(bcp) - 65 < 26)

/* encode_basic(bcp,flag) forces a basic code point to lowercase */
/* if flag is zero, uppercase if flag is nonzero, and returns    */
/* the resulting code point.  The code point is unchanged if it  */
/* is caseless.  The behavior is undefined if bcp is not a basic */
/* code point.                                                   */

static char
encode_basic (punycode_uint bcp, int flag)
{
   bcp -= (bcp - 97 < 26) << 5;
   return bcp + ((!flag && (bcp - 65 < 26)) << 5);
}

/*** Platform-specific constants ***/

/* maxint is the maximum value of a punycode_uint variable: */
static const punycode_uint maxint = -1;
/* Because maxint is unsigned, -1 becomes the maximum value. */

/*** Bias adaptation function ***/

static punycode_uint
adapt (punycode_uint delta, punycode_uint numpoints, int firsttime)
{
   punycode_uint k;

   delta = firsttime ? delta / damp : delta >> 1;
   /* delta >> 1 is a faster way of doing delta / 2 */
   delta += delta / numpoints;

   for (k = 0; delta > ((base - tmin) * tmax) / 2; k += base)
   {
      delta /= base - tmin;
   }

   return k + (base - tmin + 1) * delta / (delta + skew);
}

/*** Main encode function ***/

enum punycode_status
punycode_encode (punycode_uint input_length,
                 const punycode_uint input[], const unsigned char case_flags[], punycode_uint * output_length, char output[])
{
   punycode_uint n,
     delta,
     h,
     b,
     out,
     max_out,
     bias,
     j,
     m,
     q,
     k,
     t;

   /* Initialize the state: */

   n = initial_n;
   delta = out = 0;
   max_out = *output_length;
   bias = initial_bias;

   /* Handle the basic code points: */

   for (j = 0; j < input_length; ++j)
   {
      if (basic (input[j]))
      {
         if (max_out - out < 2)
            return punycode_big_output;
         output[out++] = case_flags ? encode_basic (input[j], case_flags[j]) : input[j];
      }
      /* else if (input[j] < n) return punycode_bad_input; */
      /* (not needed for Punycode with unsigned code points) */
   }

   h = b = out;

   /* h is the number of code points that have been handled, b is the  */
   /* number of basic code points, and out is the number of characters */
   /* that have been output.                                           */

   if (b > 0)
      output[out++] = delimiter;

   /* Main encoding loop: */

   while (h < input_length)
   {
      /* All non-basic code points < n have been     */
      /* handled already.  Find the next larger one: */

      for (m = maxint, j = 0; j < input_length; ++j)
      {
         /* if (basic(input[j])) continue; */
         /* (not needed for Punycode) */
         if (input[j] >= n && input[j] < m)
            m = input[j];
      }

      /* Increase delta enough to advance the decoder's    */
      /* <n,i> state to <m,0>, but guard against overflow: */

      if (m - n > (maxint - delta) / (h + 1))
         return punycode_overflow;
      delta += (m - n) * (h + 1);
      n = m;

      for (j = 0; j < input_length; ++j)
      {
         /* Punycode does not need to check whether input[j] is basic: */
         if (input[j] < n /* || basic(input[j]) */ )
         {
            if (++delta == 0)
               return punycode_overflow;
         }

         if (input[j] == n)
         {
            /* Represent delta as a generalized variable-length integer: */

            for (q = delta, k = base;; k += base)
            {
               if (out >= max_out)
                  return punycode_big_output;
               t = k <= bias /* + tmin */ ? tmin :      /* +tmin not needed */
                  k >= bias + tmax ? tmax : k - bias;
               if (q < t)
                  break;
               output[out++] = encode_digit (t + (q - t) % (base - t), 0);
               q = (q - t) / (base - t);
            }

            output[out++] = encode_digit (q, case_flags && case_flags[j]);
            bias = adapt (delta, h + 1, h == b);
            delta = 0;
            ++h;
         }
      }

      ++delta, ++n;
   }

   *output_length = out;
   return punycode_success;
}

/*** Main decode function ***/

enum punycode_status
punycode_decode (punycode_uint input_length,
                 const char input[], punycode_uint * output_length, punycode_uint output[], unsigned char case_flags[])
{
   punycode_uint n,
     out,
     i,
     max_out,
     bias,
     b,
     j,
     in,
     oldi,
     w,
     k,
     digit,
     t;

   /* Initialize the state: */

   n = initial_n;
   out = i = 0;
   max_out = *output_length;
   bias = initial_bias;

   /* Handle the basic code points:  Let b be the number of input code */
   /* points before the last delimiter, or 0 if there is none, then    */
   /* copy the first b code points to the output.                      */

   for (b = j = 0; j < input_length; ++j)
      if (delim (input[j]))
         b = j;
   if (b > max_out)
      return punycode_big_output;

   for (j = 0; j < b; ++j)
   {
      if (case_flags)
         case_flags[out] = flagged (input[j]);
      if (!basic (input[j]))
         return punycode_bad_input;
      output[out++] = input[j];
   }

   /* Main decoding loop:  Start just after the last delimiter if any  */
   /* basic code points were copied; start at the beginning otherwise. */

   for (in = b > 0 ? b + 1 : 0; in < input_length; ++out)
   {

      /* in is the index of the next character to be consumed, and */
      /* out is the number of code points in the output array.     */

      /* Decode a generalized variable-length integer into delta,  */
      /* which gets added to i.  The overflow checking is easier   */
      /* if we increase i as we go, then subtract off its starting */
      /* value at the end to obtain delta.                         */

      for (oldi = i, w = 1, k = base;; k += base)
      {
         if (in >= input_length)
            return punycode_bad_input;
         digit = decode_digit (input[in++]);
         if (digit >= base)
            return punycode_bad_input;
         if (digit > (maxint - i) / w)
            return punycode_overflow;
         i += digit * w;
         t = k <= bias /* + tmin */ ? tmin :    /* +tmin not needed */
            k >= bias + tmax ? tmax : k - bias;
         if (digit < t)
            break;
         if (w > maxint / (base - t))
            return punycode_overflow;
         w *= (base - t);
      }

      bias = adapt (i - oldi, out + 1, oldi == 0);

      /* i was supposed to wrap around from out+1 to 0,   */
      /* incrementing n each time, so we'll fix that now: */

      if (i / (out + 1) > maxint - n)
         return punycode_overflow;
      n += i / (out + 1);
      i %= (out + 1);

      /* Insert n at position i of the output: */

      /* not needed for Punycode: */
      /* if (decode_digit(n) <= base) return punycode_invalid_input; */
      if (out >= max_out)
         return punycode_big_output;

      if (case_flags)
      {
         memmove (case_flags + i + 1, case_flags + i, out - i);
         /* Case of last character determines uppercase flag: */
         case_flags[i] = flagged (input[in - 1]);
      }

      memmove (output + i + 1, output + i, (out - i) * sizeof *output);
      output[i++] = n;
   }

   *output_length = out;
   return punycode_success;
}

#ifndef LIB

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <err.h>

int
main (int argc, const char **argv)
{
   if (argc <= 1)
      errx (1, "List one or more domains to convert.");
   int a;
   for (a = 1; a < argc; a++)
   {
      unsigned char *d = (unsigned char *) argv[a];
      while (*d)
      {
         while (*d == '.')
            putchar (*d++);
         unsigned char *p;
         for (p = d; *p < 0x80 && *p != '.'; p++);
         if (*p == '.' || !*p)
         {                      // possible domain
            if (strncasecmp ((char *) d, "xn--", 4))
            {
               while (*d && *d != '.')
                  putchar (*d++);       // normal
               continue;
            }
            // decode
            punycode_uint out[64];
            punycode_uint len = sizeof (out) / sizeof (*out),
               n;
            if (punycode_decode (p - d - 4, (char *) d + 4, &len, out, NULL))
               continue;
            for (n = 0; n < len; n++)
            {
               int u = out[n];
               if (u >= 0x4000000)
               {
                  putchar (0xfC + (u >> 30));
                  putchar (0x80 + ((u >> 24) & 0x3F));
                  putchar (0x80 + ((u >> 18) & 0x3F));
                  putchar (0x80 + ((u >> 12) & 0x3F));
                  putchar (0x80 + ((u >> 6) & 0x3F));
                  putchar (0x80 + (u & 0x3F));
               } else if (u >= 0x200000)
               {
                  putchar (0xf8 + (u >> 24));
                  putchar (0x80 + ((u >> 18) & 0x3F));
                  putchar (0x80 + ((u >> 12) & 0x3F));
                  putchar (0x80 + ((u >> 6) & 0x3F));
                  putchar (0x80 + (u & 0x3F));
               } else if (u >= 0x10000)
               {
                  putchar (0xF0 + (u >> 18));
                  putchar (0x80 + ((u >> 12) & 0x3F));
                  putchar (0x80 + ((u >> 6) & 0x3F));
                  putchar (0x80 + (u & 0x3F));
               } else if (u >= 0x800)
               {
                  putchar (0xE0 + (u >> 12));
                  putchar (0x80 + ((u >> 6) & 0x3F));
                  putchar (0x80 + (u & 0x3F));
               } else if (u >= 0x80 || u == '<' || u == '>' || u == '&')
               {
                  putchar (0xC0 + (u >> 6));
                  putchar (0x80 + (u & 0x3F));
               } else
                  putchar (u);
            }

            d = p;
            continue;
         }
         for (p = d; *p && *p != '.'; p++);
         if (*p == '.' || !*p)
         {                      // possible utf-8 to convert
            punycode_uint in[64];
            char out[64];
            punycode_uint ilen = 0,
               olen,
               n;
            olen = sizeof (out) / sizeof (*out);
            for (p = d; *p && *p != '.' && ilen < sizeof (in) / sizeof (*in); p++)
            {
               unsigned long v = 0;
               if (*p >= 0xF8)
                  v = *p;       // silly
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
                  v = *p;       // silly
               in[ilen++] = v;
            }
            if (punycode_encode (ilen, in, NULL, &olen, out))
               continue;
            printf ("xn--");
            for (n = 0; n < olen; n++)
               putchar (out[n]);
            d = p;
            if (*d == '.')
               putchar (*d++);
            continue;
         }
         // unknown
         while (*d && *d != '.')
            putchar (*d++);
      }
      putchar ('\n');
   }
   return 0;
}

#endif
