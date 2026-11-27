CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11

SRCS = src/main.c src/lexer.c src/parser.c src/typecheck.c src/codegen.c
OBJS = $(SRCS:.c=.o)

all: kcc

kcc: self/kcc.k self/ast.kl self/lexer.kl self/parser.kl self/typecheck.kl self/codegen.kl
	@if [ -x ./kcc ]; then \
		./kcc -ffocus self/kcc.k -bo kcc.tmp && mv kcc.tmp kcc; \
	else \
		$(MAKE) kcc-c && ./kcc-c -ffocus self/kcc.k -bo kcc; \
	fi

kcc-c: $(OBJS)
	$(CC) $(CFLAGS) -o kcc-c $(OBJS)

%.o: %.c src/kcc.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) kcc kcc-c kcc-self *.s a.out

.PHONY: all clean
