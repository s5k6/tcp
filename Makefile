vpcc = gcc -std=c99 -g -Wall -Wextra -Wpedantic -Wbad-function-cast \
        -Wconversion -Wwrite-strings -Wstrict-prototypes -Wshadow \
	-Werror

targets = tcp

.PHONY : all clean

all : $(targets)

clean :
	rm -f *.o *.inc $(targets)

tcp : tcp.o
	gcc -o $@ $^
	strip tcp

tcp.o : help.inc

%.inc : %.txt
	sed -E 's/^/"/;s/$$/\\n"/' $< > $@

%.o : %.c
	$(vpcc) -c -o $@ $<
