# SPDX-License-Identifier: GPL-3.0-or-later
CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: czet-lsp

czet-lsp: czet-lsp.c
	$(CC) $(CFLAGS) -o $@ $<

# Con el propio driver CZet (también vale: es C válido):
czet: czet-lsp.c
	czet -static czet-lsp.c -o czet-lsp

check: czet-lsp
	./czet-lsp --help
	@echo "-- smoke: compila ejemplo --"
	echo 'int main(void){return 0;}' > /tmp/smoke.ct
	printf 'Content-Length: 168\r\n\r\n{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"capabilities":{}}}' | timeout 3 ./czet-lsp --no-czet | head -c 400; echo

clean:
	rm -f czet-lsp

.PHONY: all czet check clean
