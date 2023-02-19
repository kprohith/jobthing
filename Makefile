all: jobthing
jobthing: jobthing.c
	gcc -pedantic -g -Wall -std=gnu99 -I/local/courses/csse2310/include -L/local/courses/csse2310/lib -lcsse2310a3 -o $@ $<
clean:
	rm jobthing
