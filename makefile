
comp = gcc
comp_flaps = -g -Wall
comp_libs = -lm

paging : demandpage.c
	$(comp) $(comp_flags) demandpage.c -pthread -o paging $(comp_libs)

clean :
	rm -f *.o paging core
