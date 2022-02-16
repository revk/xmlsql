// SQL variable expansion library
// This tool is specifically to allow $variable expansion in an SQL query in a safe way with correct quoting.
// Copyright ©2022 Andrews & Arnold Ltd, Adrian Kennard
// This software is provided under the terms of the GPL - see LICENSE file for more details

typedef char *sqlexpandgetvar_t(const char *);

#define	SQLEXPANDSTDIN		1	// Handle $- as stdin
#define	SQLEXPANDFILE		2	// Handle $@ file
#define	SQLEXPANDUNSAFE		4	// Handle $% unsafe expansion
#define	SQLEXPANDPPID 		8	// Handle $$ as parent pid
#define	SQLEXPANDZERO		16	// Handle missing unquoted expansion as a number (0)
#define	SQLEXPANDBLANK		32	// Handle missing expansion as a blank (no error or warning)

// If success, returns malloced query string, and sets *errp to NULL
// If success but warning, returns malloced query string, and sets *errp to warning text 
// If failure, returns NULL, and sets *errp to error text
// If $?name is used and variable does not exist return is NULL and error set to NULL if expansion of name fails
char *sqlexpand(const char *query,sqlexpandgetvar_t *getvar,const char **errp,unsigned int flags);
