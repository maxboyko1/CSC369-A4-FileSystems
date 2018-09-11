PROGS = ext2_mkdir ext2_cp ext2_ln ext2_rm ext2_rm_bonus ext2_restore ext2_restore_bonus ext2_checker

all : $(PROGS)

ext2_mkdir: ext2_mkdir.o ext2_utils.o
	gcc -Wall -g -o $@ $^

ext2_cp: ext2_cp.o ext2_utils.o
	gcc -Wall -g -o $@ $^

ext2_ln: ext2_ln.o ext2_utils.o
	gcc -Wall -g -o $@ $^

ext2_rm: ext2_rm.o ext2_utils.o
	gcc -Wall -g -o $@ $^

ext2_rm_bonus: ext2_rm_bonus.o ext2_utils.o
	gcc -Wall -g -o $@ $^

ext2_restore: ext2_restore.o ext2_utils.o
	gcc -Wall -g -o $@ $^

ext2_restore_bonus: ext2_restore_bonus.o ext2_utils.o
	gcc -Wall -g -o $@ $^

ext2_checker: ext2_checker.o ext2_utils.o
	gcc -Wall -g -o $@ $^

%.o: %.c ext2.h
	gcc -Wall -c $<

clean : 
	rm -f $(PROGS) *.o