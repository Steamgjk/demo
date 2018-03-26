#include <stdio.h>
#include <sys/time.h>
#include <stdlib.h>
int main()
{
	int a, b, c;
	a = 0;
	b = 1;
	c = 3;

	int t = 10000;
	long long L1, L2;
	struct timeval tv;
	int cnt = 0;
	L1 = tv.tv_sec * 1000 * 1000 + tv.tv_usec;
	int cap = 100;
	float arr[cap];
	int i = 0;
	for (i = 0; i < cap; i++)
	{
		arr[i] = i;
	}
	while (cnt <= t)
	{
		cnt++;
		float* tt = calloc(sizeof(float), cap);
		memcpy(tt, arr, sizeof(float) * cap);
		for (i = 0; i < cap; i++)
		{
			arr[i] = arr[i] + tt[i];
		}
		free(tt);
	}
	L2 = tv.tv_sec * 1000 * 1000 + tv.tv_usec;
	printf("%d ops  duration =  %lld  micro seconds \n", (cnt), L2 - L1);


}