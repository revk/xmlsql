

ifneq ($(wildcard /usr/bin/mysql_config),)
SQLINC=$(shell mysql_config --include)
SQLLIB=$(shell mysql_config --libs)
SQLVER=$(shell mysql_config --version | sed 'sx\..*xx')
endif
ifneq ($(wildcard /usr/bin/mariadb_config),)
SQLINC=$(shell mariadb_config --include)
SQLLIB=$(shell mariadb_config --libs)
SQLVER=$(shell mariadb_config --version | sed 'sx\..*xx')
endif

xmlsql: xmlsql.c xmlparse.o punycode.o SQLlib/sqllib.o stringdecimal/stringdecimaleval.o Makefile
	cc -O -o $@ $< xmlparse.o punycode.o SQLlib/sqllib.o -lpopt -lcrypto stringdecimal/stringdecimaleval.o -ISQLlib -Istringdecimal ${SQLINC} ${SQLLIB}

update:
	git submodule update --init --remote --recursive
	git commit -a -m "Library update"
	git push
	make -C SQLlib
	make -C stringdecimal

xmlparse.o: xmlparse.c Makefile
	cc -c -o $@ $< -DLIB

punycode.o: punycode.c Makefile
	cc -c -o $@ $< -DLIB
