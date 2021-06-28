xmlsql: xmlsql.c xmlparse.o punycode.o SQLlib/sqllib.o stringdecimal/stringdecimaleval.o
	cc -O -o $@ $< xmlparse.o punycode.o SQLlib/sqllib.o -lpopt -lcrypto stringdecimal/stringdecimaleval.o

update:
	git submodule update --init --remote --recursive
	git commit -a -m "Library update"
	git push
	make -C SQLlib
	make -C stringdecimal

xmlparse.o: xmlparse.c
	cc -c -o $@ $<

punycode.o: punycode.c
	cc -c -o $@ $<
