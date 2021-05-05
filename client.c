#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>

#define FIB_DEV "/dev/fibonacci"
#define CLOCK_ID CLOCK_MONOTONIC_RAW
#define ONE_SEC 1e9
#define FIB_KTIME "/sys/kernel/fibdrv/time"

long long get_ktime(){
    long long t = -1;
    FILE *kptr = fopen(FIB_KTIME, "r");
    fscanf(kptr, "%lld", &t);
    fclose(kptr);
    return t;
}

int main()
{
    long long sz;
    FILE *fptr;

    char buf[1];
    char write_buf[] = "testing writing";
    int offset = 100; /* TODO: try test something bigger than the limit */

    struct timespec start = {0, 0};
    struct timespec end = {0, 0};

    fptr = fopen("data.txt","w");
    if(fptr == NULL){
        printf("Failed to open log data.");
        exit(1);
    }

    int fd = open(FIB_DEV, O_RDWR);
    if (fd < 0) {
        perror("Failed to open character device");
        exit(1);
    }

    for (int i = 0; i <= offset; i++) {
        sz = write(fd, write_buf, strlen(write_buf));
        printf("Writing to " FIB_DEV ", returned the sequence %lld\n", sz);
    }

    for (int i = 0; i <= offset; i++) {
        lseek(fd, i, SEEK_SET);
        clock_gettime(CLOCK_ID, &start);
        sz = read(fd, buf, 1);
        clock_gettime(CLOCK_ID, &end);
        long long utime = (double)(end.tv_sec - start.tv_sec) * ONE_SEC + (end.tv_nsec - start.tv_nsec);
        long long ktime = get_ktime();
        fprintf(fptr, "%d %lld %lld\n", i, utime, ktime);
        printf("Reading from " FIB_DEV
               " at offset %d, returned the sequence "
               "%lld.\n",
               i, sz);
    }

    for (int i = offset; i >= 0; i--) {
        lseek(fd, i, SEEK_SET);
        sz = read(fd, buf, 1);
        printf("Reading from " FIB_DEV
               " at offset %d, returned the sequence "
               "%lld.\n",
               i, sz);
    }

    fclose(fptr);
    close(fd);
    return 0;
}
