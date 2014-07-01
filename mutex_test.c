#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>

void
timeoutComp(struct timespec *pt, long iTimeout)
{
        clock_gettime(CLOCK_REALTIME, pt);
        printf("OS returns:  %lld.%lld\n", pt->tv_sec, pt->tv_nsec);
        pt->tv_sec += iTimeout / 1000;
        pt->tv_nsec += (iTimeout % 1000) * 1000000; /* think INTEGER arithmetic! */
        if(pt->tv_nsec > 999999999) { /* overrun? */
                pt->tv_nsec -= 1000000000;
                ++pt->tv_sec;
        }
}

void main(int argc, char *argv[])
{
        pthread_mutex_t mut;
        pthread_cond_t cond;
        struct timespec ts;
        int r;
        struct timeval tv;

        memset(&ts, 0, sizeof(ts));
        pthread_mutex_init(&mut, NULL);
        pthread_cond_init(&cond, NULL);
        pthread_mutex_lock(&mut);
        gettimeofday(&tv, NULL);
        timeoutComp(&ts, 2000);
        ts.tv_sec = 100000000; /* with 7 digits, it still works, with 8+ it hangs... */
        printf("system time: %lld.%lld\n", tv.tv_sec, tv.tv_usec);
        printf("going to sleep til %lld.%lld\n", ts.tv_sec, ts.tv_nsec);
        r = pthread_cond_timedwait(&cond, &mut, &ts);
        perror("awoken");
        printf("cond_wait return code %d\n", r);
}
