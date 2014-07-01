#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

void main(int argc, char *argv[])
{
	pthread_mutex_t mut;
	pthread_cond_t cond;
	struct timespec ts;

	pthread_mutex_init(&mut, NULL);
	pthread_cond_init(&cond, NULL);
	pthread_mutex_lock(&mut);
	ts.tv_sec = 0;
	ts.tv_nsec = 0;
	printf("going to sleep\n");
	pthread_cond_timedwait(&cond, &mut, &ts);
	perror("awoken");
}
