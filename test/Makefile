CFLAGS = -std=c11 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -I../main

test: frame_test
	./frame_test

frame_test: frame_test.c ../main/frame.h ../main/name_rules.h ../main/patterns.c ../main/patterns.h
	$(CC) $(CFLAGS) -o $@ frame_test.c ../main/patterns.c

clean:
	rm -f frame_test

.PHONY: test clean
