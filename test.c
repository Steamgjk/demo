#include <stdio.h>
#include <sys/time.h>
int main()
{
	int a, b, c;
	a = 0;
	b = 1;
	c = 3;
	int t = 10000;
	long long L1, L2;
	struct timeval tv;
	L1 = tv.tv_sec * 1000 * 1000 + tv.tv_usec;
	while (a < t)
	{
		a = a + b;
	}
	L2 = tv.tv_sec * 1000 * 1000 + tv.tv_usec;
	printf("%d ops  duration =  %lld  micro seconds \n", (a), L2 - L1);


}