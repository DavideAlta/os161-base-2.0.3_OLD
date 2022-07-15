/*
 * mytest.c
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <err.h>

int
main(void)
{
    
	//static char writebuf[41] = "Twiddle dee dee, Twiddle dum dum.......\n";
	//static char readbuf[41];

	//const char *file;
	//int fd, rv;
    


    printf("Inizio file mytest\n");
    return 0;

	/*if (argc == 0) {
		warnx("No arguments - running on \"testfile\"");
		file = "testfile";
	}
	else if (argc == 2) {
		file = argv[1];
        printf("file name : %s\n",file);
	}
	else {
		errx(1, "Usage: filetest <filename>");
	}*/

    /*printf("Pre open\n");

	fd = open("test.txt", O_WRONLY, 0664);
	if (fd<0) {
		err(1, "%s: open for write", "test.txt");
	}

    printf("Post open\n");

    printf("Pre write\n");*/

	/*rv = write(fd, writebuf, 40);
	if (rv<0) {
		err(1, "%s: write", "test.txt");
	}*/

    //printf("Post write\n");

	//rv = close(fd);
	/*if (rv<0) {
		err(1, "%s: close (1st time)", "test.txt");
	}*/

	/*fd = open(file, O_RDONLY);
	if (fd<0) {
		err(1, "%s: open for read", file);
	}

	rv = read(fd, readbuf, 40);
	if (rv<0) {
		err(1, "%s: read", file);
	}
	rv = close(fd);
	if (rv<0) {
		err(1, "%s: close (2nd time)", file);
	}*/
	/* ensure null termination */
	/*readbuf[40] = 0;

	if (strcmp(readbuf, writebuf)) {
		errx(1, "Buffer data mismatch!");
	}

	rv = remove(file);
	if (rv<0) {
		err(1, "%s: remove", file);
	}
	printf("Passed filetest.\n");*/
	//return 0;
}
